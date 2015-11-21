#include "control.h"
#include "service.h"

// TODO at the moment we rely on the readiness notification to send "bad packet" responses.
//   It would probably be better to, if the outgoing buffer is empty, try and send the packet
//   immediately and only queue it if necessary. However this means we would potentially
//   delete 'this' object which needs to be accounted for in calling methods. (Might be better
//   to return a bool indicating that delete is required).
void ControlConn::processPacket()
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
            // TODO queue error response
            bad_conn_close = true;
            ev_io_set(&iob, iob.fd, EV_WRITE);
        }
        
        chklen = svcSize + 3;
        if (chklen > 1024) {
            // We can't have a service name this long
            // TODO error response
            bad_conn_close = true;
            ev_io_set(&iob, iob.fd, EV_WRITE);
        }
        
        if (bufidx < chklen) {
            // packet not complete yet; read more
            return;
        }
        
        string serviceName(iobuf + 3, (size_t) svcSize);
        if (pktType == DINIT_CP_STARTSERVICE) {
            // TODO do not allow services to be started during system shutdown
            try {
                service_set->startService(serviceName.c_str());
                // TODO ack response
            }
            catch (ServiceLoadExc &slexc) {
                // TODO error response
            }
            catch (std::bad_alloc &baexc) {
                // TODO error response
            }
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
    else if (pktType == DINIT_CP_ROLLBACKALL) {
        // Roll-back all services
        if (service_set->setRollbackHandler(this)) {
            service_set->stop_all_services();
            log_to_console = true;
            // TODO send ACK
        }
        else {
            // TODO send NAK
        }
    }
    else {
        // TODO error response
    }
}

bool ControlConn::queuePacket(std::vector<char> &&pkt) noexcept
{
    bool was_empty = outbuf.empty();
    try {
        outbuf.emplace_back(pkt);
        if (was_empty) {
            ev_io_set(&iob, iob.fd, EV_READ | EV_WRITE);
        }
        return true;
    }
    catch (std::bad_alloc &baexc) {
        // Mark the connection bad, and stop reading further requests
        bad_conn_close = true;
        oom_close = true;
        if (was_empty) {
            // TODO send out-of-memory response
            delete this;
        }
        else {
            ev_io_set(&iob, iob.fd, EV_WRITE);
        }
        return false;
    }
}

void ControlConn::rollbackComplete() noexcept
{
    char ackBuf[1] = { DINIT_RP_COMPLETED };
    // TODO Queue response instead of trying to write it directly like this
    if (write(iob.fd, ackBuf, 1) == -1) {
        log(LogLevel::ERROR, "Couldn't write response to control socket");
        delete this;
    }
}

void ControlConn::dataReady() noexcept
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
        try {
            processPacket();
        }
        catch (std::bad_alloc &baexc) {
            // TODO
        }
    }
    
    if (bufidx == 1024) {
        // Too big packet
        // TODO log error?
        // TODO error response?
        bad_conn_close = true;
        ev_io_set(&iob, iob.fd, EV_WRITE);
    }
}

void ControlConn::sendData() noexcept
{
    if (outbuf.empty() && bad_conn_close) {
        if (oom_close) {
            // TODO send oom response
        }
        delete this;
        return;
    }
    
    vector<char> & pkt = outbuf.front();
    char *data = pkt.data();
    int written = write(iob.fd, data + outpkt_index, pkt.size() - outpkt_index);
    if (written == -1) {
        if (errno == EPIPE) {
            // read end closed
            delete this;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // spurious readiness notification?
        }
        else {
            // TODO log error
            delete this;
        }
        return;
    }

    outpkt_index += written;
    if (outpkt_index == pkt.size()) {
        // We've finished this packet, move on to the next:
        outbuf.pop_front();
        outpkt_index = 0;
        if (outbuf.empty()) {
            if (! bad_conn_close) {
                ev_io_set(&iob, iob.fd, EV_READ);
            }
            else {
                if (oom_close) {
                    // TODO send out-of-memory reply if possible
                }
                delete this;
            }
        }
    }
}

ControlConn::~ControlConn() noexcept
{
    close(iob.fd);
    ev_io_stop(loop, &iob);
    delete [] iobuf;
    service_set->clearRollbackHandler(this);
    active_control_conns--;
}
