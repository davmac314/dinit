#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <set>

#include "dinit.h"
#include "service.h"
#include "baseproc-sys.h"
#include "control.h"

#include "../test_service.h"

// Control protocol tests.

class control_conn_t_test
{
    public:
    static service_record * service_from_handle(control_conn_t *cc, control_conn_t::handle_t handle)
    {
        return cc->find_service_for_key(handle);
    }
};

void cptest_queryver()
{
	service_set sset;
	int fd = bp_sys::allocfd();
	auto *cc = new control_conn_t(event_loop, &sset, fd);

	bp_sys::supply_read_data(fd, { DINIT_CP_QUERYVERSION });

	event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

	// Write will process immediately, so there's no need for this:
	//event_loop.regd_bidi_watchers[fd]->write_ready(event_loop, fd);

	// We expect a version number back:
	std::vector<char> wdata;
	bp_sys::extract_written_data(fd, wdata);

	assert(wdata.size() == 5);
	assert(wdata[0] == DINIT_RP_CPVERSION);

	delete cc;
}

void cptest_listservices()
{
	service_set sset;

	service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
	sset.add_service(s1);
	service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
	sset.add_service(s2);
	service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
	sset.add_service(s3);

	int fd = bp_sys::allocfd();
	auto *cc = new control_conn_t(event_loop, &sset, fd);

	bp_sys::supply_read_data(fd, { DINIT_CP_LISTSERVICES });

	event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

	// Write will process immediately, so there's no need for this:
	//event_loop.regd_bidi_watchers[fd]->write_ready(event_loop, fd);

	// We expect, for each service:
	// (1 byte)   DINIT_RP_SVCINFO
	// (1 byte)   service name length
	// (1 byte)   state
	// (1 byte)   target state
	// (1 byte)   flags: has console, waiting for console, start skipped
	// (1 byte)   stop reason
    // (2 bytes)  reserved
	// (? bytes)  exit status (int) / process id (pid_t)
	// (N bytes)  service name

	std::vector<char> wdata;
	bp_sys::extract_written_data(fd, wdata);

	std::set<std::string> names = {"test-service-1", "test-service-2", "test-service-3"};

	int pos = 0;
	for (int i = 0; i < 3; i++) {
		assert(wdata[pos++] == DINIT_RP_SVCINFO);
		unsigned char name_len_c = wdata[pos++];
		pos += 6;

		pos += std::max(sizeof(int), sizeof(pid_t));

		std::string name;
		for (int j = 0; j < (int)name_len_c; j++) {
			name += wdata[pos++];
		}

		// Check the service name matches one from the set, and remove it:
		auto fn = names.find(name);
		assert (fn != names.end());
		names.erase(fn);
	}

	delete cc;
}

static control_conn_t::handle_t  find_service(int fd, const char *service_name,
        service_state_t expected_state, service_state_t expected_target_state)
{
    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name, service_name + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    // We expect:
    // (1 byte)   DINIT_RP_SERVICERECORD
    // (1 byte)   state
    // (handle_t) handle
    // (1 byte)   target state

    assert(wdata.size() == 3 + sizeof(control_conn_t::handle_t));
    assert(wdata[0] == DINIT_RP_SERVICERECORD);
    service_state_t s = static_cast<service_state_t>(wdata[1]);
    assert(s == expected_state);
    service_state_t ts = static_cast<service_state_t>(wdata[6]);
    assert(ts == expected_target_state);

    control_conn_t::handle_t h1;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(h1), reinterpret_cast<char *>(&h1));

    return h1;
}

void cptest_findservice1()
{
    service_set sset;

    const char * const service_name_2 = "test-service-2";

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    sset.add_service(s2);
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    sset.add_service(s3);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    find_service(fd, service_name_2, service_state_t::STOPPED, service_state_t::STOPPED);

    delete cc;
}

void cptest_findservice2()
{
    service_set sset;

    const char * const service_name_2 = "test-service-2";

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    sset.add_service(s2);
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    sset.add_service(s3);

    sset.start_service(s2);
    sset.process_queues();

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    find_service(fd, service_name_2, service_state_t::STARTED, service_state_t::STARTED);

    delete cc;
}

// test finding non-existing service
void cptest_findservice3()
{
    service_set sset;

    const char * const service_name_2 = "test-service-n";

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    sset.add_service(s2);
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    sset.add_service(s3);

    sset.start_service(s2);
    sset.process_queues();

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name_2);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name_2, service_name_2 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We expect:
    // (1 byte)   DINIT_RP_NOSERVICE

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_NOSERVICE);

    delete cc;
}

class test_service_set : public service_set
{
    public:
    service_record * service1 = nullptr;
    service_record * service2 = nullptr;

    virtual service_record *load_service(const char *name) override
    {
        auto r = find_service(name);
        if (r == nullptr) {
            if (strcmp(name, "test-service-1") == 0) {
                service1 = new service_record(this, "test-service-1");
                add_service(service1);
                return service1;
            }
            else if (strcmp(name, "test-service-2") == 0) {
                service2 = new service_record(this, "test-service-2");
                add_service(service2);
                return service2;
            }
            throw service_not_found(name);
        }
        return r;
    }
};

void cptest_loadservice()
{
    test_service_set sset;

    const char * const service_name_1 = "test-service-1";
    const char * const service_name_2 = "test-service-2";

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    std::vector<char> cmd = { DINIT_CP_LOADSERVICE };
    uint16_t name_len = strlen(service_name_1);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name_1, service_name_1 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));
    bp_sys::set_blocking(fd);

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We expect:
    // (1 byte)   DINIT_RP_SERVICERECORD
    // (1 byte)   state
    // (handle_t) handle
    // (1 byte)   target state

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 3 + sizeof(control_conn_t::handle_t));
    assert(wdata[0] == DINIT_RP_SERVICERECORD);
    service_state_t s = static_cast<service_state_t>(wdata[1]);
    assert(s == service_state_t::STOPPED);
    service_state_t ts = static_cast<service_state_t>(wdata[6]);
    assert(ts == service_state_t::STOPPED);

    assert(sset.service1 != nullptr);
    assert(sset.service2 == nullptr);

    cmd = { DINIT_CP_LOADSERVICE };
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name_2, service_name_2 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We expect:
    // (1 byte)   DINIT_RP_SERVICERECORD
    // (1 byte)   state
    // (handle_t) handle
    // (1 byte)   target state

    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 3 + sizeof(control_conn_t::handle_t));
    assert(wdata[0] == DINIT_RP_SERVICERECORD);
    s = static_cast<service_state_t>(wdata[1]);
    assert(s == service_state_t::STOPPED);
    ts = static_cast<service_state_t>(wdata[6]);
    assert(ts == service_state_t::STOPPED);

    assert(sset.service1 != nullptr);
    assert(sset.service2 != nullptr);

    delete cc;
}

void cptest_startstop()
{
    service_set sset;

    const char * const service_name = "test-service-1";

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    // Get a service handle:
    control_conn_t::handle_t h = find_service(fd, service_name, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Issue start:
    std::vector<char> cmd = { DINIT_CP_STARTSERVICE, 0 /* don't pin */ };
    char * h_cp = reinterpret_cast<char *>(&h);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1 + 7 /* ACK reply + info packet */);
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    // packetsize, key (handle), event
    assert(wdata[1] == 7);
    control_conn_t::handle_t ip_h;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h);
    assert(wdata[6] == static_cast<int>(service_event_t::STARTED));
    // we get ALREADYSS since it started immediately:
    assert(wdata[7] == DINIT_RP_ALREADYSS);
    assert(s1->get_state() == service_state_t::STARTED);

    // Issue stop:
    cmd = { DINIT_CP_STOPSERVICE, 0 /* don't pin */ };
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1 + 7);
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    // packetsize, key (handle), event
    assert(wdata[1] == 7);
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h);
    assert(wdata[6] == static_cast<int>(service_event_t::STOPPED));
    // we get ALREADYSS since it stopped immediately:
    assert(wdata[7] == DINIT_RP_ALREADYSS);
    assert(s1->get_state() == service_state_t::STOPPED);

    delete cc;
}

void cptest_start_pinned()
{
    service_set sset;

    const char * const service_name = "test-service-1";

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    s1->pin_stop();

    // Get a service handle:
    control_conn_t::handle_t h = find_service(fd, service_name, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Issue start:
    std::vector<char> cmd = { DINIT_CP_STARTSERVICE, 0 /* don't pin */ };
    char * h_cp = reinterpret_cast<char *>(&h);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1 /* DINIT_RP_PINNEDSTOPPED */);
    assert(wdata[0] == DINIT_RP_PINNEDSTOPPED);

    delete cc;
}

void cptest_gentlestop()
{
    service_set sset;

    const char * const test1_name = "test-service-1";

    service_record *s1 = new service_record(&sset, test1_name, service_type_t::INTERNAL, {});
    sset.add_service(s1);

    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL,
            {{s1, dependency_type::REGULAR}});
    sset.add_service(s2);

    // Start the services:
    sset.start_service(s2);
    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    control_conn_t::handle_t h = find_service(fd, test1_name, service_state_t::STARTED,
            service_state_t::STARTED);

    char * h_cp = reinterpret_cast<char *>(&h);

    // Issue stop:
    std::vector<char> cmd = { DINIT_CP_STOPSERVICE, 2 /* don't pin, gentle */ };
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    // We expect:
    // 1 byte: DINIT_RP_DEPENDENTS
    // size_t: number of handles (N)
    // N * handle_t: handles for dependents that would be stopped

    assert(wdata.size() == (1 + sizeof(size_t) + sizeof(control_conn_t::handle_t)));
    assert(wdata[0] == DINIT_RP_DEPENDENTS);

    size_t nhandles;
    memcpy(&nhandles, wdata.data() + 1, sizeof(nhandles));
    assert(nhandles == 1);

    control_conn_t::handle_t rhandle;
    memcpy(&rhandle, wdata.data() + 1 + sizeof(size_t), sizeof(rhandle));

    service_record * rservice = control_conn_t_test::service_from_handle(cc, rhandle);
    assert(rservice == s2);

    delete cc;
}

void cptest_queryname()
{
    service_set sset;

    const char * const test1_name = "test-service-1";

    service_record *s1 = new service_record(&sset, test1_name, service_type_t::INTERNAL, {});
    sset.add_service(s1);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    // Get a service handle:
    control_conn_t::handle_t h = find_service(fd, test1_name, service_state_t::STOPPED,
            service_state_t::STOPPED);

    char * h_cp = reinterpret_cast<char *>(&h);

    // Issue name query:
    std::vector<char> cmd = { DINIT_CP_QUERYSERVICENAME, 0 /* reserved */ };
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    // We expect:
    // 1 byte packet type = DINIT_RP_SERVICENAME
    // 1 byte reserved
    // uint16_t length
    // N bytes name

    assert(wdata.size() == (2 + sizeof(uint16_t) + strlen(test1_name)));
    assert(wdata[0] == DINIT_RP_SERVICENAME);
    assert(wdata[1] == 0);
    uint16_t len;
    memcpy(&len, wdata.data() + 2, sizeof(uint16_t));
    assert(len == strlen(test1_name));

    assert(strncmp(wdata.data() + 2 + sizeof(uint16_t), test1_name, strlen(test1_name)) == 0);

    delete cc;
}

void cptest_unload()
{
    service_set sset;

    const char * const service_name1 = "test-service-1";
    const char * const service_name2 = "test-service-2";

    service_record *s1 = new service_record(&sset, service_name1, service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, service_name2, service_type_t::INTERNAL,
            {{ s1, dependency_type::WAITS_FOR }});
    sset.add_service(s2);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    control_conn_t::handle_t h1 = find_service(fd, service_name1, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Issue unload:
    std::vector<char> cmd = { DINIT_CP_UNLOADSERVICE };
    char * h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive NAK, as the service has a dependency:
    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_NAK);


    control_conn_t::handle_t h2 = find_service(fd, service_name2, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Issue unload for s2:

    cmd = { DINIT_CP_UNLOADSERVICE };
    h_cp = reinterpret_cast<char *>(&h2);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h2));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive ACK:
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_ACK);

    // Now try to unload s1 again:

    cmd = { DINIT_CP_UNLOADSERVICE };
    h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive ACK:
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_ACK);

    // If we try to FIND service 1 now, it should not be there:
    cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name1);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name1, service_name1 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_NOSERVICE);

    delete cc;
}

void cptest_addrmdeps()
{
    service_set sset;

    const char * const service_name1 = "test-service-1";
    const char * const service_name2 = "test-service-2";

    service_record *s1 = new service_record(&sset, service_name1, service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, service_name2, service_type_t::INTERNAL, {});
    sset.add_service(s2);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    control_conn_t::handle_t h1 = find_service(fd, service_name1, service_state_t::STOPPED,
            service_state_t::STOPPED);
    control_conn_t::handle_t h2 = find_service(fd, service_name2, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Add dep from s1 -> s2:
    std::vector<char> cmd = { DINIT_CP_ADD_DEP, static_cast<char>(dependency_type::REGULAR) };
    char * h1cp = reinterpret_cast<char *>(&h1);
    char * h2cp = reinterpret_cast<char *>(&h2);
    cmd.insert(cmd.end(), h1cp, h1cp + sizeof(h1));
    cmd.insert(cmd.end(), h2cp, h2cp + sizeof(h2));

    bp_sys::supply_read_data(fd, std::move(cmd));
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_ACK);

    // Issue start for S1. S2 should also start:
    cmd = { DINIT_CP_STARTSERVICE, 0 /* don't pin */ };
    cmd.insert(cmd.end(), h1cp, h1cp + sizeof(h1));

    bp_sys::supply_read_data(fd, std::move(cmd));
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1 + 7 + 7); // ACK + 2 * info packets
    assert(s1->get_state() == service_state_t::STARTED);
    assert(s2->get_state() == service_state_t::STARTED);

    // Remove dependency from S1 -> S2:
    cmd = { DINIT_CP_REM_DEP, static_cast<char>(dependency_type::REGULAR) };
    cmd.insert(cmd.end(), h1cp, h1cp + sizeof(h1));
    cmd.insert(cmd.end(), h2cp, h2cp + sizeof(h2));

    bp_sys::supply_read_data(fd, std::move(cmd));
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1 + 7); // ACK + info packet
    assert(s2->get_state() == service_state_t::STOPPED);

    delete cc;
}

void cptest_enableservice()
{
    service_set sset;

    const char * const service_name1 = "test-service-1";
    const char * const service_name2 = "test-service-2";

    service_record *s1 = new service_record(&sset, service_name1, service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, service_name2, service_type_t::INTERNAL, {});
    sset.add_service(s2);

    s1->start();
    sset.process_queues();

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    control_conn_t::handle_t h1 = find_service(fd, service_name1, service_state_t::STARTED, service_state_t::STARTED);
    control_conn_t::handle_t h2 = find_service(fd, service_name2, service_state_t::STOPPED, service_state_t::STOPPED);

    // Enable from s1 -> s2:
    std::vector<char> cmd = { DINIT_CP_ENABLESERVICE, static_cast<char>(dependency_type::WAITS_FOR) };
    char * h1cp = reinterpret_cast<char *>(&h1);
    char * h2cp = reinterpret_cast<char *>(&h2);
    cmd.insert(cmd.end(), h1cp, h1cp + sizeof(h1));
    cmd.insert(cmd.end(), h2cp, h2cp + sizeof(h2));

    bp_sys::supply_read_data(fd, std::move(cmd));
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1 + 7 /* ACK reply + info packet */);
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    // packetsize, key (handle), event
    assert(wdata[1] == 7);
    control_conn_t::handle_t ip_h;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h2);
    assert(wdata[6] == static_cast<int>(service_event_t::STARTED));

    // and then the ack:
    assert(wdata[7] == DINIT_RP_ACK);

    sset.process_queues();

    // We expect that s2 is now started:
    assert(s2->get_state() == service_state_t::STARTED);

    s1->stop();
    sset.process_queues();

    assert(s2->get_state() == service_state_t::STOPPED);

    bp_sys::extract_written_data(fd, wdata);

    delete cc;
}

void cptest_restart()
{
    service_set sset;

    const char * const service_name = "test-service-1";

    test_service *s1 = new test_service(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    // Get a service handle:
    control_conn_t::handle_t h = find_service(fd, service_name, service_state_t::STOPPED,
            service_state_t::STOPPED);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 0);

    // Issue restart:
    std::vector<char> cmd = { DINIT_CP_STOPSERVICE, 4 /* restart */ };
    char * h_cp = reinterpret_cast<char *>(&h);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, cmd);

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1 /* NAK reply, wrong state */);
    assert(wdata[0] == DINIT_RP_NAK);

    // Start the service now:
    s1->start();
    sset.process_queues();
    s1->started();
    sset.process_queues();

    bp_sys::extract_written_data(fd, wdata);

    // Issue restart (again):
    bp_sys::supply_read_data(fd, std::move(cmd));
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 7 + 1);  // info packet (service stopped) + ACK
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    assert(wdata[1] == 7);
    control_conn_t::handle_t ip_h;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h);
    assert(wdata[6] == static_cast<int>(service_event_t::STOPPED));
    assert(wdata[7] == DINIT_RP_ACK);

    sset.process_queues();
    assert(s1->get_state() == service_state_t::STARTING);

    s1->started();
    sset.process_queues();
    assert(s1->get_state() == service_state_t::STARTED);

    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 7);  /* info packet */
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    // packetsize, key (handle), event
    assert(wdata[1] == 7);
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h);
    assert(wdata[6] == static_cast<int>(service_event_t::STARTED));

    delete cc;
}

void cptest_wake()
{
    service_set sset;

    const char * const service_name1 = "test-service-1";
    const char * const service_name2 = "test-service-2";

    service_record *s1 = new service_record(&sset, service_name1, service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, service_name2, service_type_t::INTERNAL,
            {{ s1, dependency_type::WAITS_FOR }});
    sset.add_service(s2);

    s2->start();
    sset.process_queues();

    s1->stop(true);
    sset.process_queues();

    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STARTED);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    control_conn_t::handle_t h1 = find_service(fd, service_name1, service_state_t::STOPPED,
            service_state_t::STOPPED);

    // Wake s1:
    std::vector<char> cmd = { DINIT_CP_WAKESERVICE, 0 /* don't pin */ };
    char * h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));
    bp_sys::supply_read_data(fd, std::move(cmd));

    std::vector<char> wdata;
    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1 + 7 /* ACK reply + info packet */);
    assert(wdata[0] == DINIT_IP_SERVICEEVENT);
    // packetsize, key (handle), event
    assert(wdata[1] == 7);
    control_conn_t::handle_t ip_h;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(ip_h), reinterpret_cast<char *>(&ip_h));
    assert(ip_h == h1);
    assert(wdata[6] == static_cast<int>(service_event_t::STARTED));

    // and then the ack (already started):
    assert(wdata[7] == DINIT_RP_ALREADYSS);

    // now stop s2 (and therefore s1):
    s2->stop(true);
    sset.process_queues();
    assert(s1->get_state() == service_state_t::STOPPED);
    assert(s2->get_state() == service_state_t::STOPPED);

    // Clear any info packets:
    bp_sys::extract_written_data(fd, wdata);

    // Trying to wake s1 should now fail:
    cmd = { DINIT_CP_WAKESERVICE, 0 /* don't pin */ };
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));
    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == 1);
    assert(wdata[0] == DINIT_RP_NAK);

    delete cc;
}

void cptest_servicestatus()
{
    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    sset.add_service(s2);
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    sset.add_service(s3);

    s2->start();
    sset.process_queues();

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    auto STOPPED = service_state_t::STOPPED;
    auto STARTED = service_state_t::STARTED;
    control_conn_t::handle_t h1 = find_service(fd, "test-service-1", STOPPED, STOPPED);
    control_conn_t::handle_t h2 = find_service(fd, "test-service-2", STARTED, STARTED);
    control_conn_t::handle_t h3 = find_service(fd, "test-service-3", STOPPED, STOPPED);

    std::vector<char> cmd = { DINIT_CP_SERVICESTATUS };
    char * h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));
    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    constexpr static int STATUS_BUFFER_SIZE = 6 + ((sizeof(pid_t) > sizeof(int)) ? sizeof(pid_t) : sizeof(int));

    // 1 byte: DINIT_RP_SERVICESTATUS
    // 1 byte: reserved
    // STATUS_BUFFER_SIZE bytes: status
    assert(wdata.size() == (2 + STATUS_BUFFER_SIZE));
    assert(wdata[0] == DINIT_RP_SERVICESTATUS);
    assert(wdata[2] == (int)service_state_t::STOPPED); // state
    assert(wdata[3] == (int)service_state_t::STOPPED); // target state
    assert(wdata[4] == 0); // various flags

    cmd.clear();
    cmd.push_back(DINIT_CP_SERVICESTATUS);
    h_cp = reinterpret_cast<char *>(&h2);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));
    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == (2 + STATUS_BUFFER_SIZE));
    assert(wdata[0] == DINIT_RP_SERVICESTATUS);
    assert(wdata[2] == (int)service_state_t::STARTED); // state
    assert(wdata[3] == (int)service_state_t::STARTED); // target state
    assert(wdata[4] == 8); // various flags; 8 = marked active

    (void)h3; // silence warning

    delete cc;
}


#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(cptest_queryver, "           ");
    RUN_TEST(cptest_listservices, "       ");
    RUN_TEST(cptest_findservice1, "       ");
    RUN_TEST(cptest_findservice2, "       ");
    RUN_TEST(cptest_findservice3, "       ");
    RUN_TEST(cptest_loadservice, "        ");
    RUN_TEST(cptest_startstop, "          ");
    RUN_TEST(cptest_start_pinned, "       ");
    RUN_TEST(cptest_gentlestop, "         ");
    RUN_TEST(cptest_queryname, "          ");
    RUN_TEST(cptest_unload, "             ");
    RUN_TEST(cptest_addrmdeps, "          ");
    RUN_TEST(cptest_enableservice, "      ");
    RUN_TEST(cptest_restart, "            ");
    RUN_TEST(cptest_wake, "               ");
    RUN_TEST(cptest_servicestatus, "      ");
    return 0;
}
