#include <ev++.h>

// Control connection for dinit


// forward-declaration of callback:
static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents);


// Packet types:
constexpr static int DINIT_CP_STARTSERVICE = 0;
constexpr static int DINIT_CP_STOPSERVICE  = 1;

// "packet" format:
// (1 byte) packet type
// (N bytes) additional data (service name, etc)
//   for STARTSERVICE/STOPSERVICE:
//      (2 bytes) service name length
//      (M buyes) service name (without nul terminator)


class ControlConn
{
    struct ev_io iob;
    struct ev_loop *loop;
    ServiceSet *service_set;
    char * iobuf;
    int bufidx;
    
    // The packet length before we need to re-check if the packet is complete
    int chklen;
    
    public:
    ControlConn(struct ev_loop * loop, ServiceSet * service_set, int fd) : loop(loop), service_set(service_set), bufidx(0), chklen(0)
    {
        iobuf = new char[1024];
    
        ev_io_init(&iob, control_conn_cb, fd, EV_READ);
        iob.data = this;
        ev_io_start(loop, &iob);
    }
    
    void processPacket()
    {
        using std::string;
    
        int pktType = iobuf[0];
        if (pktType == DINIT_CP_STARTSERVICE || pktType == DINIT_CP_STOPSERVICE) {
            if (bufidx < 4) {
                chklen = 4;
                return;
            }
            
            uint16_t svcSize;
            memcpy(&svcSize, iobuf + 1, 2);
            if (svcSize <= 0) {
                // TODO error response
                bufidx = 1024; // dataReady will delete - TODO clean up
            }
            
            chklen = svcSize + 3;
            if (chklen > 1024) {
                // We can't have a service name this long
                // TODO error response
                bufidx = 1024; // TODO cleanup.
            }
            
            if (bufidx < chklen) {
                // packet not complete yet; read more
                return;
            }
            
            string serviceName(iobuf + 3, (size_t) svcSize);
            if (pktType == DINIT_CP_STARTSERVICE) {
                service_set->startService(serviceName.c_str());
                // TODO catch exceptions, error response
            }
            else {
                // TODO verify the named service exists?
                service_set->stopService(serviceName.c_str());
            }
            
            // Clear the packet from the buffer
            memmove(iobuf, iobuf + chklen, 1024 - chklen);
            bufidx -= chklen;
            chklen = 0;
            return;
        }
    
    }
    
    void dataReady()
    {
        int fd = iob.fd;
        int buffree = 1024 - bufidx;
        
        int r = read(fd, iobuf + bufidx, buffree);
        
        // Note file descriptor is non-blocking
        if (r == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            // TODO log error
            delete this;
            return;
        }
        
        if (r == 0) {
            delete this;
            return;
        }
        
        bufidx += r;
        buffree -= r;
        
        // complete packet?
        if (bufidx >= chklen) {
            processPacket();
        }
        
        if (bufidx == 1024) {
            // Too big packet
            // TODO log error?
            // TODO error response?
            delete this;
        }
    }
    
    ~ControlConn()
    {
        close(iob.fd);
        ev_io_stop(loop, &iob);
        delete [] iobuf;
    }
};


static void control_conn_cb(struct ev_loop * loop, ev_io * w, int revents)
{
    ControlConn *conn = (ControlConn *) w->data;
    conn->dataReady();
}
