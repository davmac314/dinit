#include <algorithm>
#include <unordered_set>
#include <climits>

#include "control.h"
#include "service.h"
#include "proc-service.h"

// Server-side control protocol implementation. This implements the functionality that allows
// clients (such as dinitctl) to query service state and issue commands to control services.

namespace {
    constexpr auto OUT_EVENTS = dasynq::OUT_EVENTS;
    constexpr auto IN_EVENTS = dasynq::IN_EVENTS;

    // Control protocol minimum compatible version and current version:
    constexpr uint16_t min_compat_version = 1;
    constexpr uint16_t cp_version = 1;

    // check for value in a set
    template <typename T, int N, typename U>
    inline bool contains(const T (&v)[N], U i)
    {
        return std::find_if(std::begin(v), std::end(v),
                [=](T p){ return i == static_cast<U>(p); }) != std::end(v);
    }
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
    if (pktType == DINIT_CP_RELOADSERVICE) {
        return process_reload_service();
    }
    if (pktType == DINIT_CP_SHUTDOWN) {
        // Shutdown/reboot
        if (rbuf.get_length() < 2) {
            chklen = 2;
            return true;
        }
        
        if (contains({shutdown_type_t::REMAIN, shutdown_type_t::HALT,
            	shutdown_type_t::POWEROFF, shutdown_type_t::REBOOT}, rbuf[1])) {
            auto sd_type = static_cast<shutdown_type_t>(rbuf[1]);

            services->stop_all_services(sd_type);
            char ackBuf[] = { DINIT_RP_ACK };
            if (! queue_packet(ackBuf, 1)) return false;

            // Clear the packet from the buffer
            rbuf.consume(2);
            chklen = 0;
            return true;
        }

        // (otherwise fall through to below).
    }
    if (pktType == DINIT_CP_LISTSERVICES) {
        return list_services();
    }
    if (pktType == DINIT_CP_SERVICESTATUS) {
        return process_service_status();
    }
    if (pktType == DINIT_CP_ADD_DEP) {
        return add_service_dep();
    }
    if (pktType == DINIT_CP_REM_DEP) {
        return rm_service_dep();
    }
    if (pktType == DINIT_CP_QUERY_LOAD_MECH) {
        return query_load_mech();
    }
    if (pktType == DINIT_CP_ENABLESERVICE) {
        return add_service_dep(true);
    }
    if (pktType == DINIT_CP_QUERYSERVICENAME) {
        return process_query_name();
    }
    if (pktType == DINIT_CP_SETENV) {
        return process_setenv();
    }

    // Unrecognized: give error response
    char outbuf[] = { DINIT_RP_BADREQ };
    if (! queue_packet(outbuf, 1)) return false;
    bad_conn_close = true;
    iob.set_watches(OUT_EVENTS);
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
    if (svcSize <= 0 || svcSize > (1024 - 3)) {
        // Queue error response / mark connection bad
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }
    chklen = svcSize + 3; // packet type + (2 byte) length + service name
    
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
            log(loglevel_t::ERROR, "Could not load service ", slexc.service_name, ": ",
                    slexc.exc_description);
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

bool control_conn_t::check_dependents(service_record *service, bool &had_dependents)
{
    std::vector<char> reply_pkt;
    size_t num_depts = 0;

    for (service_dep *dep : service->get_dependents()) {
        if (dep->dep_type == dependency_type::REGULAR && dep->holding_acq) {
            num_depts++;
            // find or allocate a service handle
            handle_t dept_handle = allocate_service_handle(dep->get_from());
            if (reply_pkt.empty()) {
                // packet type, size
                reply_pkt.reserve(1 + sizeof(size_t) + sizeof(handle_t));
                reply_pkt.resize(1 + sizeof(size_t));
                reply_pkt[0] = DINIT_RP_DEPENDENTS;
            }
            auto old_size = reply_pkt.size();
            reply_pkt.resize(old_size + sizeof(handle_t));
            memcpy(reply_pkt.data() + old_size, &dept_handle, sizeof(dept_handle));
        }
    }

    if (num_depts != 0) {
        // There are affected dependents
        had_dependents = true;
        memcpy(reply_pkt.data() + 1, &num_depts, sizeof(num_depts));
        return queue_packet(std::move(reply_pkt));
    }

    had_dependents = false;
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
    // 1 byte: flags eg. pin in requested state (0 = no pin, 1 = pin)
    // 4 bytes: service handle
    
    bool do_pin = ((rbuf[1] & 1) == 1);
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
        char ack_buf[1] = { DINIT_RP_ACK };
        
        switch (pktType) {
        case DINIT_CP_STARTSERVICE:
            // start service, mark as required
            if (services->is_shutting_down()) {
                ack_buf[0] = DINIT_RP_SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STOPPED
                    || service->get_state() == service_state_t::STOPPING)
                    && service->is_stop_pinned()) {
                ack_buf[0] = DINIT_RP_PINNEDSTOPPED;
                break;
            }
            if (do_pin) service->pin_start();
            service->start();
            services->process_queues();
            if (service->get_state() == service_state_t::STARTED) ack_buf[0] = DINIT_RP_ALREADYSS;
            break;
        case DINIT_CP_STOPSERVICE:
        {
            // force service to stop
            bool do_restart = ((rbuf[1] & 4) == 4);
            bool gentle = ((rbuf[1] & 2) == 2) || do_restart;  // restart is always "gentle"
            if (do_restart && services->is_shutting_down()) {
                ack_buf[0] = DINIT_RP_SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STARTED
                    || service->get_state() == service_state_t::STARTING)
                    && service->is_start_pinned()) {
                ack_buf[0] = DINIT_RP_PINNEDSTARTED;
                break;
            }
            if (gentle) {
                // Check dependents; return appropriate response if any will be affected
                bool has_dependents;
                if (! check_dependents(service, has_dependents)) {
                    return false;
                }
                if (has_dependents) {
                    // Reply packet has already been sent
                    goto clear_out;
                }
            }
            service_state_t wanted_state;
            if (do_restart) {
                if (! service->restart()) {
                    ack_buf[0] = DINIT_RP_NAK;
                    break;
                }
                wanted_state = service_state_t::STARTED;
            }
            else {
                if (do_pin) service->pin_stop();
                service->stop(true);
                service->forced_stop();
                wanted_state = service_state_t::STOPPED;
            }
            services->process_queues();
            if (service->get_state() == wanted_state && !do_restart) ack_buf[0] = DINIT_RP_ALREADYSS;
            break;
        }
        case DINIT_CP_WAKESERVICE:
        {
            // re-attach a service to its (started) dependents, causing it to start.
            if (services->is_shutting_down()) {
                ack_buf[0] = DINIT_RP_SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STOPPED
                    || service->get_state() == service_state_t::STOPPING)
                    && service->is_stop_pinned()) {
                ack_buf[0] = DINIT_RP_PINNEDSTOPPED;
                break;
            }
            bool found_dpt = false;
            for (auto dpt : service->get_dependents()) {
                auto from = dpt->get_from();
                auto from_state = from->get_state();
                if (from_state == service_state_t::STARTED || from_state == service_state_t::STARTING) {
                    found_dpt = true;
                    if (! dpt->holding_acq) {
                        dpt->get_from()->start_dep(*dpt);
                    }
                }
            }
            if (! found_dpt) {
                ack_buf[0] = DINIT_RP_NAK;
            }

            if (do_pin) service->pin_start();
            services->process_queues();
            if (service->get_state() == service_state_t::STARTED) ack_buf[0] = DINIT_RP_ALREADYSS;
            break;
        }
        case DINIT_CP_RELEASESERVICE:
            // remove required mark, stop if not required by dependents
            if (do_pin) service->pin_stop();
            service->stop(false);
            services->process_queues();
            if (service->get_state() == service_state_t::STOPPED) ack_buf[0] = DINIT_RP_ALREADYSS;
            break;
        }
        
        if (! queue_packet(ack_buf, 1)) return false;
    }
    
    clear_out:
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

bool control_conn_t::process_reload_service()
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

    if (! service->has_lone_ref(false)) {
        // Cannot unload: has other references
        char nak_rep[] = { DINIT_RP_NAK };
        if (! queue_packet(nak_rep, 1)) return false;
    }
    else {
        try {
            // reload
            auto *new_service = services->reload_service(service);
            if (new_service != service) {
                service->prepare_for_unload();
                services->replace_service(service, new_service);
                delete service;
            }
            else {
                service->remove_listener(this);
            }

            // drop handle
            key_service_map.erase(handle);
            service_key_map.erase(service);

            services->process_queues();

            // send ack
            char ack_buf[] = { (char) DINIT_RP_ACK };
            if (! queue_packet(ack_buf, 1)) return false;
        }
        catch (service_load_exc &slexc) {
            log(loglevel_t::ERROR, "Could not reload service ", slexc.service_name, ": ",
                    slexc.exc_description);
            char nak_rep[] = { DINIT_RP_NAK };
            if (! queue_packet(nak_rep, 1)) return false;
        }
    }

    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

constexpr static int STATUS_BUFFER_SIZE = 6 + ((sizeof(pid_t) > sizeof(int)) ? sizeof(pid_t) : sizeof(int));

static void fill_status_buffer(char *buffer, service_record *service)
{
    buffer[0] = static_cast<char>(service->get_state());
    buffer[1] = static_cast<char>(service->get_target_state());

    char b0 = service->is_waiting_for_console() ? 1 : 0;
    b0 |= service->has_console() ? 2 : 0;
    b0 |= service->was_start_skipped() ? 4 : 0;
    b0 |= service->is_marked_active() ? 8 : 0;
    buffer[2] = b0;
    buffer[3] = static_cast<char>(service->get_stop_reason());

    buffer[4] = 0; // (if exec failed, these are replaced with stage)
    buffer[5] = 0;

    // Next: either the exit status, or the process ID
    if (service->get_state() != service_state_t::STOPPED) {
        pid_t proc_pid = service->get_pid();
        memcpy(buffer + 6, &proc_pid, sizeof(proc_pid));
    }
    else {
        // If exec failed,
        if (buffer[3] == (char)stopped_reason_t::EXECFAILED) {
            base_process_service *bsp = (base_process_service *)service;
            run_proc_err exec_err = bsp->get_exec_err_info();
            uint16_t stage = (uint16_t)exec_err.stage;
            memcpy(buffer + 4, &stage, 2);
            memcpy(buffer + 6, &exec_err.st_errno, sizeof(int));
        }
        else {
            int exit_status = service->get_exit_status();
            memcpy(buffer + 6, &exit_status, sizeof(exit_status));
        }
    }
}

bool control_conn_t::list_services()
{
    rbuf.consume(1); // clear request packet
    chklen = 0;
    
    try {
        auto slist = services->list_services();
        for (auto sptr : slist) {
            std::vector<char> pkt_buf;
            
            int hdrsize = 2 + STATUS_BUFFER_SIZE;

            const std::string &name = sptr->get_name();
            int nameLen = std::min((size_t)256, name.length());
            pkt_buf.resize(hdrsize + nameLen);
            
            pkt_buf[0] = DINIT_RP_SVCINFO;
            pkt_buf[1] = nameLen;
            
            fill_status_buffer(&pkt_buf[2], sptr);

            for (int i = 0; i < nameLen; i++) {
                pkt_buf[hdrsize+i] = name[i];
            }
            
            if (!queue_packet(std::move(pkt_buf))) return false;
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

bool control_conn_t::process_service_status()
{
    constexpr int pkt_size = 1 + sizeof(handle_t);
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t handle;
    rbuf.extract(&handle, 1, sizeof(handle));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr || service->get_name().length() > std::numeric_limits<uint16_t>::max()) {
        char nak_rep[] = { DINIT_RP_NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = DINIT_RP_SERVICESTATUS
    // 1 byte reserved ( = 0)
    // STATUS_BUFFER_SIZE bytes status

    std::vector<char> pkt_buf(2 + STATUS_BUFFER_SIZE);
    pkt_buf[0] = DINIT_RP_SERVICESTATUS;
    pkt_buf[1] = 0;
    fill_status_buffer(pkt_buf.data() + 2, service);

    return queue_packet(std::move(pkt_buf));
}

bool control_conn_t::add_service_dep(bool do_enable)
{
    // 1 byte packet type
    // 1 byte dependency type
    // handle: "from"
    // handle: "to"

    constexpr int pkt_size = 2 + sizeof(handle_t) * 2;

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t from_handle;
    handle_t to_handle;
    rbuf.extract((char *) &from_handle, 2, sizeof(from_handle));
    rbuf.extract((char *) &to_handle, 2 + sizeof(from_handle), sizeof(to_handle));

    service_record *from_service = find_service_for_key(from_handle);
    service_record *to_service = find_service_for_key(to_handle);
    if (from_service == nullptr || to_service == nullptr || from_service == to_service) {
        // Service handle is bad
        char badreq_rep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }

    // Check dependency type is valid:
    int dep_type_int = rbuf[1];
    if (! contains({dependency_type::MILESTONE, dependency_type::REGULAR,
            dependency_type::WAITS_FOR}, dep_type_int)) {
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
    }
    dependency_type dep_type = static_cast<dependency_type>(dep_type_int);

    // Check current service states are valid for given dep type
    if (dep_type == dependency_type::REGULAR) {
        if (from_service->get_state() != service_state_t::STOPPED &&
                to_service->get_state() != service_state_t::STARTED) {
            // Cannot create dependency now since it would be contradicted:
            char nak_rep[] = { DINIT_RP_NAK };
            if (! queue_packet(nak_rep, 1)) return false;
            rbuf.consume(pkt_size);
            chklen = 0;
            return true;
        }
    }

    // Check for creation of circular dependency chain
    std::unordered_set<service_record *> dep_marks;
    std::vector<service_record *> dep_queue;
    dep_queue.push_back(to_service);
    while (! dep_queue.empty()) {
        service_record * sr = dep_queue.back();
        dep_queue.pop_back();
        // iterate deps; if dep == from, abort; otherwise add to set/queue
        // (only add to queue if not already in set)
        for (auto &dep : sr->get_dependencies()) {
            service_record * dep_to = dep.get_to();
            if (dep_to == from_service) {
                // fail, circular dependency!
                char nak_rep[] = { DINIT_RP_NAK };
                if (! queue_packet(nak_rep, 1)) return false;
                rbuf.consume(pkt_size);
                chklen = 0;
                return true;
            }
            if (dep_marks.insert(dep_to).second) {
                dep_queue.push_back(dep_to);
            }
        }
    }
    dep_marks.clear();
    dep_queue.clear();

    bool dep_exists = false;
    service_dep * dep_record = nullptr;

    // Prevent creation of duplicate dependency:
    for (auto &dep : from_service->get_dependencies()) {
        service_record * dep_to = dep.get_to();
        if (dep_to == to_service && dep.dep_type == dep_type) {
            // Dependency already exists
            dep_exists = true;
            dep_record = &dep;
            break;
        }
    }

    if (! dep_exists) {
        // Create dependency:
        dep_record = &(from_service->add_dep(to_service, dep_type));
        services->process_queues();
    }

    if (do_enable && contains({service_state_t::STARTED, service_state_t::STARTING},
            from_service->get_state())) {
        // The dependency record is activated: mark it as holding acquisition of the dependency, and start
        // the dependency.
        if (!services->is_shutting_down()) {
            dep_record->get_from()->start_dep(*dep_record);
            services->process_queues();
        }
    }

    char ack_rep[] = { DINIT_RP_ACK };
    if (! queue_packet(ack_rep, 1)) return false;
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

bool control_conn_t::rm_service_dep()
{
    // 1 byte packet type
    // 1 byte dependency type
    // handle: "from"
    // handle: "to"

    constexpr int pkt_size = 2 + sizeof(handle_t) * 2;

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t from_handle;
    handle_t to_handle;
    rbuf.extract((char *) &from_handle, 2, sizeof(from_handle));
    rbuf.extract((char *) &to_handle, 2 + sizeof(from_handle), sizeof(to_handle));

    service_record *from_service = find_service_for_key(from_handle);
    service_record *to_service = find_service_for_key(to_handle);
    if (from_service == nullptr || to_service == nullptr || from_service == to_service) {
        // Service handle is bad
        char badreq_rep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
        return true;
    }

    // Check dependency type is valid:
    int dep_type_int = rbuf[1];
    if (! contains({dependency_type::MILESTONE, dependency_type::REGULAR,
            dependency_type::WAITS_FOR}, dep_type_int)) {
        char badreqRep[] = { DINIT_RP_BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        iob.set_watches(OUT_EVENTS);
    }
    dependency_type dep_type = static_cast<dependency_type>(dep_type_int);

    // Remove dependency:
    from_service->rm_dep(to_service, dep_type);
    services->process_queues();

    char ack_rep[] = { DINIT_RP_ACK };
    if (! queue_packet(ack_rep, 1)) return false;
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

bool control_conn_t::process_query_name()
{
    // 1 byte packet type
    // 1 byte reserved
    // handle: service
    constexpr int pkt_size = 2 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t handle;
    rbuf.extract(&handle, 2, sizeof(handle));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr || service->get_name().length() > std::numeric_limits<uint16_t>::max()) {
        char nak_rep[] = { DINIT_RP_NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = DINIT_RP_SERVICENAME
    // 1 byte reserved
    // uint16_t length
    // N bytes name

    std::vector<char> reply;
    const std::string &name = service->get_name();
    uint16_t name_length = name.length();
    reply.resize(2 + sizeof(uint16_t) + name_length);
    reply[0] = DINIT_RP_SERVICENAME;
    memcpy(reply.data() + 2, &name_length, sizeof(name_length));
    memcpy(reply.data() + 2 + sizeof(uint16_t), name.c_str(), name_length);

    return queue_packet(std::move(reply));
}

bool control_conn_t::process_setenv()
{
    using std::string;

    string envVar;
    typename string::size_type eq;

    constexpr int pkt_size = 4;
    char badreqRep[] = { DINIT_RP_BADREQ };
    char okRep[] = { DINIT_RP_ACK };

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    uint16_t envSize;
    rbuf.extract((char *)&envSize, 1, 2);
    if (envSize <= 0 || envSize > (1024 - 3)) {
        goto badreq;
    }
    chklen = envSize + 3; // packet type + (2 byte) length + envvar

    if (rbuf.get_length() < chklen) {
        // packet not complete yet; read more
        return true;
    }

    envVar = rbuf.extract_string(3, envSize);

    eq = envVar.find('=');
    if (!eq || eq == envVar.npos) {
        // not found or at the beginning of the string
        goto badreq;
    }

    envVar[eq] = '\0';

    if (setenv(envVar.c_str(), &envVar[eq + 1], 1) != 0) {
        // failed to set the var
        goto badreq;
    }

    // success response
    if (! queue_packet(okRep, 1)) return false;

    // Clear the packet from the buffer
    rbuf.consume(chklen);
    chklen = 0;
    return true;

badreq:
    // Queue error response / mark connection bad
    if (! queue_packet(badreqRep, 1)) return false;
    bad_conn_close = true;
    iob.set_watches(OUT_EVENTS);
    return true;
}

bool control_conn_t::query_load_mech()
{
    rbuf.consume(1);
    chklen = 0;

    if (services->get_set_type_id() == SSET_TYPE_DIRLOAD) {
        dirload_service_set *dss = static_cast<dirload_service_set *>(services);
        std::vector<char> reppkt;
        reppkt.resize(2 + sizeof(uint32_t) * 2);  // packet type, loader type, packet size, # dirs
        reppkt[0] = DINIT_RP_LOADER_MECH;
        reppkt[1] = SSET_TYPE_DIRLOAD;

        // Number of directories in load path:
        uint32_t sdirs = dss->get_service_dir_count();
        std::memcpy(reppkt.data() + 2 + sizeof(uint32_t), &sdirs, sizeof(sdirs));

        // Our current working directory, which above are relative to:
        // leave sizeof(uint32_t) for size, which we'll fill in afterwards:
        std::size_t curpos = reppkt.size() + sizeof(uint32_t);
#ifdef PATH_MAX
        uint32_t try_path_size = PATH_MAX;
#else
        uint32_t try_path_size = 2048;
#endif
        char *wd;
        while (true) {
            std::size_t total_size = curpos + std::size_t(try_path_size);
            if (total_size < curpos) {
                // Overflow. In theory we could now limit to size_t max, but the size must already
                // be crazy long; let's abort.
                char ack_rep[] = { DINIT_RP_NAK };
                if (! queue_packet(ack_rep, 1)) return false;
                return true;
            }
            reppkt.resize(total_size);
            wd = getcwd(reppkt.data() + curpos, try_path_size);
            if (wd != nullptr) break;

            // Keep doubling the path size we try until it's big enough, or we get numeric overflow
            uint32_t new_try_path_size = try_path_size * uint32_t(2u);
            if (new_try_path_size < try_path_size) {
                // Overflow.
                char ack_rep[] = { DINIT_RP_NAK };
                return queue_packet(ack_rep, 1);
            }
            try_path_size = new_try_path_size;
        }

        uint32_t wd_len = std::strlen(reppkt.data() + curpos);
        reppkt.resize(curpos + std::size_t(wd_len));
        std::memcpy(reppkt.data() + curpos - sizeof(uint32_t), &wd_len, sizeof(wd_len));

        // Each directory in the load path:
        for (int i = 0; uint32_t(i) < sdirs; i++) {
            const char *sdir = dss->get_service_dir(i);
            uint32_t dlen = std::strlen(sdir);
            auto cursize = reppkt.size();
            reppkt.resize(cursize + sizeof(dlen) + dlen);
            std::memcpy(reppkt.data() + cursize, &dlen, sizeof(dlen));
            std::memcpy(reppkt.data() + cursize + sizeof(dlen), sdir, dlen);
        }

        // Total packet size:
        uint32_t fsize = reppkt.size();
        std::memcpy(reppkt.data() + 2, &fsize, sizeof(fsize));

        if (! queue_packet(std::move(reppkt))) return false;
        return true;
    }
    else {
        // If we don't know how to deal with the service set type, send a NAK reply:
        char ack_rep[] = { DINIT_RP_NAK };
        return queue_packet(ack_rep, 1);
    }
}

control_conn_t::handle_t control_conn_t::allocate_service_handle(service_record *record)
{
    // Try to find a unique handle (integer) in a single pass. Since the map is ordered, we can search until
    // we find a gap in the handle values.
    handle_t candidate = 0;
    for (auto p : key_service_map) {
        if (p.first == candidate) ++candidate;
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
        int wr = bp_sys::write(iob.get_watched_fd(), pkt, size);
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
        int wr = bp_sys::write(iob.get_watched_fd(), pkt.data(), pkt.size());
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
    else if (rbuf.get_length() == rbuf.get_size()) {
        // Too big packet
        log(loglevel_t::WARN, "Received too-large control packet; dropping connection");
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
            bp_sys::write(iob.get_watched_fd(), oomBuf, 1);
        }
        return true;
    }
    
    vector<char> & pkt = outbuf.front();
    char *data = pkt.data();
    int written = bp_sys::write(iob.get_watched_fd(), data + outpkt_index, pkt.size() - outpkt_index);
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
    bp_sys::close(iob.get_watched_fd());
    iob.deregister(loop);
    
    // Clear service listeners
    for (auto p : service_key_map) {
        p.first->remove_listener(this);
    }
    
    active_control_conns--;
}
