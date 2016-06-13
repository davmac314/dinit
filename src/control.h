#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>
#include <unordered_map>
#include <limits>
#include <cstddef>

#include <unistd.h>

#include "dasync.h"

#include "dinit-log.h"
#include "control-cmds.h"
#include "service-listener.h"
#include "cpbuffer.h"

// Control connection for dinit

using namespace dasync;
using EventLoop_t = EventLoop<NullMutex>;

class ControlConn;
class ControlConnWatcher;

// forward-declaration of callback:
static dasync::Rearm control_conn_cb(EventLoop_t *loop, ControlConnWatcher *watcher, int revents);

// Pointer to the control connection that is listening for rollback completion
extern ControlConn * rollback_handler_conn;

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

class ServiceSet;
class ServiceRecord;

class ControlConnWatcher : public PosixBidiFdWatcher<NullMutex>
{
    inline Rearm receiveEvent(EventLoop_t * loop, int fd, int flags) noexcept;

    Rearm readReady(EventLoop_t * loop, int fd) noexcept override
    {
        return receiveEvent(loop, fd, in_events);
    }
    
    Rearm writeReady(EventLoop_t * loop, int fd) noexcept override
    {
        return receiveEvent(loop, fd, out_events);
    }
    
    public:
    int fd; // TODO this is already stored, find a better way to access it.
    EventLoop_t * eventLoop;
    
    void setWatchFlags(int flags)
    {
        PosixBidiFdWatcher<NullMutex>::setWatchFlags(eventLoop, flags);
    }
    
    void registerWith(EventLoop_t *loop, int fd, int flags)
    {
        this->fd = fd;
        this->eventLoop = loop;
        PosixBidiFdWatcher<NullMutex>::registerWith(loop, fd, flags);
    }
};

inline Rearm ControlConnWatcher::receiveEvent(EventLoop_t * loop, int fd, int flags) noexcept
{
    return control_conn_cb(loop, this, flags);
}


class ControlConn : private ServiceListener
{
    friend dasync::Rearm control_conn_cb(EventLoop_t *loop, ControlConnWatcher *watcher, int revents);
    
    ControlConnWatcher iob;
    EventLoop_t *loop;
    ServiceSet *service_set;
    
    bool bad_conn_close = false; // close when finished output?
    bool oom_close = false;      // send final 'out of memory' indicator

    // The packet length before we need to re-check if the packet is complete.
    // processPacket() will not be called until the packet reaches this size.
    int chklen;
    
    // Receive buffer
    CPBuffer<1024> rbuf;
    
    template <typename T> using list = std::list<T>;
    template <typename T> using vector = std::vector<T>;
    
    // A mapping between service records and their associated numerical identifier used
    // in communction
    using handle_t = uint32_t;
    std::unordered_multimap<ServiceRecord *, handle_t> serviceKeyMap;
    std::unordered_map<handle_t, ServiceRecord *> keyServiceMap;
    
    // Buffer for outgoing packets. Each outgoing back is represented as a vector<char>.
    list<vector<char>> outbuf;
    // Current index within the first outgoing packet (all previous bytes have been sent).
    unsigned outpkt_index = 0;
    
    // Queue a packet to be sent
    //  Returns:  false if the packet could not be queued and the connection has been
    //              destroyed;
    //            true (with bad_conn_close == false) if the packet was successfully
    //              queued;
    //            true (with bad_conn_close == true) if the packet was not successfully
    //              queued.
    // The in/out watch enabled state will also be set appropriately.
    bool queuePacket(vector<char> &&v) noexcept;
    bool queuePacket(const char *pkt, unsigned size) noexcept;

    // Process a packet. Can cause the ControlConn to be deleted iff there are no
    // outgoing packets queued (returns false).
    // Throws:
    //    std::bad_alloc - if an out-of-memory condition prevents processing
    bool processPacket();
    
    // Process a STARTSERVICE/STOPSERVICE packet. May throw std::bad_alloc.
    bool processStartStop(int pktType);
    
    // Process a FINDSERVICE/LOADSERVICE packet. May throw std::bad_alloc.
    bool processFindLoad(int pktType);

    // Process an UNPINSERVICE packet. May throw std::bad_alloc.
    bool processUnpinService();

    // Notify that data is ready to be read from the socket. Returns true if the connection was
    // deleted.
    bool dataReady() noexcept;
    
    bool sendData() noexcept;
    
    // Allocate a new handle for a service; may throw std::bad_alloc
    handle_t allocateServiceHandle(ServiceRecord *record);
    
    ServiceRecord *findServiceForKey(uint32_t key)
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
        iob.setWatchFlags(out_events);
    }
    
    // Process service event broadcast.
    void serviceEvent(ServiceRecord * service, ServiceEvent event) noexcept final override
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
    ControlConn(EventLoop_t * loop, ServiceSet * service_set, int fd) : loop(loop), service_set(service_set), chklen(0)
    {
        iob.registerWith(loop, fd, in_events);
        active_control_conns++;
    }
    
    bool rollbackComplete() noexcept;
        
    virtual ~ControlConn() noexcept;
};


static dasync::Rearm control_conn_cb(EventLoop_t * loop, ControlConnWatcher * watcher, int revents)
{
    using Rearm = dasync::Rearm;

    char * cc_addr = (reinterpret_cast<char *>(watcher)) - offsetof(ControlConn, iob);
    ControlConn *conn = reinterpret_cast<ControlConn *>(cc_addr);
    if (revents & in_events) {
        if (conn->dataReady()) {
            // ControlConn was deleted
            return Rearm::REMOVED;
        }
    }
    if (revents & out_events) {
        if (conn->sendData()) {
            return Rearm::REMOVED;
        }
    }
    
    return Rearm::NOOP;
}

#endif
