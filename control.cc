#include "control.h"
#include "service.h"

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
            // TODO do not allow services to be started during system shutdown
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

void ControlConn::rollbackComplete()
{
    char ackBuf[1] = { DINIT_RP_COMPLETED };
    if (write(iob.fd, ackBuf, 1) == -1) {
        // TODO queue or at least re-try if error or 0 bytes written.
        log(LogLevel::ERROR, "Couldn't write response to control socket");
    }
}

void ControlConn::dataReady()
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

ControlConn::~ControlConn()
{
    close(iob.fd);
    ev_io_stop(loop, &iob);
    delete [] iobuf;
    service_set->clearRollbackHandler(this);
    active_control_conns--;
}
