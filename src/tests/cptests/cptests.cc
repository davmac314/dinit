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
    return 0;
}
