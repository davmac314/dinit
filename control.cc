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
                char ack_buf[] = { DINIT_RP_ACK };
                service_set->startService(serviceName.c_str());
                queuePacket(ack_buf, 1);
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
            char ackBuf[] = { DINIT_RP_ACK };
            queuePacket(ackBuf, 1);
        }
        else {
            // TODO send NAK
        }
    }
    else {
        // TODO error response
    }
}

bool ControlConn::queuePacket(const char *pkt, unsigned size) noexcept
{
    if (bad_conn_close) return false;

    bool was_empty = outbuf.empty();

    if (was_empty) {
        int wr = write(iob.fd, pkt, size);
        if (wr == -1) {
            if (errno == EPIPE) {
                delete this;
                return false;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // TODO log error
                delete this;
                return false;
            }
        }
        else {
            if ((unsigned)wr == size) {
                // Ok, all written.
                return true;
            }
            pkt += wr;
            size -= wr;
        }
        ev_io_set(&iob, iob.fd, EV_READ | EV_WRITE);
    }
    
    // Create a vector out of the (remaining part of the) packet:
    try {
        outbuf.emplace_back(pkt, pkt + size);
        return true;
    }
    catch (std::bad_alloc &baexc) {
        // Mark the connection bad, and stop reading further requests
        bad_conn_close = true;
        oom_close = true;
        if (was_empty) {
            // We can't send out-of-memory response as we already wrote as much as we
            // could above. Neither can we later send the response since we have currently
            // sent an incomplete packet. All we can do is close the connection.
            delete this;
        }
        else {
            ev_io_set(&iob, iob.fd, EV_WRITE);
        }
        return false;    
    }
}


bool ControlConn::queuePacket(std::vector<char> &&pkt) noexcept
{
    if (bad_conn_close) return false;

    bool was_empty = outbuf.empty();
    
    if (was_empty) {
        outpkt_index = 0;
        // We can try sending the packet immediately:
        int wr = write(iob.fd, pkt.data(), pkt.size());
        if (wr == -1) {
            if (errno == EPIPE) {
                delete this;
                return false;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // TODO log error
                delete this;
                return false;
            }
        }
        else {
            if ((unsigned)wr == pkt.size()) {
                // Ok, all written.
                return true;
            }
            outpkt_index = wr;
        }
        ev_io_set(&iob, iob.fd, EV_READ | EV_WRITE);
    }
    
    try {
        outbuf.emplace_back(pkt);
        return true;
    }
    catch (std::bad_alloc &baexc) {
        // Mark the connection bad, and stop reading further requests
        bad_conn_close = true;
        oom_close = true;
        if (was_empty) {
            // We can't send out-of-memory response as we already wrote as much as we
            // could above. Neither can we later send the response since we have currently
            // sent an incomplete packet. All we can do is close the connection.
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
    char ackBuf[1] = { DINIT_ROLLBACK_COMPLETED };
    queuePacket(ackBuf, 1);
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
