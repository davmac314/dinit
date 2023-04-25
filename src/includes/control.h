#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>
#include <unordered_map>
#include <map>
#include <limits>
#include <cstddef>

#include <unistd.h>

#include "dinit.h"
#include "dinit-log.h"
#include "control-cmds.h"
#include "service-listener.h"
#include "cpbuffer.h"

// Control connection for dinit

constexpr int OUTBUF_LIMIT = 16384; // Output buffer high-water mark

class control_conn_t;
class control_conn_watcher;

// forward-declaration of callback:
inline dasynq::rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);

// Pointer to the control connection that is listening for rollback completion
extern control_conn_t * rollback_handler_conn;

extern int active_control_conns;

// "packet" format:
// (1 byte) packet type
// (N bytes) additional data (service name, etc)
//   for LOADSERVICE/FINDSERVICE:
//      (2 bytes) service name length
//      (M bytes) service name (without nul terminator)

// Information packet:
// (1 byte) packet type, >= 100
// (1 byte) packet length (including all fields)
//       N bytes: packet data (N = (length - 2))

class service_set;
class service_record;

class control_conn_watcher : public eventloop_t::bidi_fd_watcher_impl<control_conn_watcher>
{
    inline rearm receive_event(eventloop_t &loop, int fd, int flags) noexcept
    {
        return control_conn_cb(&loop, this, flags);
    }

    eventloop_t * event_loop;

    public:
    control_conn_watcher(eventloop_t & event_loop_p) : event_loop(&event_loop_p)
    {
        // constructor
    }

    control_conn_watcher(const control_conn_watcher &) = delete;
    void operator=(const control_conn_watcher &) = delete;

    rearm read_ready(eventloop_t &loop, int fd) noexcept
    {
        return receive_event(loop, fd, dasynq::IN_EVENTS);
    }
    
    rearm write_ready(eventloop_t &loop, int fd) noexcept
    {
        return receive_event(loop, fd, dasynq::OUT_EVENTS);
    }

    void set_watches(int flags)
    {
        eventloop_t::bidi_fd_watcher::set_watches(*event_loop, flags);
    }
};

class control_conn_t : private service_listener
{
    friend rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);
    friend class control_conn_t_test;
    
    public:
    // A mapping between service records and their associated numerical identifier used
    // in communction
    using handle_t = uint32_t;

    private:
    control_conn_watcher iob;
    eventloop_t &loop;
    service_set *services;
    
    bool bad_conn_close = false; // close when finished output?
    bool oom_close = false;      // send final 'out of memory' indicator

    // The packet length before we need to re-check if the packet is complete.
    // process_packet() will not be called until the packet reaches this size.
    unsigned chklen;
    
    // Receive buffer
    cpbuffer<1024> rbuf;
    
    template <typename T> using list = std::list<T>;
    template <typename T> using vector = std::vector<T>;
    
    std::unordered_multimap<service_record *, handle_t> service_key_map;
    std::map<handle_t, service_record *> key_service_map;
    
    // Buffer for outgoing packets. Each outgoing packet is represented as a vector<char>.
    list<vector<char>> outbuf;
    // Current output buffer size in bytes.
    unsigned outbuf_size = 0;
    // Current index within the first outgoing packet (all previous bytes have been sent).
    unsigned outpkt_index = 0;
    
    // Queue a packet to be sent
    //  Returns:  false if the packet could not be queued and a suitable error packet
    //              could not be sent/queued (the connection should be closed);
    //            true (with bad_conn_close == false) if the packet was successfully
    //              queued;
    //            true (with bad_conn_close == true) if the packet was not successfully
    //              queued (but a suitable error packet has been queued).
    // The in/out watch enabled state will also be set appropriately.
    bool queue_packet(vector<char> &&v) noexcept;
    bool queue_packet(const char *pkt, unsigned size) noexcept;

    // Process a packet.
    //  Returns:  true (with bad_conn_close == false) if successful
    //            true (with bad_conn_close == true) if an error packet was queued
    //            false if an error occurred but no error packet could be queued
    //                (connection should be closed).
    // Throws:
    //    std::bad_alloc - if an out-of-memory condition prevents processing
    bool process_packet();
    
    // Process a STARTSERVICE/STOPSERVICE packet. May throw std::bad_alloc.
    bool process_start_stop(int pktType);
    
    // Process a FINDSERVICE/LOADSERVICE packet. May throw std::bad_alloc.
    bool process_find_load(int pktType);

    // Process an UNPINSERVICE packet. May throw std::bad_alloc.
    bool process_unpin_service();
    
    // Process an UNLOADSERVICE packet.
    bool process_unload_service();

    // Process a RELOADSERVICE packet. May throw std::bad_alloc.
    bool process_reload_service();

    // Process a QUERYSERVICENAME packet.
    bool process_query_name();

    // Process a SETENV packet.
    bool process_setenv();

    // Process a SETTRIGGER packet.
    bool process_set_trigger();

    // Process a CATLOG packet.
    bool process_catlog();

    // Process a SIGNAL packet.
    bool process_signal();

    // List all loaded services and their state.
    bool list_services();

    // Query service status/
    bool process_service_status();

    // Add a dependency between two services.
    bool add_service_dep(bool do_start = false);

    // Remove a dependency between two services.
    bool rm_service_dep();

    // Query service path / load mechanism.
    bool query_load_mech();

    // Notify that data is ready to be read from the socket. Returns true if the connection should
    // be closed.
    bool data_ready() noexcept;
    
    bool send_data() noexcept;

    // Check if any dependents will be affected by stopping a service, generate a response packet if so.
    // had_dependents will be set true if the service should not be stopped, false otherwise.
    // Returns false if the connection must be closed, true otherwise.
    bool check_dependents(service_record *service, bool &had_dependents);

    // Allocate a new handle for a service; may throw std::bad_alloc
    handle_t allocate_service_handle(service_record *record);
    
    // Find the service corresponding to a service handle; returns nullptr if not found.
    service_record *find_service_for_key(handle_t key) noexcept
    {
        try {
            return key_service_map.at(key);
        }
        catch (std::out_of_range &exc) {
            return nullptr;
        }
    }
    
    // Close connection due to out-of-memory condition.
    void do_oom_close() noexcept
    {
        bad_conn_close = true;
        oom_close = true;
    }
    
    // Process service event broadcast.
    // Note that this can potentially be called during packet processing (upon issuing
    // service start/stop orders etc).
    void service_event(service_record *service, service_event_t event) noexcept final override;
    
    public:
    control_conn_t(eventloop_t &loop, service_set * services_p, int fd)
            : iob(loop), loop(loop), services(services_p), chklen(0)
    {
        iob.add_watch(loop, fd, dasynq::IN_EVENTS);
        active_control_conns++;
    }
    
    control_conn_t(const control_conn_t &) = delete;

    virtual ~control_conn_t() noexcept;
};


inline dasynq::rearm control_conn_cb(eventloop_t * loop, control_conn_watcher * watcher, int revents)
{
    // Get the address of the containing control_connt_t object:
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Winvalid-offsetof\"")
    uintptr_t cc_addr = reinterpret_cast<uintptr_t>(watcher) - offsetof(control_conn_t, iob);
    control_conn_t *conn = reinterpret_cast<control_conn_t *>(cc_addr);
    _Pragma ("GCC diagnostic pop")

    if (revents & dasynq::IN_EVENTS) {
        if (conn->data_ready()) {
            delete conn;
            return dasynq::rearm::REMOVED;
        }
    }
    if (revents & dasynq::OUT_EVENTS) {
        if (conn->send_data()) {
            delete conn;
            return dasynq::rearm::REMOVED;
        }
    }
    
    // Accept more commands unless the output buffer high water mark is exceeded
    int watch_flags = 0;
    if (!conn->bad_conn_close && conn->outbuf_size < OUTBUF_LIMIT) {
        watch_flags |= dasynq::IN_EVENTS;
    }
    if (!conn->outbuf.empty() || conn->bad_conn_close) {
        watch_flags |= dasynq::OUT_EVENTS;
    }
    watcher->set_watches(watch_flags);

    return dasynq::rearm::NOOP;
}

#endif
