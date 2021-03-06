#include <netinet/tcp.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

#include "config.h"

#include "Connection.h"
#include "distributions.h"
#include "Generator.h"
#include "mutilate.h"
#include "binary_protocol.h"
#include "util.h"

Connection::Connection(struct event_base* _base, struct evdns_base* _evdns,
                       string _hostname, string _port, options_t _options,
                       bool sampling) :
  hostname(_hostname), port(_port), start_time(0),
  stats(sampling), options(_options), base(_base), evdns(_evdns)
{
  valuesize = createGenerator(options.valuesize);
  keysize = createGenerator(options.keysize);
  keygen = new KeyGenerator(keysize, options.records);

  if (options.lambda <= 0) {
    iagen = createGenerator("0");
  } else {
    D("iagen = createGenerator(%s)", options.ia);
    iagen = createGenerator(options.ia);
    iagen->set_lambda(options.lambda);
  }

  read_state = INIT_READ;
  write_state = INIT_WRITE;

  last_tx = last_rx = 0.0;

  if (!options.udp) {
    bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, bev_read_cb, bev_write_cb, bev_event_cb, this);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect_hostname(bev, evdns, AF_UNSPEC,
                                          hostname.c_str(),
                                          atoi(port.c_str())))
      DIE("bufferevent_socket_connect_hostname()");
  }
  // TODO: make UDP mode IPvX-agnostic
  else {
    evutil_socket_t fd;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
      DIE("socket()");

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(atoi(port.c_str()));
    if (inet_pton(AF_INET, hostname.c_str(), &sin.sin_addr) <= 0)
      DIE("pton()");
    // sin_zero is for byte-padding; should be zeroed
    memset(&sin.sin_zero, 0, sizeof(sin.sin_zero));

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)))
      DIE("connect()");

    int n = 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) != -1)
      V("Increased RCVBUF size");

    // if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n)) != -1)
    //   printf("Increased SNDBUF size\n");

    ev = event_new(base, fd, EV_READ | EV_PERSIST, udp_event_cb, this);
    read_state = IDLE;
    // D("UDP socket %s:%s writable.", hostname.c_str(), port.c_str());
    memset(udpHdr, 0, sizeof(udpHdr));
    udpHdr[5] = 1;    // want to send only 1 datagram

    timeout = {3, 0};
    event_add(ev, &timeout);

    read = evbuffer_new();
    write = evbuffer_new();
  }

  srand48(time(NULL));
  timer = evtimer_new(base, timer_cb, this);
}

Connection::~Connection() {
  event_free(timer);
  timer = NULL;

  // FIXME:  W("Drain op_q?");

  // bufferevent already set to close on free
  if (!options.udp) bufferevent_free(bev);
  else {
    close(event_get_fd(ev));
    event_free(ev);
    evbuffer_free(read);
    evbuffer_free(write);
  }

  delete iagen;
  delete keygen;
  delete keysize;
  delete valuesize;
}

void Connection::reset() {
  // FIXME: Actually check the connection, drain all bufferevents, drain op_q.
  assert(op_queue.size() == 0);
  evtimer_del(timer);
  read_state = IDLE;
  write_state = INIT_WRITE;
  stats = ConnectionStats(stats.sampling);
}

void Connection::issue_sasl() {
  read_state = WAITING_FOR_SASL;

  string username = string(options.username);
  string password = string(options.password);

  binary_header_t header = {0x80, CMD_SASL, 0, 0, 0, {0}, 0, 0, 0};
  header.key_len = htons(5);
  header.body_len = htonl(6 + username.length() + 1 + password.length());

  bufferevent_write(bev, &header, 24);
  bufferevent_write(bev, "PLAIN\0", 6);
  bufferevent_write(bev, username.c_str(), username.length() + 1);
  bufferevent_write(bev, password.c_str(), password.length());
}

void Connection::issue_get(const char* key, double now) {
  Operation op;
  int l;
  uint16_t keylen = strlen(key);

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) {
#if USE_CACHED_TIME
    struct timeval now_tv;
    event_base_gettimeofday_cached(base, &now_tv);

    op.start_time = tv_to_double(&now_tv);
#else
    op.start_time = get_time();
#endif
  } else {
    op.start_time = now;
  }
#endif

  op.type = Operation::GET;
  op.key = string(key);

  op_queue.push(op);

  if (read_state == IDLE)
    read_state = WAITING_FOR_GET;

if (options.binary) {
    // each line is 4-bytes
    binary_header_t h = {0x80, CMD_GET, htons(keylen),
                       0x00, 0x00, {htons(0)}, //TODO(syang0) get actual vbucket?
                       htonl(keylen) };
                       
    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));
      evbuffer_add(write, &h, 24);  // size does not include extras
      evbuffer_add(write, key, keylen);
      l = sizeof(udpHdr) + 24 + keylen;

      // char foo[500];
      // memset(foo, 0, 500);
      // evbuffer_copyout(write, &foo, evbuffer_get_length(write));

      evbuffer_write(write, event_get_fd(ev));
    }
    else {
      bufferevent_write(bev, &h, 24); // size does not include extras
      bufferevent_write(bev, key, keylen);
      l = 24 + keylen;
    }
  } else {
    char getHdr[] = "get %s\r\n";
    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));
      l = sizeof(udpHdr) + evbuffer_add_printf(write, getHdr,
                                key);

      evbuffer_write(write, event_get_fd(ev));
    }
    else l = evbuffer_add_printf(bufferevent_get_output(bev), "get %s\r\n", key);
  }

  if (read_state != LOADING) stats.tx_bytes += l;
}

void Connection::issue_set(const char* key, const char* value, int length,
                           double now) {
  Operation op;
  int l;
  uint16_t keylen = strlen(key);

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) op.start_time = get_time();
  else op.start_time = now;
#endif

  op.type = Operation::SET;
  op_queue.push(op);

  if (read_state == IDLE)
    read_state = WAITING_FOR_SET;

  if (options.binary) {
    // each line is 4-bytes
    binary_header_t h = { 0x80, CMD_SET, htons(keylen),
                        0x08, 0x00, {htons(0)}, //TODO(syang0) get actual vbucket?
                        htonl(keylen + 8 + length)};

    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));
      evbuffer_add(write, &h, 32);  // With extras
      evbuffer_add(write, key, keylen);
      evbuffer_add(write, value, length);
      l = sizeof(udpHdr) + sizeof(h) + keylen + length;

      evbuffer_write(write, event_get_fd(ev));
    }
    else {
      bufferevent_write(bev, &h, 32); // With extras
      bufferevent_write(bev, key, keylen);
      bufferevent_write(bev, value, length);
      l = 24 + h.body_len;
    }
  }
  else {
    char setHdr[] = "set %s 0 0 %d\r\n";

    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));

      l = sizeof(udpHdr) + evbuffer_add_printf(write, setHdr,
                                key, length);

      evbuffer_add(write, value, length);
      evbuffer_add(write, "\r\n", 2);

      evbuffer_write(write, event_get_fd(ev));
    }
    else {
      l = evbuffer_add_printf(bufferevent_get_output(bev),
                                setHdr, key, length);
      bufferevent_write(bev, value, length);
      bufferevent_write(bev, "\r\n", 2);
    }

    l += length + 2;
  }

  if (read_state != LOADING) stats.tx_bytes += l;
  loadedKeys.insert(atoll(key));
}

void Connection::issue_delete(const char* key, double now) {
  Operation op;
  int l;
  uint16_t keylen = strlen(key); 

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) {
#if USE_CACHED_TIME
    struct timeval now_tv;
    event_base_gettimeofday_cached(base, &now_tv);

    op.start_time = tv_to_double(&now_tv);
#else
    op.start_time = get_time();
#endif
  } else {
    op.start_time = now;
  }
#endif

  op.type = Operation::DELETE;
  op.key = string(key);

  op_queue.push(op);

  if (read_state == IDLE)
    read_state = WAITING_FOR_DELETE;

  if (options.binary) {
    // each line is 4-bytes
    binary_header_t h = {0x80, CMD_DELETE, htons(keylen),
                       0x00, 0x00, {htons(0)}, //TODO(syang0) get actual vbucket?
                       htonl(keylen) };
                       
    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));
      evbuffer_add(write, &h, 24);  // size does not include extras
      evbuffer_add(write, key, keylen);
      l = sizeof(udpHdr) + 24 + keylen;

      // char foo[500];
      // memset(foo, 0, 500);
      // evbuffer_copyout(write, &foo, evbuffer_get_length(write));

      evbuffer_write(write, event_get_fd(ev));
    }
    else {
      bufferevent_write(bev, &h, 24); // size does not include extras
      bufferevent_write(bev, key, keylen);
      l = 24 + keylen;
    }
  } else {
    char getHdr[] = "delete %s\r\n";
    if (options.udp) {
      evbuffer_add(write, udpHdr, sizeof(udpHdr));
      l = sizeof(udpHdr) + evbuffer_add_printf(write, getHdr,
                                key);

      evbuffer_write(write, event_get_fd(ev));
    }
    else l = evbuffer_add_printf(bufferevent_get_output(bev), "delete %s\r\n", key);
  }

  if (read_state != LOADING) stats.tx_bytes += l;
}

// generate key from loader_issued, possibly?
// this would be sequential, and therefore possibly bad
void Connection::issue_something(double now) {
  // int key_index = lrand48() % options.records;
  // generate_key(key_index, options.keysize, key);

  // if (!(post_load_issued % 6)) issue_delete(key, now);

  /* Note that use of ratio overrides --update. */
  if (options.ratioSum) {
    char key[256];
    // FIXME: generate key distribution here!
    string keystr = keygen->generate(lrand48() % options.records);
    strcpy(key, keystr.c_str());

    int cycleIndex = lrand48() % options.ratioSum;

    int opToPerform;
    for (opToPerform = 0; opToPerform < 7; opToPerform++) {
      cycleIndex -= options.intRatios[opToPerform];
      if (cycleIndex < 0) break;
    }

    // printf("\t opToPerform: %d\n", opToPerform);

    // idea = generate the key randomly, check for it, then
    // while (<<generate new key>>)
    /*printf("\t\topToPerform: %d\n", opToPerform); */
    //while (1) {
      // char key[256];
      // // \/ would be a distribution
      // key_t numericKey = lrand48() % options.records;
      // string cppkey = keygen->generate(numericKey);
      // strcpy(key, cppkey.c_str());
      switch(opToPerform) {
        case 0: {
          // stop program... for now
          if (absentKeys.empty()) {
            DIE("All keys set; cannot set absent key");
          }
          key_t keyInQuestion = absentKeys.front();
          absentKeys.pop();
          loadedKeys.insert(keyInQuestion);
          char key2[256];
          int index = keyInQuestion % (1024 * 1024);
          string keystr2 = keygen->generate(keyInQuestion);
          strcpy(key2, keystr2.c_str());
        
        issue_set(key, &random_char[index], valuesize->generate());
        // loader_issued++;

          ratioStats.sa++;
        }; break;
        case 1: {
          ratioStats.slss++;
          //randomCheck needed...
          // would generate range [to get from] here
          if (!loadedKeys.count(atoll(key))){
            issue_get(key, now);
            return;
          }
          int index = atoll(key) % (1024 * 1024);
          issue_set(key, &random_char[index], valuesize->generate(), now);
        }; break;
        case 2: {
          ratioStats.slds++;
          // randomCheck needed, same as above...
          issue_get(key, now);
        }; break;
        case 3: {
          ratioStats.ga++;
          // this actually does what it's supposed to
          if (absentKeys.empty()) {
            // SKIP_THIS_TURN?  defaulting to issue_get(...)...
            issue_get(key, now);
            return;
          }
          key_t keyInQuestion = absentKeys.front();
          absentKeys.pop();               // if didn't do this...
          absentKeys.push(keyInQuestion); // wouldn't even need to do this
          char key2[256];
          string keystr2 = keygen->generate(keyInQuestion);
          strcpy(key2, keystr2.c_str());
        
        issue_get(key2, now);
        // loader_issued++;
        }; break;
        case 4: {
          ratioStats.gl++;
          if (loadedKeys.count(atoll(key))) {
            issue_get(key, now);
            return;
          }
          // dummy below, not above
          issue_get(key, now);
        }; break;
        case 5: {
          ratioStats.da++;
          if (absentKeys.empty()) {
            // SKIP_THIS_TURN?  defaulting to issue_get(...)...
            issue_get(key, now);
            return;
          }
          key_t keyInQuestion = absentKeys.front();
          absentKeys.pop();               // if didn't do this...
          absentKeys.push(keyInQuestion); // wouldn't even need to do this
          char key2[256];
          string keystr2 = keygen->generate(keyInQuestion);
          strcpy(key2, keystr2.c_str());
        
        issue_delete(key2, now);
        // loader_issued++;

        }; break;
        case 6: {
          ratioStats.dl++;
          if (loadedKeys.count(atoll(key))) {
            issue_delete(key, now);
            absentKeys.push(atoll(key)); 
            return;
          }
          issue_get(key, now);
        }; break;
      }
    //}
  }
  else {
    char key[256];
    // FIXME: generate key distribution here!
    string keystr = keygen->generate(lrand48() % options.records);
    strcpy(key, keystr.c_str());
    if (drand48() < options.update) {
      // int index = lrand48() % (1024 * 1024);
      // use atoll, not atoi?
      int index = atoi(key) % (1024 * 1024);
      //    issue_set(key, &random_char[index], options.valuesize, now);
      issue_set(key, &random_char[index], valuesize->generate(), now);
    }
    else {
      issue_get(key, now);
    }
  }
}

void Connection::pop_op() {
  assert(op_queue.size() > 0);

  op_queue.pop();

  if (read_state == LOADING) return;
  read_state = IDLE;

  // Advance the read state machine.
  if (op_queue.size() > 0) {
    Operation& op = op_queue.front();
    switch (op.type) {
    case Operation::GET: read_state = WAITING_FOR_GET; break;
    case Operation::SET: read_state = WAITING_FOR_SET; break;
    case Operation::DELETE: read_state = WAITING_FOR_DELETE; break;
    default: DIE("Not implemented.");
    }
  }
}

// Right now, timeout is the only way to stop testing.
bool Connection::check_exit_condition(double now) {
  if (read_state == INIT_READ) return false;
  if (now == 0.0) now = get_time();
  if (now > start_time + options.time) return true;
  if (options.loadonly && read_state == IDLE) return true;
  return false;
}

// drive_write_machine() determines whether or not to issue a new
// command.  Note that this function loops.  Be wary of break
// vs. return.

void Connection::drive_write_machine(double now) {
  if (now == 0.0) now = get_time();

  double delay;
  struct timeval tv;

  if (check_exit_condition(now)) return;

  while (1) {
    switch (write_state) {
    case INIT_WRITE:
      delay = iagen->generate();

      next_time = now + delay;
      double_to_tv(delay, &tv);
      evtimer_add(timer, &tv);

      write_state = WAITING_FOR_TIME;
      break;

    case ISSUING:
      if (op_queue.size() >= (size_t) options.depth) {
        write_state = WAITING_FOR_OPQ;
        return;
      } else if (now < next_time) {
        write_state = WAITING_FOR_TIME;
        break; // We want to run through the state machine one more time
               // to make sure the timer is armed.
        //      } else if (options.moderate && options.lambda > 0.0 &&
        //                 now < last_rx + 0.25 / options.lambda) {
      } else if (options.moderate && now < last_rx + 0.00025) {
        write_state = WAITING_FOR_TIME;
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          //          delay = last_rx + 0.25 / options.lambda - now;
          delay = last_rx + 0.00025 - now;
          //          I("MODERATE %f %f %f %f %f", now - last_rx, 0.25/options.lambda,
            //            1/options.lambda, now-last_tx, delay);
          
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }

      issue_something(now);
      last_tx = now;
      stats.log_op(op_queue.size());

      next_time += iagen->generate();

      if (options.skip && options.lambda > 0.0 &&
          now - next_time > 0.005000 &&
          op_queue.size() >= (size_t) options.depth) {

        while (next_time < now - 0.004000) {
          stats.skips++;
          next_time += iagen->generate();
        }
      }

      break;

    case WAITING_FOR_TIME:
      if (now < next_time) {
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          delay = next_time - now;
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }

      write_state = ISSUING;
      break;

    case WAITING_FOR_OPQ:
      if (op_queue.size() >= (size_t) options.depth) return;
      write_state = ISSUING;
      break;

    default: DIE("Not implemented");
    }
  }
}

void Connection::bev_callback(short events) {
  //  struct timeval now_tv;
  // event_base_gettimeofday_cached(base, &now_tv);

  if (events & BEV_EVENT_CONNECTED) {
    D("Connected to %s:%s.", hostname.c_str(), port.c_str());

    int fd = bufferevent_getfd(bev);
    if (fd < 0) DIE("bufferevent_getfd");

    if (!options.no_nodelay) {
      int one = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (void *) &one, sizeof(one)) < 0)
        DIE("setsockopt()");
    }

    if (options.sasl)
      issue_sasl();
    else
      read_state = IDLE;  // This is the most important part!
  } else if (events & BEV_EVENT_ERROR) {
    int err = bufferevent_socket_get_dns_error(bev);
    if (err) DIE("DNS error: %s", evutil_gai_strerror(err));

    DIE("BEV_EVENT_ERROR: %s", strerror(errno));
  } else if (events & BEV_EVENT_EOF) {
    DIE("Unexpected EOF from server.");
  }
}

void Connection::read_callback() {
  struct evbuffer *input;
  if (options.udp) {
      // 3. thread barrier: don't release our threads until all agents ready
    evbuffer_read(read, event_get_fd(ev), 2000);
    // if (!evbuffer_get_length(read)) return;  // nothing to process
    // drain the read buffer for now...
    // TODO: parse returned pseudoheader, organize
    evbuffer_drain(read, 8);
    input = read;
  }
  else input = bufferevent_get_input(bev);

#if USE_CACHED_TIME
  struct timeval now_tv;
  event_base_gettimeofday_cached(base, &now_tv);
#endif

  char *buf = NULL;
  Operation *op = NULL;
  int length;
  size_t n_read_out;

  double now;

  // Protocol processing loop.

  if (op_queue.size() == 0) V("Spurious read callback.");

  while (1) {
    if (op_queue.size() > 0) op = &op_queue.front();

    switch (read_state) {
    case INIT_READ: DIE("event from uninitialized connection");
    case IDLE: return;  // We munched all the data we expected?

    // Note: for binary, the whole get suite (GET, GET_DATA, END) is collapsed
    // into one state
    case WAITING_FOR_GET:
      assert(op_queue.size() > 0);

      if (options.binary) {
        if (consume_binary_response(input)) {
#if USE_CACHED_TIME
            now = tv_to_double(&now_tv);
#else
            now = get_time();
#endif
#if HAVE_CLOCK_GETTIME
            op->end_time = get_time_accurate();
#else
            op->end_time = now;
#endif
            stats.log_get(*op);

            last_rx = now;
            pop_op();
            drive_write_machine(now);
            break;
        } else {
          return;
        }
      }

      buf = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF);
      if (buf == NULL) return;  // A whole line not received yet. Punt.

      stats.rx_bytes += n_read_out; // strlen(buf);

      if (!strcmp(buf, "END")) {
        //        D("GET (%s) miss.", op->key.c_str());
        stats.get_misses++;

#if USE_CACHED_TIME
        now = tv_to_double(&now_tv);
#else
        now = get_time();
#endif
#if HAVE_CLOCK_GETTIME
        op->end_time = get_time_accurate();
#else
        op->end_time = now;
#endif

        stats.log_get(*op);

        free(buf);

        last_rx = now;
        pop_op();
        drive_write_machine();
        break;
      } else if (!strncmp(buf, "VALUE", 5)) {
        sscanf(buf, "VALUE %*s %*d %d", &length);

        // FIXME: check key name to see if it corresponds to the op at
        // the head of the op queue?  This will be necessary to
        // support "gets" where there may be misses.

        data_length = length;
        read_state = WAITING_FOR_GET_DATA;
      }

      free(buf);

    case WAITING_FOR_GET_DATA:
      assert(op_queue.size() > 0);

      length = evbuffer_get_length(input);

      if (length >= data_length + 2) {
        // FIXME: Actually parse the value?  Right now we just drain it.

        /*
        unsigned char* buffer = evbuffer_pullup(input, valuesize->generate());
        int index = atoi(key) % (1024 * 1024);
      //    issue_set(key, &random_char[index], options.valuesize, now);
      issue_set(key, &random_char[index], valuesize->generate(), now);
      */



        evbuffer_drain(input, data_length + 2);
        read_state = WAITING_FOR_END;

        stats.rx_bytes += data_length + 2;
      } else {
        return;
      }

    case WAITING_FOR_DELETE:
      assert(op_queue.size() > 0);
      pop_op();
      drive_write_machine();
      return;

    case WAITING_FOR_END:
      assert(op_queue.size() > 0);

      buf = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF);
      if (buf == NULL) return; // Haven't received a whole line yet. Punt.

      stats.rx_bytes += n_read_out;

      if (!strcmp(buf, "END")) {
#if USE_CACHED_TIME
        now = tv_to_double(&now_tv);
#else
        now = get_time();
#endif
#if HAVE_CLOCK_GETTIME
        op->end_time = get_time_accurate();
#else
        op->end_time = now;
#endif

        stats.log_get(*op);

        free(buf);

        last_rx = now;
        pop_op();
        drive_write_machine(now);
        break;
      } else {
        DIE("Unexpected result when waiting for END");
      }

    case WAITING_FOR_SET:
      assert(op_queue.size() > 0);

      if (options.binary) {
        if (!consume_binary_response(input)) return;
      } else {
        buf = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF);
        if (buf == NULL) return; // Haven't received a whole line yet. Punt.
        stats.rx_bytes += n_read_out;
      }

      now = get_time();

#if HAVE_CLOCK_GETTIME
      op->end_time = get_time_accurate();
#else
      op->end_time = now;
#endif

      stats.log_set(*op);

      if (!options.binary)
        free(buf);

      last_rx = now;
      pop_op();
      drive_write_machine(now);
      break;

    case LOADING:
      assert(op_queue.size() > 0);

      if (options.binary) {
        if (!consume_binary_response(input)) return;
      } else {
        buf = evbuffer_readln(input, NULL, EVBUFFER_EOL_CRLF);
        if (buf == NULL) return; // Haven't received a whole line yet.
        free(buf);
      }

      loader_completed++;
      pop_op();

      if (loader_completed == options.records) {
        D("Finished loading.");
        read_state = IDLE;
      } else {
        // printf("issued: %d; completed: %d\n", loader_issued, loader_completed);
        while (loader_issued < loader_completed + options.loader_chunk) {
          if (loader_issued >= options.records) break;

          if (!(loader_issued % options.loader_chunk)) usleep(options.rate_delay);

          char key[256];
          string keystr = keygen->generate(loader_issued);
          strcpy(key, keystr.c_str());
          // int index = lrand48() % (1024 * 1024);
          int index = atoi(key) % (1024 * 1024);
          //          generate_key(loader_issued, options.keysize, key);
          //          issue_set(key, &random_char[index], options.valuesize);
          issue_set(key, &random_char[index], valuesize->generate());

          loader_issued++;
        }
      }

      break;

    case WAITING_FOR_SASL:
      assert(options.binary);
      if (!consume_binary_response(input)) return;
      read_state = IDLE;
      break;

    default: DIE("not implemented");
    }
  }
}

/**
 * Tries to consume a binary response (in its entirety) from an evbuffer.
 *
 * @param input evBuffer to read response from
 * @return  true if consumed, false if not enough data in buffer
 */
bool Connection::consume_binary_response(evbuffer *input) {
  // Read the first 24 bytes as a header
  int length = evbuffer_get_length(input);
  if (length < 24) return false;
  binary_header_t* h =
          reinterpret_cast<binary_header_t*>(evbuffer_pullup(input, 24));
  assert(h);

  // Not whole response
  int targetLen = 24 + ntohl(h->body_len);
  if (length < targetLen) {
    return false;
  }

  // if something other than success, count it as a miss
  if (h->opcode == CMD_GET && h->status) {
      stats.get_misses++;
 } 

  #define unlikely(x) __builtin_expect((x),0)
  if (unlikely(h->opcode == CMD_SASL)) {
    if (h->status == RESP_OK) {
      V("SASL authentication succeeded");
    } else {;
      DIE("SASL authentication failed");
    }
  }

  evbuffer_drain(input, targetLen);
  stats.rx_bytes += targetLen;
  return true;
}

void Connection::udp_callback(short events) {
 /*
  printf("Got an event on socket [%d]:%s%s%s%s\n",
    event_get_fd(ev),
    (events&EV_TIMEOUT) ? " timeout" : "",
    (events&EV_READ)    ? " read" : "",
    (events&EV_WRITE)   ? " write" : "",
    (events&EV_SIGNAL)  ? " signal" : ""
    );
  //*/

  if (events & EV_READ) read_callback();

  if (events & EV_TIMEOUT && loader_completed != loader_issued) {
    V("issued: %d; completed: %d", loader_issued, loader_completed);
    loader_completed = loader_issued;   // HAKCERY
    // event_remove_timer(ev);
    // because the above^ line is deprecated?
    event_del(ev);      // ^ should just remove timer...
    event_add(ev, NULL);
    // for (unsigned int i = 0; i < op_queue.size(); i++) op_queue.pop(); 
    drain_op_queue();
    read_state = IDLE;
  }

  /* UDP connections must fire the read callback manually - 
     for TCP connections, bufferevent has its own read callback. */
}

void Connection::write_callback() {}
void Connection::timer_callback() { drive_write_machine(); }

// The follow are C trampolines for libevent callbacks.
void bev_event_cb(struct bufferevent *bev, short events, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->bev_callback(events);
}

void bev_read_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->read_callback();
}

void bev_write_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->write_callback();
}

void udp_event_cb(evutil_socket_t fd, short events, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->udp_callback(events);
}

void timer_cb(evutil_socket_t fd, short events, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->timer_callback();
}

void Connection::set_priority(int pri) {
  if (bufferevent_priority_set(bev, pri))
    DIE("bufferevent_set_priority(bev, %d) failed", pri);
}

void Connection::start_loading() {
  read_state = LOADING;
  loader_issued = loader_completed = 0;

  for (int i = 0; i < options.loader_chunk; i++) {
    if (loader_issued >= options.records) break;

    // if (options.udp) {                // optimized out?
    //   usleep(options.rate_delay);
    //   event_base_loop(base, EVLOOP_NONBLOCK);
    // }

    char key[256];
    // int index = lrand48() % (1024 * 1024);
    int index = atoi(key) % (1024 * 1024);
    // printf("loader_issued: %d; random_char[%d]\n", loader_issued, index);
    string keystr = keygen->generate(loader_issued);
    strcpy(key, keystr.c_str());

    
          //    generate_key(loader_issued, options.keysize, key);
    //    issue_set(key, &random_char[index], options.valuesize);
    issue_set(key, &random_char[index], valuesize->generate());
    loader_issued++;
  }
}

void Connection::drain_op_queue() {
  unsigned int size = op_queue.size();
  for (unsigned int i = 0; i < size; i++) {
    op_queue.pop();
  }
}

void Connection::note_absent_keys() {
  for (int i = 0; i < options.records; i++) {
    absentKeys.push(i);
  } 
}
