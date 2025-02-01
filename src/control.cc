#include <algorithm>
#include <unordered_set>
#include <climits>

#include "control-cmds.h"
#include "dinit-env.h"
#include "control.h"
#include "service.h"
#include "proc-service.h"
#include "control-datatypes.h"

// Server-side control protocol implementation. This implements the functionality that allows
// clients (such as dinitctl) to query service state and issue commands to control services.

// common communication datatypes
using namespace dinit_cptypes;

namespace {
    // Control protocol minimum compatible version and current version:
    constexpr uint16_t min_compat_version = 1;
    constexpr uint16_t cp_version = 5;

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

    cp_cmd pkt_type = (cp_cmd)rbuf[0];

    switch (pkt_type) {
        case cp_cmd::QUERYVERSION:
        {
            // Responds with:
            // cp_rply::CPVERSION, (2 byte) minimum compatible version, (2 byte) actual version
            char replyBuf[] = { (char)cp_rply::CPVERSION, 0, 0, 0, 0 };
            memcpy(replyBuf + 1, &min_compat_version, 2);
            memcpy(replyBuf + 3, &cp_version, 2);
            if (!queue_packet(replyBuf, sizeof(replyBuf))) return false;
            rbuf.consume(1);
            return true;
        }
        case cp_cmd::FINDSERVICE:
        case cp_cmd::LOADSERVICE:
            return process_find_load(pkt_type);
        case cp_cmd::CLOSEHANDLE:
            return process_close_handle();
        case cp_cmd::STARTSERVICE:
        case cp_cmd::STOPSERVICE:
        case cp_cmd::WAKESERVICE:
        case cp_cmd::RELEASESERVICE:
            return process_start_stop(pkt_type);
        case cp_cmd::UNPINSERVICE:
            return process_unpin_service();
        case cp_cmd::UNLOADSERVICE:
            return process_unload_service();
        case cp_cmd::RELOADSERVICE:
            return process_reload_service();
        case cp_cmd::SHUTDOWN:
            // Shutdown/reboot
            if (rbuf.get_length() < 2) {
                chklen = 2;
                return true;
            }

            if (contains({shutdown_type_t::REMAIN, shutdown_type_t::HALT,
                    shutdown_type_t::POWEROFF, shutdown_type_t::REBOOT,
                    shutdown_type_t::SOFTREBOOT, shutdown_type_t::KEXEC}, rbuf[1])) {
                auto sd_type = static_cast<shutdown_type_t>(rbuf[1]);

                services->stop_all_services(sd_type);
                char ackBuf[] = { (char)cp_rply::ACK };
                if (! queue_packet(ackBuf, 1)) return false;

                // Clear the packet from the buffer
                rbuf.consume(2);
                chklen = 0;
                return true;
            }

            break;
        case cp_cmd::LISTSERVICES:
            return list_services();
        case cp_cmd::LISTSERVICES5:
            return list_services5();
        case cp_cmd::SERVICESTATUS:
            return process_service_status();
        case cp_cmd::SERVICESTATUS5:
            return process_service_status5();
        case cp_cmd::ADD_DEP:
            return add_service_dep();
        case cp_cmd::REM_DEP:
            return rm_service_dep();
        case cp_cmd::QUERY_LOAD_MECH:
            return query_load_mech();
        case cp_cmd::ENABLESERVICE:
            return add_service_dep(true);
        case cp_cmd::QUERYSERVICENAME:
            return process_query_name();
        case cp_cmd::SETENV:
            return process_setenv();
        case cp_cmd::GETALLENV:
            return process_getallenv();
        case cp_cmd::LISTENENV:
            return process_listenenv();
        case cp_cmd::SETTRIGGER:
            return process_set_trigger();
        case cp_cmd::CATLOG:
            return process_catlog();
        case cp_cmd::SIGNAL:
            return process_signal();
        case cp_cmd::QUERYSERVICEDSCDIR:
            return process_query_dsc_dir();
        default:
            break;
    }

    // Unrecognized: give error response
    char outbuf[] = { (char)cp_rply::BADREQ };
    if (!queue_packet(outbuf, 1)) return false;
    bad_conn_close = true;
    return true;
}

bool control_conn_t::process_find_load(cp_cmd pktType)
{
    using std::string;
    
    constexpr int pkt_size = 4;
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }
    
    srvname_len_t srvname_len;
    rbuf.extract(&srvname_len, 1, sizeof(srvname_len));
    if (srvname_len <= 0 || srvname_len > (1024 - 3)) {
        // Queue error response / mark connection bad
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        return true;
    }
    chklen = srvname_len + 3; // packet type + (2 byte) length + service name
    
    if (rbuf.get_length() < chklen) {
        // packet not complete yet; read more
        return true;
    }
    
    service_record * record = nullptr;
    
    string service_name = rbuf.extract_string(3, srvname_len);

    // Clear the packet from the buffer
    rbuf.consume(chklen);
    chklen = 0;
    
    cp_rply fail_code = cp_rply::NOSERVICE;

    if (pktType == cp_cmd::LOADSERVICE) {
        // LOADSERVICE
        try {
            record = services->load_service(service_name.c_str());
        }
        catch (service_description_exc &sdexc) {
            log_service_load_failure(sdexc);
            fail_code = cp_rply::SERVICE_DESC_ERR;
        }
        catch (service_not_found &snf) {
            log(loglevel_t::ERROR, "Could not load service ", snf.service_name, ": ",
                    snf.exc_description);
            // fail_code = cp_rply::NOSERVICE;   (already set)
        }
        catch (service_load_exc &slexc) {
            log(loglevel_t::ERROR, "Could not load service ", slexc.service_name, ": ",
                    slexc.exc_description);
            fail_code = cp_rply::SERVICE_LOAD_ERR;
        }
    }
    else {
        // FINDSERVICE
        record = services->find_service(service_name.c_str());
    }
    
    if (record == nullptr) {
        std::vector<char> rp_buf = { (char)fail_code };
        if (! queue_packet(std::move(rp_buf))) return false;
        return true;
    }

    // Allocate a service handle
    handle_t handle = allocate_service_handle(record);
    std::vector<char> rp_buf;
    rp_buf.reserve(7);
    rp_buf.push_back((char)cp_rply::SERVICERECORD);
    rp_buf.push_back(static_cast<char>(record->get_state()));
    for (int i = 0; i < (int) sizeof(handle); i++) {
        rp_buf.push_back(*(((char *) &handle) + i));
    }
    rp_buf.push_back(static_cast<char>(record->get_target_state()));
    if (! queue_packet(std::move(rp_buf))) return false;
    
    return true;
}

bool control_conn_t::process_close_handle()
{
    constexpr int pkt_size = 1 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t handle;
    rbuf.extract((char *) &handle, 1, sizeof(handle));

    rbuf.consume(pkt_size);
    chklen = 0;

    auto key_it = key_service_map.find(handle);
    if (key_it == key_service_map.end()) {
        // Service handle is bad
        char badreq_rep[] = { (char)cp_rply::BADREQ };
        if (!queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    service_record *service = key_it->second;
    key_service_map.erase(key_it);

    bool have_other_handle = false;
    auto handle_range = service_key_map.equal_range(service);
    auto it = handle_range.first;
    while (it->second != handle) {
        have_other_handle = true;
        ++it;
    }
    if (!have_other_handle) {
        // check if more handles beyond the found handle
        have_other_handle = std::next(it) != handle_range.second;
    }
    service_key_map.erase(it);

    if (!have_other_handle) {
        service->remove_listener(this);
    }

    char ack_reply[] = { (char)cp_rply::ACK };
    return queue_packet(ack_reply, sizeof(ack_reply));
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
                reply_pkt[0] = (char)cp_rply::DEPENDENTS;
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

bool control_conn_t::process_start_stop(cp_cmd pktType)
{
    using std::string;
    
    constexpr int pkt_size = 2 + sizeof(handle_t);
    
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }
    
    // 1 byte: packet type
    // 1 byte: flags
    //    bit 0: 0 = no pin, 1 = pin
    //    bit 1: 0 = force stop, 1 = not forced ("gentle")
    //    bit 2: 0 = don't restart, 1 = restart after stopping
    //      --- (reserved)
    //    bit 7: 0 = no pre-ack, 1 = issue pre-ack
    // 4 bytes: service handle
    
    bool do_pin = ((rbuf[1] & 1) == 1);
    handle_t handle;
    rbuf.extract((char *) &handle, 2, sizeof(handle));
    
    service_record *service = find_service_for_key(handle);
    if (service == nullptr) {
        // Service handle is bad
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (!queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        return true;
    }
    else {
        char ack_buf[1] = { (char)cp_rply::PREACK };
        if (rbuf[1] & 128) {
            // Issue PREACK before doing anything that might change service state (and cause a
            // service event info packet to be issued as a result). This allows the client to
            // determine whether the info packets were queued before or after the command was
            // processed.
            if (!queue_packet(ack_buf, 1)) return false;
        }

        ack_buf[0] = (char)cp_rply::ACK;
        
        switch (pktType) {
        case cp_cmd::STARTSERVICE:
            // start service, mark as required
            if (services->is_shutting_down()) {
                ack_buf[0] = (char)cp_rply::SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STOPPED
                    || service->get_state() == service_state_t::STOPPING)
                    && service->is_stop_pinned()) {
                ack_buf[0] = (char)cp_rply::PINNEDSTOPPED;
                break;
            }
            if (do_pin) service->pin_start();
            service->start();
            services->process_queues();
            if (service->get_state() == service_state_t::STARTED) ack_buf[0] = (char)cp_rply::ALREADYSS;
            break;
        case cp_cmd::STOPSERVICE:
        {
            // force service to stop
            bool do_restart = ((rbuf[1] & 4) == 4);
            bool gentle = ((rbuf[1] & 2) == 2);
            if (do_restart && services->is_shutting_down()) {
                ack_buf[0] = (char)cp_rply::SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STARTED
                    || service->get_state() == service_state_t::STARTING)
                    && service->is_start_pinned()) {
                ack_buf[0] = (char)cp_rply::PINNEDSTARTED;
                break;
            }
            if (gentle) {
                // Check dependents; return appropriate response if any will be affected
                bool has_dependents;
                if (!check_dependents(service, has_dependents)) {
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
                    ack_buf[0] = (char)cp_rply::NAK;
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
            if (service->get_state() == wanted_state) ack_buf[0] = (char)cp_rply::ALREADYSS;
            break;
        }
        case cp_cmd::WAKESERVICE:
        {
            // re-attach a service to its (started) dependents, causing it to start.
            if (services->is_shutting_down()) {
                ack_buf[0] = (char)cp_rply::SHUTTINGDOWN;
                break;
            }
            if ((service->get_state() == service_state_t::STOPPED
                    || service->get_state() == service_state_t::STOPPING)
                    && service->is_stop_pinned()) {
                ack_buf[0] = (char)cp_rply::PINNEDSTOPPED;
                break;
            }
            bool found_dpt = false;
            for (auto dpt : service->get_dependents()) {
                if (dpt->is_only_ordering()) continue;
                auto from = dpt->get_from();
                auto from_state = from->get_state();
                if (from_state == service_state_t::STARTED || from_state == service_state_t::STARTING) {
                    found_dpt = true;
                    if (!dpt->holding_acq) {
                        dpt->get_from()->start_dep(*dpt);
                    }
                }
            }
            if (!found_dpt) {
                ack_buf[0] = (char)cp_rply::NAK;
            }

            if (do_pin) service->pin_start();
            services->process_queues();
            if (service->get_state() == service_state_t::STARTED) ack_buf[0] = (char)cp_rply::ALREADYSS;
            break;
        }
        case cp_cmd::RELEASESERVICE:
            // remove required mark, stop if not required by dependents
            if (do_pin) service->pin_stop();
            service->stop(false);
            services->process_queues();
            if (service->get_state() == service_state_t::STOPPED) ack_buf[0] = (char)cp_rply::ALREADYSS;
            break;
        default:
            // avoid warning for unhandled switch/case values
            return false;
        }
        
        if (!queue_packet(ack_buf, 1)) return false;
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
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    service->unpin();
    services->process_queues();
    char ack_buf[] = { (char) cp_rply::ACK };
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
        char badreq_rep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    if (!service->has_lone_ref() || service->get_state() != service_state_t::STOPPED) {
        // Cannot unload: has other references
        char nak_rep[] = { (char)cp_rply::NAK };
        if (!queue_packet(nak_rep, 1)) return false;
    }
    else {
        // unload (this may fail with bad_alloc)
        services->unload_service(service);

        // drop handle
        service_key_map.erase(service);
        key_service_map.erase(handle);

        // send ack
        char ack_buf[] = { (char) cp_rply::ACK };
        if (!queue_packet(ack_buf, 1)) return false;
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
        char badreq_rep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    if (!service->has_lone_ref(false)) {
        // Cannot unload: has other references
        char nak_rep[] = { (char)cp_rply::NAK };
        if (! queue_packet(nak_rep, 1)) return false;
    }
    else {
        try {
            // drop handle
            key_service_map.erase(handle);
            service_key_map.erase(service);

            // reload
            service->remove_listener(this);
            services->reload_service(service);
            services->process_queues();

            // send ack
            char ack_buf[] = { (char) cp_rply::ACK };
            if (! queue_packet(ack_buf, 1)) return false;
        }
        catch (service_load_exc &slexc) {
            log(loglevel_t::ERROR, "Could not reload service ", slexc.service_name, ": ",
                    slexc.exc_description);
            char nak_rep[] = { (char)cp_rply::NAK };
            if (! queue_packet(nak_rep, 1)) return false;
        }
    }

    // Clear the packet from the buffer
    rbuf.consume(pkt_size);
    chklen = 0;
    return true;
}

constexpr static unsigned SIZEOF_INT_PIDT_UNION = ((sizeof(pid_t) > sizeof(int)) ? sizeof(pid_t) : sizeof(int));
constexpr static unsigned STATUS_BUFFER_SIZE = 6 + SIZEOF_INT_PIDT_UNION;

static void fill_status_buffer(char *buffer, service_record *service)
{
    buffer[0] = static_cast<char>(service->get_state());
    buffer[1] = static_cast<char>(service->get_target_state());

    pid_t proc_pid = service->get_pid();

    char b0 = service->is_waiting_for_console() ? 1 : 0;
    b0 |= service->has_console() ? 2 : 0;
    b0 |= service->was_start_skipped() ? 4 : 0;
    b0 |= service->is_marked_active() ? 8 : 0;
    b0 |= (proc_pid != -1) ? 16 : 0;
    buffer[2] = b0;
    buffer[3] = static_cast<char>(service->get_stop_reason());

    buffer[4] = 0; // (if exec failed, these are replaced with stage)
    buffer[5] = 0;

    if (proc_pid != -1) {
        memcpy(buffer + 6, &proc_pid, sizeof(proc_pid));
    }
    else {
        // These values only make sense in STOPPING/STOPPED, but we'll fill them in regardless:
        if (buffer[3] == (char)stopped_reason_t::EXECFAILED) {
            base_process_service *bsp = (base_process_service *)service;
            run_proc_err exec_err = bsp->get_exec_err_info();
            uint16_t stage = (uint16_t)exec_err.stage;
            memcpy(buffer + 4, &stage, 2);
            memcpy(buffer + 6, &exec_err.st_errno, sizeof(int));
        }
        else {
            auto exit_status_ps = service->get_exit_status();
            // There is no portable way to derive the correct exit status value corresponding
            // to any condition except for a clean exit (0) which is the most important anyway.
            // We'll use -1 for any other status although this may not be valid.
            int exit_status = exit_status_ps.did_exit_clean() ? 0 : -1;
            memcpy(buffer + 6, &exit_status, sizeof(exit_status));
        }
    }
}

constexpr static unsigned STATUS_BUFFER5_SIZE = 6 + 2 * sizeof(int);

static void fill_status_buffer5(char *buffer, service_record *service)
{
    buffer[0] = static_cast<char>(service->get_state());
    buffer[1] = static_cast<char>(service->get_target_state());

    pid_t proc_pid = service->get_pid();

    char b0 = service->is_waiting_for_console() ? 1 : 0;
    b0 |= service->has_console() ? 2 : 0;
    b0 |= service->was_start_skipped() ? 4 : 0;
    b0 |= service->is_marked_active() ? 8 : 0;
    b0 |= (proc_pid != -1) ? 16 : 0;
    buffer[2] = b0;
    buffer[3] = static_cast<char>(service->get_stop_reason());

    buffer[4] = 0; // (if exec failed, these are replaced with stage)
    buffer[5] = 0;

    if (proc_pid != -1) {
        memcpy(buffer + 6, &proc_pid, sizeof(proc_pid));
        unsigned remains = STATUS_BUFFER5_SIZE - (6 + sizeof(proc_pid));
        memset(buffer + 6 + sizeof(proc_pid), 0, remains);
    }
    else {
        // These values only make sense in STOPPING/STOPPED, but we'll fill them in regardless:
        if (buffer[3] == (char)stopped_reason_t::EXECFAILED) {
            base_process_service *bsp = (base_process_service *)service;
            run_proc_err exec_err = bsp->get_exec_err_info();
            uint16_t stage = (uint16_t)exec_err.stage;
            memcpy(buffer + 4, &stage, 2);
            memcpy(buffer + 6, &exec_err.st_errno, sizeof(int));
            unsigned remains = STATUS_BUFFER5_SIZE - (6 + sizeof(int));
            memset(buffer + 6 + sizeof(proc_pid), 0, remains);
        }
        else {
            auto exit_status = service->get_exit_status();
            int xs_si_code = exit_status.get_si_code();
            int xs_si_status = exit_status.get_si_status();
            memcpy(buffer + 6, &xs_si_code, sizeof(xs_si_code));
            memcpy(buffer + 6 + sizeof(xs_si_code), &xs_si_status, sizeof(xs_si_status));
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
            if (sptr->get_type() == service_type_t::PLACEHOLDER) continue;

            std::vector<char> pkt_buf;
            int hdrsize = 2 + STATUS_BUFFER_SIZE;

            const std::string &name = sptr->get_name();
            int nameLen = std::min((size_t)256, name.length());
            pkt_buf.resize(hdrsize + nameLen);
            
            pkt_buf[0] = (char)cp_rply::SVCINFO;
            pkt_buf[1] = nameLen;
            
            fill_status_buffer(&pkt_buf[2], sptr);

            for (int i = 0; i < nameLen; i++) {
                pkt_buf[hdrsize+i] = name[i];
            }
            
            if (!queue_packet(std::move(pkt_buf))) return false;
        }
        
        char ack_buf[] = { (char) cp_rply::LISTDONE };
        if (!queue_packet(ack_buf, 1)) return false;

        return true;
    }
    catch (std::bad_alloc &exc)
    {
        do_oom_close();
        return true;
    }
}

bool control_conn_t::list_services5()
{
    rbuf.consume(1); // clear request packet
    chklen = 0;

    try {
        auto slist = services->list_services();
        for (auto sptr : slist) {
            if (sptr->get_type() == service_type_t::PLACEHOLDER) continue;

            std::vector<char> pkt_buf;
            int hdrsize = 2 + STATUS_BUFFER5_SIZE;

            const std::string &name = sptr->get_name();
            int nameLen = std::min((size_t)256, name.length());
            pkt_buf.resize(hdrsize + nameLen);

            pkt_buf[0] = (char)cp_rply::SVCINFO;
            pkt_buf[1] = nameLen;

            fill_status_buffer5(&pkt_buf[2], sptr);

            for (int i = 0; i < nameLen; i++) {
                pkt_buf[hdrsize+i] = name[i];
            }

            if (!queue_packet(std::move(pkt_buf))) return false;
        }

        char ack_buf[] = { (char) cp_rply::LISTDONE };
        if (!queue_packet(ack_buf, 1)) return false;
        
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
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = cp_rply::SERVICESTATUS
    // 1 byte reserved ( = 0)
    // STATUS_BUFFER_SIZE bytes status

    std::vector<char> pkt_buf(2 + STATUS_BUFFER_SIZE);
    pkt_buf[0] = (char)cp_rply::SERVICESTATUS;
    pkt_buf[1] = 0;
    fill_status_buffer(pkt_buf.data() + 2, service);

    return queue_packet(std::move(pkt_buf));
}

bool control_conn_t::process_service_status5()
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
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = cp_rply::SERVICESTATUS
    // 1 byte reserved ( = 0)
    // STATUS_BUFFER5_SIZE bytes status

    std::vector<char> pkt_buf(2 + STATUS_BUFFER5_SIZE);
    pkt_buf[0] = (char)cp_rply::SERVICESTATUS;
    pkt_buf[1] = 0;
    fill_status_buffer5(pkt_buf.data() + 2, service);

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
        char badreq_rep[] = { (char)cp_rply::BADREQ };
        if (!queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    // Check dependency type is valid:
    int dep_type_int = rbuf[1];
    if (!contains({dependency_type::MILESTONE, dependency_type::REGULAR,
            dependency_type::WAITS_FOR}, dep_type_int)) {
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (!queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
    }
    dependency_type dep_type = static_cast<dependency_type>(dep_type_int);

    // Check current service states are valid for given dep type
    if (dep_type == dependency_type::REGULAR) {
        if (from_service->get_state() != service_state_t::STOPPED &&
                to_service->get_state() != service_state_t::STARTED) {
            // Cannot create dependency now since it would be contradicted:
            char nak_rep[] = { (char)cp_rply::NAK };
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
                char nak_rep[] = { (char)cp_rply::NAK };
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

    char ack_rep[] = { (char)cp_rply::ACK };
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
        char badreq_rep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreq_rep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    // Check dependency type is valid:
    int dep_type_int = rbuf[1];
    if (! contains({dependency_type::MILESTONE, dependency_type::REGULAR,
            dependency_type::WAITS_FOR}, dep_type_int)) {
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (! queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
    }
    dependency_type dep_type = static_cast<dependency_type>(dep_type_int);

    // Remove dependency:
    bool did_remove = from_service->rm_dep(to_service, dep_type);
    services->process_queues();

    char ack_rep[] = { did_remove ? (char)cp_rply::ACK : (char)cp_rply::NAK };
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
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = cp_rply::SERVICENAME
    // 1 byte reserved
    // uint16_t length
    // N bytes name

    std::vector<char> reply;
    const std::string &name = service->get_name();
    uint16_t name_length = name.length();
    reply.resize(2 + sizeof(uint16_t) + name_length);
    reply[0] = (char)cp_rply::SERVICENAME;
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
    char badreqRep[] = { (char)cp_rply::BADREQ };
    char okRep[] = { (char)cp_rply::ACK };

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    envvar_len_t envvar_len;
    rbuf.extract(&envvar_len, 1, sizeof(envvar_len));
    if (envvar_len <= 0 || envvar_len > (1024 - 3)) {
        goto badreq;
    }
    chklen = envvar_len + 1 + sizeof(envvar_len); // packet type + (2 byte) length + envvar

    if (rbuf.get_length() < chklen) {
        // packet not complete yet; read more
        return true;
    }

    envVar = rbuf.extract_string(3, envvar_len);

    eq = envVar.find('=');
    if (eq == envVar.npos) {
        // Unset the env var
        main_env.undefine_var(std::move(envVar), true);
    }
    else if (eq) {
        // Regular set
        main_env.set_var(std::move(envVar), true);
    }
    else {
        // At the beginning of the string
        goto badreq;
    }

    // Success response
    if (!queue_packet(okRep, 1)) return false;

    // Clear the packet from the buffer
    rbuf.consume(chklen);
    chklen = 0;
    return true;

badreq:
    // Queue error response / mark connection bad
    if (!queue_packet(badreqRep, 1)) return false;
    bad_conn_close = true;
    return true;
}

bool control_conn_t::process_getallenv()
{
    // 1 byte packet type
    // 1 byte reserved - must be 0

    constexpr int pkt_size = 2;
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    uint8_t reserved_byte = rbuf[1];
    if (reserved_byte != 0) {
        char badreqRep[] = { (char)cp_rply::BADREQ };
        if (!queue_packet(badreqRep, 1)) return false;
        bad_conn_close = true;
        return true;
    }

    // The reply looks like:
    // 1 byte - reply type
    // sizeof(size_t) - reply data size
    // n bytes - reply data (NAME=VALUE, separated by nul characters)

    std::vector<char> env_block;
    constexpr size_t env_block_hdr_size = sizeof(size_t) + 1;
    env_block.resize(env_block_hdr_size);

    rbuf.consume(pkt_size);
    auto env = main_env.build();
    for (const char *env_var : env.env_list) {
        if (env_var != nullptr) {
            env_block.insert(env_block.end(), env_var, env_var + strlen(env_var) + 1);
        }
    }

    env_block[0] = (char)cp_rply::ALLENV;
    size_t block_size = env_block.size() - env_block_hdr_size;
    memcpy(env_block.data() + 1, &block_size, sizeof(block_size));
    if (!queue_packet(std::move(env_block))) return false;
    return true;
}

bool control_conn_t::process_listenenv()
{
    // 1 byte packet type, nothing else
    rbuf.consume(1);

    main_env.add_listener(this);

    char ack_rep[] = { (char)cp_rply::ACK };
    return queue_packet(ack_rep, 1);
}

bool control_conn_t::process_set_trigger()
{
    // 1 byte packet type
    // handle: service
    // 1 byte trigger value
    constexpr int pkt_size = 2 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t handle;
    trigger_val_t trigger_val;

    rbuf.extract(&handle, 1, sizeof(handle));
    rbuf.extract(&trigger_val, 1 + sizeof(handle), sizeof(trigger_val));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr || service->get_type() != service_type_t::TRIGGERED) {
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    triggered_service *tservice = static_cast<triggered_service *>(service);
    tservice->set_trigger(trigger_val != 0);
    services->process_queues();

    char ack_rep[] = { (char)cp_rply::ACK };
    return queue_packet(ack_rep, 1);
}

bool control_conn_t::process_catlog()
{
    // 1 byte packet type
    // 1 byte reserved for future use
    // handle
    constexpr int pkt_size = 2 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    handle_t handle;
    char flags = rbuf[1];

    rbuf.extract(&handle, 2, sizeof(handle));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr || (service->get_type() != service_type_t::PROCESS
            && service->get_type() != service_type_t::BGPROCESS
            && service->get_type() != service_type_t::SCRIPTED)) {
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    base_process_service *bps = static_cast<base_process_service *>(service);
    if (bps->get_log_mode() != log_type_id::BUFFER) {
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    auto buffer_details = bps->get_log_buffer();
    const char *bufaddr = buffer_details.first;
    unsigned buflen = buffer_details.second;

    std::vector<char> pkt = { (char)cp_rply::SERVICE_LOG, 0 /* flags; reserved for future */ };
    pkt.insert(pkt.end(), (char *)(&buflen), (char *)(&buflen + 1));
    pkt.insert(pkt.end(), bufaddr, bufaddr + buflen);
    if ((flags & 1) != 0) {
        bps->clear_log_buffer();
    }
    return queue_packet(std::move(pkt));
}

bool control_conn_t::process_signal()
{
    // packet contains signal number and process handle
    constexpr int pkt_size = 1 + sizeof(int) + sizeof(handle_t);
    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    sig_num_t sig_num;
    rbuf.extract(&sig_num, 1, sizeof(sig_num));
    handle_t handle;
    rbuf.extract(&handle, 1 + sizeof(sig_num), sizeof(handle));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr) {
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = cp_rply::*

    pid_t spid = service->get_pid();
    // we probably don't want to kill/signal every process (in the current group),
    // but get_pid() sometimes returns -1 if e.g. service is not 'started'
    if (spid == -1 || spid == 0) {
        char nak_rep[] = { (char)cp_rply::SIGNAL_NOPID };
        return queue_packet(nak_rep, 1);
    }
    else {
        if (bp_sys::kill(spid, sig_num) != 0) {
            if (errno == EINVAL) {
                log(loglevel_t::ERROR, "Requested signal not in valid signal range.");
                char nak_rep[] = { (char)cp_rply::SIGNAL_BADSIG };
                return queue_packet(nak_rep, 1);
            }
            log(loglevel_t::ERROR, "Error sending signal to process: ", strerror(errno));
            char nak_rep[] = { (char)cp_rply::SIGNAL_KILLERR };
            return queue_packet(nak_rep, 1);
        }
    }
    char ack_rep[] = { (char)cp_rply::ACK };
    return queue_packet(ack_rep, 1);
}

bool control_conn_t::process_query_dsc_dir()
{
    // packet contains command byte, spare byte, and service handle
    constexpr int pkt_size = 2 + sizeof(handle_t);

    if (rbuf.get_length() < pkt_size) {
        chklen = pkt_size;
        return true;
    }

    bool spare_ok = (rbuf[1] == 0);
    handle_t handle;
    rbuf.extract(&handle, 2, sizeof(handle));
    rbuf.consume(pkt_size);
    chklen = 0;

    service_record *service = find_service_for_key(handle);
    if (service == nullptr || !spare_ok) {
        char nak_rep[] = { (char)cp_rply::NAK };
        return queue_packet(nak_rep, 1);
    }

    // Reply:
    // 1 byte packet type = cp_rply::SVCDSCDIR
    // 4 bytes (uint32_t) = directory length (no nul terminator)
    // N bytes            = directory (no nul)
    std::vector<char> reppkt;
    auto sdir_len = static_cast<uint32_t>(strlen(service->get_service_dsc_dir()));
    reppkt.resize(1 + sizeof(uint32_t) + sdir_len);  // packet type, dir length, dir
    reppkt[0] = (char)cp_rply::SVCDSCDIR;
    std::memcpy(&reppkt[1], &sdir_len, sizeof(sdir_len));
    std::memcpy(&reppkt[1 + sizeof(uint32_t)], service->get_service_dsc_dir(), sdir_len);

    if (! queue_packet(std::move(reppkt))) return false;
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
        reppkt[0] = (char)cp_rply::LOADER_MECH;
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
                char ack_rep[] = { (char)cp_rply::NAK };
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
                char ack_rep[] = { (char)cp_rply::NAK };
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
        char ack_rep[] = { (char)cp_rply::NAK };
        return queue_packet(ack_rep, 1);
    }
}

handle_t control_conn_t::allocate_service_handle(service_record *record)
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

void control_conn_t::service_event(service_record *service, service_event_t event) noexcept
{
    // For each service handle corresponding to the event, send an information packet.
    auto range = service_key_map.equal_range(service);
    auto &i = range.first;
    auto &end = range.second;
    try {
        while (i != end) {
            uint32_t key = i->second;
            std::vector<char> pkt;

            // There are two types of service event packet: v5+, and the original. For backwards
            // compatibility, we send both. The new packet is sent first since this simplifies
            // things for the client (eg if waiting for an event, the client will receive the
            // packet with the most information first).

            // packet type (byte) + packet length (byte) + event type (byte) + key + status buffer
            constexpr int pktsize5 = 3 + sizeof(key) + STATUS_BUFFER5_SIZE;
            pkt.reserve(pktsize5);
            pkt.push_back((char)cp_info::SERVICEEVENT5);
            pkt.push_back(pktsize5);
            char *p = (char *)&key;
            for (unsigned j = 0; j < sizeof(key); j++) {
                pkt.push_back(*p++);
            }
            pkt.push_back(static_cast<char>(event));
            pkt.resize(pktsize5);
            fill_status_buffer5(pkt.data() + 3 + sizeof(key), service);
            queue_packet(std::move(pkt));

            pkt.clear();

            constexpr int pktsize = 3 + sizeof(key) + STATUS_BUFFER_SIZE;
            pkt.reserve(pktsize);
            pkt.push_back((char)cp_info::SERVICEEVENT);
            pkt.push_back(pktsize);
            p = (char *)&key;
            for (unsigned j = 0; j < sizeof(key); j++) {
                pkt.push_back(*p++);
            }
            pkt.push_back(static_cast<char>(event));
            pkt.resize(pktsize);
            fill_status_buffer(pkt.data() + 3 + sizeof(key), service);
            queue_packet(std::move(pkt));

            ++i;
        }
    }
    catch (std::bad_alloc &exc) {
        do_oom_close();
    }
}

void control_conn_t::environ_event(environment *env, std::string const &var_and_val, bool overridden) noexcept
{
    // packet type (byte) + packet length (byte) + flags byte + data size + data
    // flags byte can be 1 or 0 for now, 1 if the var was overridden and 0 if fresh
    constexpr int pktsize = 3 + sizeof(envvar_len_t);
    envvar_len_t ln = var_and_val.size() + 1;
    auto *ptr = var_and_val.data();

    try {
        std::vector<char> pkt;
        pkt.reserve(pktsize + ln);
        pkt.push_back((char)cp_info::ENVEVENT);
        pkt.push_back(pktsize);
        pkt.push_back(overridden ? 1 : 0);
        pkt.insert(pkt.end(), (char *)&ln, ((char *)&ln) + sizeof(envvar_len_t));
        pkt.insert(pkt.end(), ptr, ptr + ln);
        queue_packet(std::move(pkt));
    } catch (std::bad_alloc &exc) {
        do_oom_close();
    }
}

bool control_conn_t::queue_packet(const char *pkt, unsigned size) noexcept
{
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
                return true;
            }
            pkt += wr;
            size -= wr;
        }
    }
    
    // Create a vector out of the (remaining part of the) packet:
    try {
        outbuf.emplace_back(pkt, pkt + size);
        outbuf_size += size;
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
            return true;
        }
    }
}

// This queue_packet method is frustratingly similar to the one above, but the subtle differences
// make them extraordinary difficult to combine into a single method.
bool control_conn_t::queue_packet(std::vector<char> &&pkt) noexcept
{
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
                return true;
            }
            outpkt_index = wr;
        }
    }
    
    try {
        outbuf.emplace_back(std::move(pkt));
        outbuf_size += pkt.size();
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
            if (errno != ECONNRESET) {
                log(loglevel_t::WARN, "Error reading from control connection: ", strerror(errno));
            }
            return true;
        }
        return false;
    }
    
    if (r == 0) {
        return true;
    }
    
    // complete packet?
    while (rbuf.get_length() >= chklen) {
        try {
            if (!process_packet() || bad_conn_close) {
                return false;
            }
        }
        catch (std::bad_alloc &baexc) {
            do_oom_close();
            return false;
        }

        chklen = std::max(chklen, 1u);
    }

    if (rbuf.get_length() == rbuf.get_size()) {
        // Too big packet
        log(loglevel_t::WARN, "Received too-large control packet; dropping connection");
        bad_conn_close = true;
    }
    
    return false;
}

bool control_conn_t::send_data() noexcept
{
    if (outbuf.empty() && bad_conn_close) {
        if (oom_close) {
            // Send oom response
            char oomBuf[] = { (char)cp_rply::OOM };
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
            return false;
        }
        else {
            log(loglevel_t::ERROR, "Error writing to control connection: ", strerror(errno));
            return true;
        }
    }

    outpkt_index += written;
    if (outpkt_index == pkt.size()) {
        // We've finished this packet, move on to the next:
        outbuf_size -= pkt.size();
        outbuf.pop_front();
        outpkt_index = 0;
        if (oom_close) {
            // remain active, try to send cp_rply::OOM shortly
            return false;
        }
        if (outbuf.empty() && bad_conn_close) {
            return true;
        }
    }
    
    // more to send
    return false;
}

control_conn_t::~control_conn_t() noexcept
{
    int fd = iob.get_watched_fd();
    iob.deregister(loop);
    bp_sys::close(fd);
    
    // Clear service listeners
    for (auto p : service_key_map) {
        p.first->remove_listener(this);
    }
    main_env.remove_listener(this);
    
    active_control_conns--;
}
