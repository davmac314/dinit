#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>
#include <unordered_map>
#include <limits>

#include <unistd.h>
#include <ev++.h>

#include "dinit-log.h"
#include "control-cmds.h"
#include "service-listener.h"
#include "cpbuffer.h"

// Control connection for dinit

// TODO: Use the input buffer as a circular buffer, instead of chomping data from
// the front using a data move.

// forward-declaration of callback:
static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents);

class ControlConn;

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

class ControlConn : private ServiceListener
{
    friend void control_conn_cb(struct ev_loop *, ev_io *, int);
    
    struct ev_io iob;
    struct ev_loop *loop;
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
    //  Returns:  true if the packet was successfully queued, false if otherwise
    //            (eg if out of memory); in the latter case the connection might
    //            no longer be valid (iff there are no outgoing packets queued).
    bool queuePacket(vector<char> &&v) noexcept;
    bool queuePacket(const char *pkt, unsigned size) noexcept;

    // Process a packet. Can cause the ControlConn to be deleted iff there are no
    // outgoing packets queued.
    // Throws:
    //    std::bad_alloc - if an out-of-memory condition prevents processing
    void processPacket();
    
    // Process a STARTSERVICE/STOPSERVICE packet. May throw std::bad_alloc.
    void processStartStop(int pktType);
    
    // Process a FINDSERVICE/LOADSERVICE packet. May throw std::bad_alloc.
    void processFindLoad(int pktType);

    // Process an UNPINSERVICE packet. May throw std::bad_alloc.
    void processUnpinService();

    // Notify that data is ready to be read from the socket. Returns true in cases where the
    // connection was deleted with potentially pending outgoing packets.
    bool dataReady() noexcept;
    
    void sendData() noexcept;
    
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
        ev_io_set(&iob, iob.fd, EV_WRITE);
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
    ControlConn(struct ev_loop * loop, ServiceSet * service_set, int fd) : loop(loop), service_set(service_set), chklen(0)
    {
        ev_io_init(&iob, control_conn_cb, fd, EV_READ);
        iob.data = this;
        ev_io_start(loop, &iob);
        
        active_control_conns++;
    }
    
    bool rollbackComplete() noexcept;
        
    virtual ~ControlConn() noexcept;
};


static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents)
{
    ControlConn *conn = (ControlConn *) w->data;
    if (revents & EV_READ) {
        if (conn->dataReady()) {
            // ControlConn was deleted
            return;
        }
    }
    if (revents & EV_WRITE) {
        conn->sendData();
    }    
}

#endif
