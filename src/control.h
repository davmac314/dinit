#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>
#include <unordered_map>
#include <limits>
#include <cstddef>

#include <unistd.h>

#include "dasynq.h"

#include "dinit-log.h"
#include "control-cmds.h"
#include "service-listener.h"
#include "cpbuffer.h"

// Control connection for dinit

using namespace dasynq;
using eventloop_t = event_loop<null_mutex>;

class control_conn_t;
class control_conn_watcher;

// forward-declaration of callback:
static rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);

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
    inline rearm receiveEvent(eventloop_t &loop, int fd, int flags) noexcept;

    public:
    rearm read_ready(eventloop_t &loop, int fd) noexcept
    {
        return receiveEvent(loop, fd, IN_EVENTS);
    }
    
    rearm write_ready(eventloop_t &loop, int fd) noexcept
    {
        return receiveEvent(loop, fd, OUT_EVENTS);
    }
    
    eventloop_t * eventLoop;
    
    void set_watches(int flags)
    {
        eventloop_t::bidi_fd_watcher::set_watches(*eventLoop, flags);
    }
    
    void registerWith(eventloop_t &loop, int fd, int flags)
    {
        this->eventLoop = &loop;
        bidi_fd_watcher<eventloop_t>::add_watch(loop, fd, flags);
    }
};

inline rearm control_conn_watcher::receiveEvent(eventloop_t &loop, int fd, int flags) noexcept
{
    return control_conn_cb(&loop, this, flags);
}


class control_conn_t : private service_listener
{
    friend rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);
    
    control_conn_watcher iob;
    eventloop_t *loop;
    service_set *services;
    
    bool bad_conn_close = false; // close when finished output?
    bool oom_close = false;      // send final 'out of memory' indicator

    // The packet length before we need to re-check if the packet is complete.
    // processPacket() will not be called until the packet reaches this size.
    int chklen;
    
    // Receive buffer
    cpbuffer<1024> rbuf;
    
    template <typename T> using list = std::list<T>;
    template <typename T> using vector = std::vector<T>;
    
    // A mapping between service records and their associated numerical identifier used
    // in communction
    using handle_t = uint32_t;
    std::unordered_multimap<service_record *, handle_t> serviceKeyMap;
    std::unordered_map<handle_t, service_record *> keyServiceMap;
    
    // Buffer for outgoing packets. Each outgoing back is represented as a vector<char>.
    list<vector<char>> outbuf;
    // Current index within the first outgoing packet (all previous bytes have been sent).
    unsigned outpkt_index = 0;
    
    // Queue a packet to be sent
    //  Returns:  false if the packet could not be queued and a suitable error packet
    //              could not be sent/queued (the connection should be closed);
    //            true (with bad_conn_close == false) if the packet was successfully
    //              queued;
    //            true (with bad_conn_close == true) if the packet was not successfully
    //              queued (but a suitable error packate has been queued).
    // The in/out watch enabled state will also be set appropriately.
    bool queuePacket(vector<char> &&v) noexcept;
    bool queuePacket(const char *pkt, unsigned size) noexcept;

    // Process a packet.
    //  Returns:  true (with bad_conn_close == false) if successful
    //            true (with bad_conn_close == true) if an error packet was queued
    //            false if an error occurred but no error packet could be queued
    //                (connection should be closed).
    // Throws:
    //    std::bad_alloc - if an out-of-memory condition prevents processing
    bool processPacket();
    
    // Process a STARTSERVICE/STOPSERVICE packet. May throw std::bad_alloc.
    bool processStartStop(int pktType);
    
    // Process a FINDSERVICE/LOADSERVICE packet. May throw std::bad_alloc.
    bool processFindLoad(int pktType);

    // Process an UNPINSERVICE packet. May throw std::bad_alloc.
    bool processUnpinService();
    
    bool listServices();

    // Notify that data is ready to be read from the socket. Returns true if the connection should
    // be closed.
    bool dataReady() noexcept;
    
    bool sendData() noexcept;
    
    // Allocate a new handle for a service; may throw std::bad_alloc
    handle_t allocateServiceHandle(service_record *record);
    
    service_record *findServiceForKey(uint32_t key)
    {
        try {
            return keyServiceMap.at(key);
        }
        catch (std::out_of_range &exc) {
            return nullptr;
        }
    }
    
    // Close connection due to out-of-memory condition.
    void doOomClose()
    {
        bad_conn_close = true;
        oom_close = true;
        iob.set_watches(OUT_EVENTS);
    }
    
    // Process service event broadcast.
    // Note that this can potentially be called during packet processing (upon issuing
    // service start/stop orders etc).
    void serviceEvent(service_record * service, service_event event) noexcept final override
    {
        // For each service handle corresponding to the event, send an information packet.
        auto range = serviceKeyMap.equal_range(service);
        auto & i = range.first;
        auto & end = range.second;
        try {
            while (i != end) {
                uint32_t key = i->second;
                std::vector<char> pkt;
                constexpr int pktsize = 3 + sizeof(key);
                pkt.reserve(pktsize);
                pkt.push_back(DINIT_IP_SERVICEEVENT);
                pkt.push_back(pktsize);
                char * p = (char *) &key;
                for (int j = 0; j < (int)sizeof(key); j++) {
                    pkt.push_back(*p++);
                }
                pkt.push_back(static_cast<char>(event));
                queuePacket(std::move(pkt));
                ++i;
            }
        }
        catch (std::bad_alloc &exc) {
            doOomClose();
        }
    }
    
    public:
    control_conn_t(eventloop_t * loop, service_set * services_p, int fd) : loop(loop), services(services_p), chklen(0)
    {
        iob.registerWith(*loop, fd, IN_EVENTS);
        active_control_conns++;
    }
    
    bool rollbackComplete() noexcept;
        
    virtual ~control_conn_t() noexcept;
};


static rearm control_conn_cb(eventloop_t * loop, control_conn_watcher * watcher, int revents)
{
    char * cc_addr = (reinterpret_cast<char *>(watcher)) - offsetof(control_conn_t, iob);
    control_conn_t *conn = reinterpret_cast<control_conn_t *>(cc_addr);
    if (revents & IN_EVENTS) {
        if (conn->dataReady()) {
            delete conn;
            return rearm::REMOVED;
        }
    }
    if (revents & OUT_EVENTS) {
        if (conn->sendData()) {
            delete conn;
            return rearm::REMOVED;
        }
    }
    
    return rearm::NOOP;
}

#endif
