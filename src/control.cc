#include "control.h"
#include "service.h"

// Server-side control protocol implementation. This implements the functionality that allows
// clients (such as dinitctl) to query service state and issue commands to control services.

namespace {
    constexpr auto OUT_EVENTS = dasynq::OUT_EVENTS;
    constexpr auto IN_EVENTS = dasynq::IN_EVENTS;

    // Control protocol minimum compatible version and current version:
    constexpr uint16_t min_compat_version = 1;
    constexpr uint16_t cp_version = 1;
}

bool control_conn_t::process_packet()
{
    using std::string;
    
    // Note that where we call queue_packet, we must generally check the return value. If it
    // returns false it has either deleted the connection or marked it for deletion; we
    // shouldn't touch instance members after that point.

    int pktType = rbuf[0];
    if (pktType == DINIT_CP_QUERYVERSION) {
        // Responds with:
        // DINIT_RP_CVERSION, (2 byte) minimum compatible version, (2 byte) actual version
        char replyBuf[] = { DINIT_RP_CPVERSION, 0, 0, 0, 0 };
        memcpy(replyBuf + 1, &min_compat_version, 2);
        memcpy(replyBuf + 3, &cp_version, 2);
        if (! queue_packet(replyBuf, sizeof(replyBuf))) return false;
        rbuf.consume(1);
        return true;
    }
    if (pktType == DINIT_CP_FINDSERVICE || pktType == DINIT_CP_LOADSERVICE) {
        return process_find_load(pktType);
    }
    if (pktType == DINIT_CP_STARTSERVICE || pktType == DINIT_CP_STOPSERVICE
            || pktType == DINIT_CP_WAKESERVICE || pktType == DINIT_CP_RELEASESERVICE) {
        return process_start_stop(pktType);
    }
    if (pktType == DINIT_CP_UNPINSERVICE) {
        return process_unpin_service();
    }
    if (pktType == DINIT_CP_UNLOADSERVICE) {
        return process_unload_service();
    }
    if (pktType == DINIT_CP_SHUTDOWN) {
        // Shutdown/reboot
        if (rbuf.get_length() < 2) {
            chklen = 2;
            return true;
        }
        
        auto sd_type = static_cast<shutdown_type_t>(rbuf[1]);
        
        services->stop_all_services(sd_type);
        char ackBuf[] = { DINIT_RP_ACK };
        if (! queue_packet(ackBuf, 1)) return false;
        
        // Clear the packet from the buffer
        rbuf.consume(2);
        chklen = 0;
        return true;
    }
    if (pktType == DINIT_CP_LISTSERVICES) {
        return list_services();
    }
    else {
        // Unrecognized: give error response
        char outbuf[] = { DINIT_RP_BADREQ };
        if (! queue_packet(outbuf, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
    }
    return true;
}

bool control_conn_t::process_find_load(int pktType)
{
    using std::string;
    
    constexpr int pkt_size = 4;
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }
    
    uint16_t svcSize;
    rbuf.extract((char *)&svcSize, 1, 2);
    chklen = svcSize + 3; // packet type + (2 byte) length + service name
    if (svcSize <= 0 || chklen > 1024) {
        // Queue error response / mark connection bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }
    
    if (rbuf.get_length() < chklen) {
        // packet not complete yet; read more
        return true;
    }
    
    service_record * record = nullptr;
    
    string serviceName = rbuf.extract_string(3, svcSize);
    
    if (pktType == DINIT_CP_LOADSERVICE) {
        // LOADSERVICE
        try {
            record = services->load_service(serviceName.c_str());
        }
        catch (service_load_exc &slexc) {
            log(loglevel_t::ERROR, "Could not load service ", slexc.serviceName, ": ", slexc.excDescription);
        }
    }
    else {
        // FINDSERVICE
        record = services->find_service(serviceName.c_str());
    }
    
    if (record != nullptr) {
        // Allocate a service handle
        handle_t handle = allocate_service_handle(record);
        std::vector<char> rp_buf;
        rp_buf.reserve(7);
        rp_buf.push_back(DINIT_RP_SERVICERECORD);
        rp_buf.push_back(static_cast<char>(record->get_state()));
        for (int i = 0; i < (int) sizeof(handle); i++) {
            rp_buf.push_back(*(((char *) &handle) + i));
        }
        rp_buf.push_back(static_cast<char>(record->get_target_state()));
        if (! queue_packet(std::move(rp_buf))) return false;
    }
    else {
        std::vector<char> rp_buf = { DINIT_RP_NOSERVICE };
        if (! queue_packet(std::move(rp_buf))) return false;
    }
    
    // Clear the packet from the buffer
    rbuf.consume(chklen);
    chklen = 0;
    return true;
}

bool control_conn_t::process_start_stop(int pktType)
{
    using std::string;
    
    constexpr int pkt_size = 2 + sizeof(handle_t);
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }
    
    // 1 byte: packet type
    // 1 byte: pin in requested state (0 = no pin, 1 = pin)
    // 4 bytes: service handle
    
    bool do_pin = (rbuf[1] == 1);
    handle_t handle;
    rbuf.extract((char *) &handle, 2, sizeof(handle));
    
    service_record *service = find_service_for_key(handle);
    if (service == nullptr) {
        // Service handle is bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }
    else {
        bool already_there = false;
        
        switch (pktType) {
        case DINIT_CP_STARTSERVICE:
            // start service, mark as required
            if (do_pin) service->pin_start();
            service->start();
            services->process_queues();
            already_there = service->get_state() == service_state_t::STARTED;
            break;
        case DINIT_CP_STOPSERVICE:
            // force service to stop
            if (do_pin) service->pin_stop();
            service->stop(true);
            service->forced_stop();
            services->process_queues();
            already_there = service->get_state() == service_state_t::STOPPED;
            break;
        case DINIT_CP_WAKESERVICE:
            // re-start a stopped service (do not mark as required)
            if (do_pin) service->pin_start();
            service->start(false);
            services->process_queues();
            already_there = service->get_state() == service_state_t::STARTED;
            break;
        case DINIT_CP_RELEASESERVICE:
            // remove required mark, stop if not required by dependents
            if (do_pin) service->pin_stop();
            service->stop(false);
            services->process_queues();
            already_there = service->get_state() == service_state_t::STOPPED;
            break;
        }
        
        char ack_buf[] = { (char)(already_there ? DINIT_RP_ALREADYSS : DINIT_RP_ACK) };
        
        if (! queue_packet(ack_buf, 1)) return false;
    }
    
    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

bool control_conn_t::process_unpin_service()
{
    using std::string;
    
    constexpr int pkt_size = 1 + sizeof(handle_t);
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }
    
    // 1 byte: packet type
    // 4 bytes: service handle
    
    handle_t handle;
    rbuf.extract((char *) &handle, 1, sizeof(handle));
    
    service_record *service = find_service_for_key(handle);
    if (service == nullptr) {
        // Service handle is bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }

    service->unpin();
    services->process_queues();
    char ack_buf[] = { (char) DINIT_RP_ACK };
    if (! queue_packet(ack_buf, 1)) return false;
    
    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

bool control_conn_t::process_unload_service()
{
    using std::string;

    constexpr int pkt_size = 1 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    // 1 byte: packet type
    // 4 bytes: service handle

    handle_t handle;
    rbuf.extract((char *) &handle, 1, sizeof(handle));

    service_record *service = find_service_for_key(handle);
    if (service == nullptr) {
        // Service handle is bad
        char badreq_rep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }

    if (! service->has_lone_ref() || service->get_state() != service_state_t::STOPPED) {
        // Cannot unload: has other references
        char nak_rep[] = { DINIT_RP_NAK };
        if (! queue_packet(nak_rep, 1)) return false;
    }
    else {
        // unload
        service->prepare_for_unload();
        services->remove_service(service);
        delete service;

        // drop handle
        service_key_map.erase(service);
        key_service_map.erase(handle);

        // send ack
        char ack_buf[] = { (char) DINIT_RP_ACK };
        if (! queue_packet(ack_buf, 1)) return false;
    }

    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

bool control_conn_t::list_services()
{
    rbuf.consume(1); // clear request packet
    chklen = 0;
    
    try {
        auto slist = services->list_services();
        for (auto sptr : slist) {
            std::vector<char> pkt_buf;
            
            int hdrsize = 8 + std::max(sizeof(int), sizeof(pid_t));

            const std::string &name = sptr->get_name();
            int nameLen = std::min((size_t)256, name.length());
            pkt_buf.resize(hdrsize + nameLen);
            
            pkt_buf[0] = DINIT_RP_SVCINFO;
            pkt_buf[1] = nameLen;
            pkt_buf[2] = static_cast<char>(sptr->get_state());
            pkt_buf[3] = static_cast<char>(sptr->get_target_state());
            
            char b0 = sptr->is_waiting_for_console() ? 1 : 0;
            b0 |= sptr->has_console() ? 2 : 0;
            b0 |= sptr->was_start_skipped() ? 4 : 0;
            pkt_buf[4] = b0;
            pkt_buf[5] = static_cast<char>(sptr->get_stop_reason());

            pkt_buf[6] = 0; // reserved
            pkt_buf[7] = 0;
            
            // Next: either the exit status, or the process ID
            if (sptr->get_state() != service_state_t::STOPPED) {
                pid_t proc_pid = sptr->get_pid();
                memcpy(pkt_buf.data() + 8, &proc_pid, sizeof(proc_pid));
            }
            else {
                int exit_status = sptr->get_exit_status();
                memcpy(pkt_buf.data() + 8, &exit_status, sizeof(exit_status));
            }

            for (int i = 0; i < nameLen; i++) {
                pkt_buf[hdrsize+i] = name[i];
            }
            
            if (! queue_packet(std::move(pkt_buf))) return false;
        }
        
        char ack_buf[] = { (char) DINIT_RP_LISTDONE };
        if (! queue_packet(ack_buf, 1)) return false;
        
        return true;
    }
    catch (std::bad_alloc &exc)
    {
        do_oom_close();
        return true;
    }
}

control_conn_t::handle_t control_conn_t::allocate_service_handle(service_record *record)
{
    // Try to find a unique handle (integer) in a single pass. Since the map is ordered, we can search until
    // we find a gap in the handle values.
    handle_t candidate = 0;
    for (auto p : key_service_map) {
        if (p.first == candidate) candidate++;
        else break;
    }

    bool is_unique = (service_key_map.find(record) == service_key_map.end());

    // The following operations perform allocation (can throw std::bad_alloc). If an exception occurs we
    // must undo any previous actions:
    if (is_unique) {
        record->add_listener(this);
    }
    
    try {
        key_service_map[candidate] = record;
        service_key_map.insert(std::make_pair(record, candidate));
    }
    catch (...) {
        if (is_unique) {
            record->remove_listener(this);
        }

        key_service_map.erase(candidate);
    }
    
    return candidate;
}

bool control_conn_t::queue_packet(const char *pkt, unsigned size) noexcept
{
    int in_flag = bad_conn_close ? 0 : IN_EVENTS;
    bool was_empty = outbuf.empty();

    // If the queue is empty, we can try to write the packet out now rather than queueing it.
    // If the write is unsuccessful or partial, we queue the remainder.
    if (was_empty) {
        int wr = write(iob.get_watched_fd(), pkt, size);
        if (wr == -1) {
            if (errno == EPIPE) {
                return false;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                log(loglevel_t::WARN, "Error writing to control connection: ", strerror(errno));
                return false;
            }
            // EAGAIN etc: fall through to below
        }
        else {
            if ((unsigned)wr == size) {
                // Ok, all written.
                iob.set_watches(in_flag);
                return true;
            }
            pkt += wr;
            size -= wr;
        }
    }
    
    // Create a vector out of the (remaining part of the) packet:
    try {
        outbuf.emplace_back(pkt, pkt + size);
        iob.set_watches(in_flag | OUT_EVENTS);
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
            return false;
        }
        else {
            iob.set_watches(OUT_EVENTS);
            return true;
        }
    }
}

// This queue_packet method is frustratingly similar to the one above, but the subtle differences
// make them extraordinary difficult to combine into a single method.
bool control_conn_t::queue_packet(std::vector<char> &&pkt) noexcept
{
    int in_flag = bad_conn_close ? 0 : IN_EVENTS;
    bool was_empty = outbuf.empty();
    
    if (was_empty) {
        outpkt_index = 0;
        // We can try sending the packet immediately:
        int wr = write(iob.get_watched_fd(), pkt.data(), pkt.size());
        if (wr == -1) {
            if (errno == EPIPE) {
                return false;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                log(loglevel_t::WARN, "Error writing to control connection: ", strerror(errno));
                return false;
            }
            // EAGAIN etc: fall through to below
        }
        else {
            if ((unsigned)wr == pkt.size()) {
                // Ok, all written.
                iob.set_watches(in_flag);
                return true;
            }
            outpkt_index = wr;
        }
    }
    
    try {
        outbuf.emplace_back(pkt);
        iob.set_watches(in_flag | OUT_EVENTS);
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
            return false;
        }
        else {
            iob.set_watches(OUT_EVENTS);
            return true;
        }
    }
}

bool control_conn_t::rollback_complete() noexcept
{
    char ackBuf[2] = { DINIT_ROLLBACK_COMPLETED, 2 };
    return queue_packet(ackBuf, 2);
}

bool control_conn_t::data_ready() noexcept
{
    int fd = iob.get_watched_fd();
    
    int r = rbuf.fill(fd);
    
    // Note file descriptor is non-blocking
    if (r == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            log(loglevel_t::WARN, "Error writing to control connection: ", strerror(errno));
            return true;
        }
        return false;
    }
    
    if (r == 0) {
        return true;
    }
    
    // complete packet?
    if (rbuf.get_length() >= chklen) {
        try {
            return !process_packet();
        }
        catch (std::bad_alloc &baexc) {
            do_oom_close();
            return false;
        }
    }
    else if (rbuf.get_length() == 1024) {
        // Too big packet
        log(loglevel_t::WARN, "Received too-large control package; dropping connection");
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
    }
    else {
        int out_flags = (bad_conn_close || !outbuf.empty()) ? OUT_EVENTS : 0;
        iob.set_watches(IN_EVENTS | out_flags);
    }
    
    return false;
}

bool control_conn_t::send_data() noexcept
{
    if (outbuf.empty() && bad_conn_close) {
        if (oom_close) {
            // Send oom response
            char oomBuf[] = { DINIT_RP_OOM };
            write(iob.get_watched_fd(), oomBuf, 1);
        }
        return true;
    }
    
    vector<char> & pkt = outbuf.front();
    char *data = pkt.data();
    int written = write(iob.get_watched_fd(), data + outpkt_index, pkt.size() - outpkt_index);
    if (written == -1) {
        if (errno == EPIPE) {
            // read end closed
            return true;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            // spurious readiness notification?
        }
        else {
            log(loglevel_t::ERROR, "Error writing to control connection: ", strerror(errno));
            return true;
        }
        return false;
    }

    outpkt_index += written;
    if (outpkt_index == pkt.size()) {
        // We've finished this packet, move on to the next:
        outbuf.pop_front();
        outpkt_index = 0;
        if (outbuf.empty() && ! oom_close) {
            if (! bad_conn_close) {
                iob.set_watches(IN_EVENTS);
            }
            else {
                return true;
            }
        }
    }
    
    return false;
}

control_conn_t::~control_conn_t() noexcept
{
    close(iob.get_watched_fd());
    iob.deregister(loop);
    
    // Clear service listeners
    for (auto p : service_key_map) {
        p.first->remove_listener(this);
    }
    
    active_control_conns--;
}
