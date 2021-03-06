// -*- c++-mode -*-

#include <queue>
#include <string>
#include <unordered_set>

#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/util.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "AdaptiveSampler.h"
#include "cmdline.h"
#include "ConnectionOptions.h"
#include "ConnectionStats.h"
#include "Generator.h"
#include "Operation.h"
#include "util.h"

using namespace std;

void bev_event_cb(struct bufferevent *bev, short events, void *ptr);
void bev_read_cb(struct bufferevent *bev, void *ptr);
void bev_write_cb(struct bufferevent *bev, void *ptr);
void udp_event_cb(evutil_socket_t fd, short what, void *ptr);
void timer_cb(evutil_socket_t fd, short what, void *ptr);

class Connection {
public:
  Connection(struct event_base* _base, struct evdns_base* _evdns,
             string _hostname, string _port, options_t options,
             bool sampling = true);
  ~Connection();

  string hostname;
  string port;

  double start_time;  // Time when this connection began operations.

  enum read_state_enum {
    INIT_READ,
    LOADING,
    IDLE,
    WAITING_FOR_SASL,
    WAITING_FOR_GET,
    WAITING_FOR_GET_DATA,
    WAITING_FOR_END,
    WAITING_FOR_SET,
    WAITING_FOR_DELETE,
    MAX_READ_STATE,
  };

  enum write_state_enum {
    INIT_WRITE,
    ISSUING,
    WAITING_FOR_TIME,
    WAITING_FOR_OPQ,
    MAX_WRITE_STATE,
  };

  read_state_enum read_state;
  write_state_enum write_state;

  ConnectionStats stats;

  void issue_get(const char* key, double now = 0.0);
  void issue_set(const char* key, const char* value, int length,
                 double now = 0.0);
  void issue_delete(const char *key, double now = 0.0);

  void issue_something(double now = 0.0);
  void pop_op();
  bool check_exit_condition(double now = 0.0);
  void drive_write_machine(double now = 0.0);

  void start_loading();
  void drive_rate_control();

  void reset();
  void issue_sasl();

  void bev_callback(short events);
  void read_callback();
  void write_callback();
  void udp_callback(short events);
  void timer_callback();
  bool consume_binary_response(evbuffer *input);

  void set_priority(int pri);

  void note_absent_keys();
  void drain_op_queue();

  options_t options;

  std::queue<Operation> op_queue;

private:
  struct event_base *base;
  struct evdns_base *evdns;
  struct bufferevent *bev;

  struct event *ev;       // UDP only
  struct evbuffer *read;  // UDP only
  struct evbuffer *write; // UDP only
  char udpHdr[8];         // UDP only
  struct timeval timeout; // UDP only

  struct event *timer;  // Used to control inter-transmission time.
  //  double lambda;
  double next_time; // Inter-transmission time parameters.
  double last_rx; // Used to moderate transmission rate.
  double last_tx;

  int data_length;  // When waiting for data, how much we're peeking for.

  // for --ratio.  keeps track of operations issued.
  // s - set; g - get; d - delete
  // a - absent (key not in memcached); l - loaded (key in memcached)
  // ss - same (value) size; ds - different size (not yet implemented)
  struct {
    int sa, slss, slds,
      ga, gl, da, dl;
  } ratioStats;
  int loader_issued, loader_completed;
  
  typedef key_t uint_t;
  std::queue<key_t> absentKeys;
  std::unordered_set<key_t> loadedKeys;

  // Parameters to tack progress of second-stage operations
  // int post_load_issued;

  Generator *valuesize;
  Generator *keysize;
  KeyGenerator *keygen;
  Generator *iagen;
};