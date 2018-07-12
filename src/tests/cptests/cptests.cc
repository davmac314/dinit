#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <set>

#include "dinit.h"
#include "service.h"
#include "baseproc-sys.h"
#include "control.h"

// Control protocol tests.

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

    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name_2);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name_2, service_name_2 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

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

    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name_2);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name_2, service_name_2 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

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
    assert(s == service_state_t::STARTED);
    service_state_t ts = static_cast<service_state_t>(wdata[6]);
    assert(ts == service_state_t::STARTED);

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
    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name, service_name + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

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

    control_conn_t::handle_t h;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(h), reinterpret_cast<char *>(&h));

    // Issue start:
    cmd = { DINIT_CP_STARTSERVICE, 0 /* don't pin */ };
    char * h_cp = reinterpret_cast<char *>(&h);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

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

    // Get a service handle:
    std::vector<char> cmd = { DINIT_CP_FINDSERVICE };
    uint16_t name_len = strlen(service_name1);
    char *name_len_cptr = reinterpret_cast<char *>(&name_len);
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name1, service_name1 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

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

    control_conn_t::handle_t h1;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(h1), reinterpret_cast<char *>(&h1));

    // Issue unload:
    cmd = { DINIT_CP_UNLOADSERVICE };
    char * h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive NAK, as the service has a dependency:
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] = DINIT_RP_NAK);

    // Get handle for service 2:
    cmd = { DINIT_CP_FINDSERVICE };
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name2, service_name2 + name_len);

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

    control_conn_t::handle_t h2;
    std::copy(wdata.data() + 2, wdata.data() + 2 + sizeof(h2), reinterpret_cast<char *>(&h2));

    // Issue unload for s2:

    cmd = { DINIT_CP_UNLOADSERVICE };
    h_cp = reinterpret_cast<char *>(&h2);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h2));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive ACK:
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] = DINIT_RP_NAK);

    // Now try to unload s1 again:

    cmd = { DINIT_CP_UNLOADSERVICE };
    h_cp = reinterpret_cast<char *>(&h1);
    cmd.insert(cmd.end(), h_cp, h_cp + sizeof(h1));

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // We should receive ACK:
    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] = DINIT_RP_NAK);

    // If we try to FIND service 1 now, it should not be there:
    cmd = { DINIT_CP_FINDSERVICE };
    cmd.insert(cmd.end(), name_len_cptr, name_len_cptr + sizeof(name_len));
    cmd.insert(cmd.end(), service_name1, service_name1 + name_len);

    bp_sys::supply_read_data(fd, std::move(cmd));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    bp_sys::extract_written_data(fd, wdata);
    assert(wdata.size() == 1);
    assert(wdata[0] = DINIT_RP_NOSERVICE);

    delete cc;
}


#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(cptest_queryver, "    ");
    RUN_TEST(cptest_listservices, "");
    RUN_TEST(cptest_findservice1, "");
    RUN_TEST(cptest_findservice2, "");
    RUN_TEST(cptest_findservice3, "");
    RUN_TEST(cptest_loadservice, " ");
    RUN_TEST(cptest_startstop, "   ");
    RUN_TEST(cptest_unload, "      ");
    return 0;
}
