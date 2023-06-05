#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <set>

#include "dinit.h"
#include "service.h"
#include "baseproc-sys.h"
#include "control.h"

// Control protocol fuzzing.


extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
    if (Size == 0) return 0;

    service_set sset;

    service_record *s1 = new service_record(&sset, "test-service-1", service_type_t::INTERNAL, {});
    sset.add_service(s1);
    service_record *s2 = new service_record(&sset, "test-service-2", service_type_t::INTERNAL, {});
    sset.add_service(s2);
    service_record *s3 = new service_record(&sset, "test-service-3", service_type_t::INTERNAL, {});
    sset.add_service(s3);

    int fd = bp_sys::allocfd();
    auto *cc = new control_conn_t(event_loop, &sset, fd);

    std::vector<char> input_data(Data, Data + Size);
    bp_sys::supply_read_data(fd, std::move(input_data));

    event_loop.regd_bidi_watchers[fd]->read_ready(event_loop, fd);

    // Write will process immediately, so there's no need for this:
    //event_loop.regd_bidi_watchers[fd]->write_ready(event_loop, fd);

    delete cc;

    return 0;
}
