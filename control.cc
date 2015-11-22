#include "control.h"
#include "service.h"

// TODO queuePacket can close connection, so make sure not to touch instance variables after calling it
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
            // Queue error response mark connection bad
            char badreqRep[] = { DINIT_RP_BADREQ };
            if (! queuePacket(badreqRep, 1)) return;
            bad_conn_close = true;
            ev_io_set(&iob, iob.fd, EV_WRITE);
            return;
        }
        
        chklen = svcSize + 3;
        if (chklen > 1024) {
            // We can't have a service name this long
            // TODO error response
            bad_conn_close = true;
            ev_io_set(&iob, iob.fd, EV_WRITE);
            return;
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
                if (! queuePacket(ack_buf, 1)) return;
            }
            catch (ServiceLoadExc &slexc) {
                char outbuf[] = { DINIT_RP_SERVICELOADERR };
                if (! queuePacket(outbuf, 1)) return;
            }
            catch (std::bad_alloc &baexc) {
                char outbuf[] = { DINIT_RP_SERVICEOOM };
                if (! queuePacket(outbuf, 1)) return; // might degenerate to DINIT_RP_OOM, which is fine.
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
            if (! queuePacket(ackBuf, 1)) return;
        }
        else {
            // TODO send NAK
        }
        
        // Clear the packet from the buffer
        memmove(iobuf, iobuf + 1, 1024 - 1);
        bufidx -= 1;
        chklen = 0;
        return;
    }
    else {
        // TODO error response
    }
    return;
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

bool ControlConn::rollbackComplete() noexcept
{
    char ackBuf[1] = { DINIT_ROLLBACK_COMPLETED };
    return queuePacket(ackBuf, 1);
}

void ControlConn::dataReady() noexcept
{
    int fd = iob.fd;
    int buffree = 1024 - bufidx;
    
    int r = read(fd, iobuf + bufidx, buffree);
    
    // Note file descriptor is non-blocking
    if (r == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            // TODO log error
            delete this;
        }
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
    
    return;
}

void ControlConn::sendData() noexcept
{
    if (outbuf.empty() && bad_conn_close) {
        if (oom_close) {
            // Send oom response
            char oomBuf[] = { DINIT_RP_OOM };
            write(iob.fd, oomBuf, 1);
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
            log(LogLevel::ERROR, "Error writing to control connection: ", strerror(errno));
            delete this;
        }
        return;
    }

    outpkt_index += written;
    if (outpkt_index == pkt.size()) {
        // We've finished this packet, move on to the next:
        outbuf.pop_front();
        outpkt_index = 0;
        if (outbuf.empty() && ! oom_close) {
            if (! bad_conn_close) {
                ev_io_set(&iob, iob.fd, EV_READ);
            }
            else {
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
