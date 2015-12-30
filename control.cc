#include "control.h"
#include "service.h"

void ControlConn::processPacket()
{
    using std::string;
    
    // Note that where we call queuePacket, we must generally check the return value. If it
    // returns false it has either deleted the connection or marked it for deletion; we
    // shouldn't touch instance members after that point.

    int pktType = rbuf[0];
    if (pktType == DINIT_CP_QUERYVERSION) {
        // Responds with:
        // DINIT_RP_CVERSION, (2 byte) minimum compatible version, (2 byte) maximum compatible version
        char replyBuf[] = { DINIT_RP_CPVERSION, 0, 0, 0, 0 };
        if (! queuePacket(replyBuf, 1)) return;
        rbuf.consume(1);
        return;
    }
    if (pktType == DINIT_CP_FINDSERVICE || pktType == DINIT_CP_LOADSERVICE) {
        processFindLoad(pktType);
        return;
    }
    if (pktType == DINIT_CP_STARTSERVICE || pktType == DINIT_CP_STOPSERVICE) {
        processStartStop(pktType);
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
            char nakBuf[] = { DINIT_RP_NAK };
            if (! queuePacket(nakBuf, 1)) return;
        }
        
        // Clear the packet from the buffer
        rbuf.consume(1);
        chklen = 0;
        return;
    }
    else {
        // Unrecognized: give error response
        char outbuf[] = { DINIT_RP_BADREQ };
        if (! queuePacket(outbuf, 1)) return;
        bad_conn_close = true;
        ev_io_set(&iob, iob.fd, EV_WRITE);
    }
    return;
}

void ControlConn::processFindLoad(int pktType)
{
    using std::string;
    
    constexpr int pkt_size = 4;
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return;
    }
    
    uint16_t svcSize;
    rbuf.extract((char *)&svcSize, 1, 2);
    chklen = svcSize + 3; // packet type + (2 byte) length + service name
    if (svcSize <= 0 || chklen > 1024) {
        // Queue error response / mark connection bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queuePacket(badreqRep, 1)) return;
        bad_conn_close = true;
        ev_io_set(&iob, iob.fd, EV_WRITE);
        return;
    }
    
    if (rbuf.get_length() < chklen) {
        // packet not complete yet; read more
        return;
    }
    
    ServiceRecord * record = nullptr;
    
    string serviceName = std::move(rbuf.extract_string(3, svcSize));
    
    if (pktType == DINIT_CP_LOADSERVICE) {
        // LOADSERVICE
        try {
            record = service_set->loadService(serviceName);
        }
        catch (ServiceLoadExc &slexc) {
            log(LogLevel::ERROR, "Could not load service ", slexc.serviceName, ": ", slexc.excDescription);
        }
    }
    else {
        // FINDSERVICE
        record = service_set->findService(serviceName.c_str());
    }
    
    if (record != nullptr) {
        // Allocate a service handle
        handle_t handle = allocateServiceHandle(record);
        std::vector<char> rp_buf;
        rp_buf.reserve(7);
        rp_buf.push_back(DINIT_RP_SERVICERECORD);
        rp_buf.push_back(static_cast<char>(record->getState()));
        for (int i = 0; i < (int) sizeof(handle); i++) {
            rp_buf.push_back(*(((char *) &handle) + i));
        }
        rp_buf.push_back(static_cast<char>(record->getTargetState()));
        if (! queuePacket(std::move(rp_buf))) return;
    }
    else {
        std::vector<char> rp_buf = { DINIT_RP_NOSERVICE };
        if (! queuePacket(std::move(rp_buf))) return;
    }
    
    // Clear the packet from the buffer
    rbuf.consume(chklen);
    chklen = 0;
    return;
}

void ControlConn::processStartStop(int pktType)
{
    using std::string;
    
    constexpr int pkt_size = 2 + sizeof(handle_t);
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return;
    }
    
    // 1 byte: packet type
    // 1 byte: pin in requested state (0 = no pin, 1 = pin)
    // 4 bytes: service handle
    
    bool do_pin = (rbuf[1] == 1);
    handle_t handle;
    rbuf.extract((char *) &handle, 2, sizeof(handle));
    
    ServiceRecord *service = findServiceForKey(handle);
    if (service == nullptr) {
        // Service handle is bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queuePacket(badreqRep, 1)) return;
        bad_conn_close = true;
        ev_io_set(&iob, iob.fd, EV_WRITE);
        return;
    }
    else {
        if (pktType == DINIT_CP_STARTSERVICE) {
            if (do_pin) {
                service->pinStart();
            }
            else {
                service->start();
            }
        }
        else {
            if (do_pin) {
                service->pinStop();
            }
            else {
                service->stop();
            }
        }
        
        char ack_buf[] = { DINIT_RP_ACK };
        if (! queuePacket(ack_buf, 1)) return;
    }
    
    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return;
}

ControlConn::handle_t ControlConn::allocateServiceHandle(ServiceRecord *record)
{
    bool is_unique = true;
    handle_t largest_seen = 0;
    handle_t candidate = 0;
    for (auto p : keyServiceMap) {
        if (p.first > largest_seen) largest_seen = p.first;
        if (p.first == candidate) {
            if (largest_seen == std::numeric_limits<handle_t>::max()) throw std::bad_alloc();
            candidate = largest_seen + 1;
        }
        is_unique &= (p.second != record);
    }
    
    keyServiceMap[candidate] = record;
    serviceKeyMap.insert(std::make_pair(record, candidate));
    
    if (is_unique) {
        record->addListener(this);
    }
    
    return candidate;
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
    char ackBuf[2] = { DINIT_ROLLBACK_COMPLETED, 2 };
    return queuePacket(ackBuf, 2);
}

bool ControlConn::dataReady() noexcept
{
    int fd = iob.fd;
    
    int r = rbuf.fill(fd);
    
    // Note file descriptor is non-blocking
    if (r == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            // TODO log error
            delete this;
            return true;
        }
        return false;
    }
    
    if (r == 0) {
        delete this;
        return true;
    }
    
    // complete packet?
    if (rbuf.get_length() >= chklen) {
        try {
            processPacket();
        }
        catch (std::bad_alloc &baexc) {
            doOomClose();
        }
    }
    
    if (rbuf.get_length() == 1024) {
        // Too big packet
        // TODO log error?
        // TODO error response?
        bad_conn_close = true;
        ev_io_set(&iob, iob.fd, EV_WRITE);
    }
    
    return false;
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
    
    // Clear service listeners
    for (auto p : serviceKeyMap) {
        p.first->removeListener(this);
    }
    
    service_set->clearRollbackHandler(this);
    active_control_conns--;
}
