#include <cassert>
#include <iostream>

#include "dinit.h"
#include "service.h"
#include "baseproc-sys.h"
#include "control.h"

#define RUN_TEST(name) \
    std::cout << #name "... "; \
    name(); \
    std::cout << "PASSED" << std::endl;

void cptest1()
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

int main(int argc, char **argv)
{
    RUN_TEST(cptest1);
    return 0;
}
