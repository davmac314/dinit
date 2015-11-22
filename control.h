#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>

#include <unistd.h>
#include <ev++.h>
#include "dinit-log.h"
#include "control-cmds.h"

// Control connection for dinit


// forward-declaration of callback:
static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents);

class ControlConn;

// Pointer to the control connection that is listening for rollback completion
extern ControlConn * rollback_handler_conn;

extern int active_control_conns;

// "packet" format:
// (1 byte) packet type
// (N bytes) additional data (service name, etc)
//   for STARTSERVICE/STOPSERVICE:
//      (2 bytes) service name length
//      (M buyes) service name (without nul terminator)

class ServiceSet;


class ControlConn
{
    friend void control_conn_cb(struct ev_loop *, ev_io *, int);
    
    struct ev_io iob;
    struct ev_loop *loop;
    ServiceSet *service_set;
    char * iobuf;
    int bufidx;
    
    bool bad_conn_close; // close when finished output?
    //bool bad_conn_wrerr; // write error has occurred
    bool oom_close;      // send final 'out of memory' indicator
    
    template <typename T> using list = std::list<T>;
    template <typename T> using vector = std::vector<T>;
    
    // Buffer for outgoing packets. Each outgoing back is represented as a vector<char>.
    list<vector<char>> outbuf;
    unsigned outpkt_index = 0;
    
    // The packet length before we need to re-check if the packet is complete
    int chklen;
    
    // Queue a packet to be sent
    //  Returns:  true if the packet was successfully queued, false if otherwise
    //            (eg if out of memory); in the latter case the connection might
    //            no longer be valid (iff there are no outgoing packets queued).
    bool queuePacket(vector<char> &&v) noexcept;
    bool queuePacket(const char *pkt, unsigned size) noexcept;

    // Process a packet. Can cause the ControlConn to be deleted iff there are no
    // outgoing packets queued.
    void processPacket();

    // Notify that data is ready to be read from the socket.
    void dataReady() noexcept;
    void sendData() noexcept;
    
    public:
    ControlConn(struct ev_loop * loop, ServiceSet * service_set, int fd) : loop(loop), service_set(service_set), bufidx(0), chklen(0)
    {
        iobuf = new char[1024];
    
        ev_io_init(&iob, control_conn_cb, fd, EV_READ);
        iob.data = this;
        ev_io_start(loop, &iob);
        
        active_control_conns++;
    }
    
    bool rollbackComplete() noexcept;
        
    ~ControlConn() noexcept;
};


static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents)
{
    ControlConn *conn = (ControlConn *) w->data;
    if (revents & EV_READ) {
        conn->dataReady();
    }
    // TODO issue here: what if above deletes the connection?
    if (revents & EV_WRITE) {
        conn->sendData();
    }    
}

#endif
