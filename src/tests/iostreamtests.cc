#include <array>
#include <iostream>
#include <cassert>

#include "baseproc-sys.h"
#include "dinit-iostream.h"

#ifdef NDEBUG
#error "This file must be built with assertions ENABLED!"
#endif

void ostream_basic_test()
{
    int fd = bp_sys::allocfd();
    dio::ostream stream(fd);
    dio::streambuf* buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    const char msg[] = "This is a test message!\n";

    assert(stream.write(msg));
    assert(buf->get_length());

    char* ptr = buf->get_ptr(0);
    unsigned len = buf->get_contiguous_length(ptr);
    assert(strncmp(ptr, msg, len) == 0);

    assert(stream.flush_nx() == sizeof(msg) - 1);
    assert(buf->get_length() == 0);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == sizeof(msg) - 1);
    assert(strncmp(wdata.data(), msg, wdata.size()) == 0);
}

void ostream_write_buf_test()
{
    int fd = bp_sys::allocfd();
    dio::ostream stream(fd);
    dio::streambuf* buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    const char msg[] = "This is a test message!\n";

    assert(stream.write_buf(msg) == sizeof(msg) - 1);
    assert(buf->get_length() == sizeof(msg) - 1);

    char* ptr = buf->get_ptr(0);
    unsigned len = buf->get_contiguous_length(ptr);
    assert(strncmp(ptr, msg, len) == 0);

    assert(stream.flush_nx() == sizeof(msg) - 1);
    assert(buf->get_length() == 0);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == sizeof(msg) - 1);
    assert(strncmp(wdata.data(), msg, wdata.size()) == 0);
}

void ostream_types_test()
{
    int fd = bp_sys::allocfd();
    dio::ostream stream(fd);
    dio::streambuf *buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    const char msg1[] = "The 2 + 2 equals to: ";
    const int msg2 = 2 + 2;
    const char msg_full[] = "The 2 + 2 equals to: 4";

    assert(stream.write(msg1, msg2));
    assert(buf->get_length() == sizeof(msg_full) - 1);

    char* ptr = buf->get_ptr(0);
    unsigned len = buf->get_contiguous_length(ptr);
    assert(strncmp(ptr, msg_full, len) == 0);

    assert(stream.flush_nx() == sizeof(msg_full) - 1);
    assert(buf->get_length() == 0);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == sizeof(msg_full) - 1);
    assert(strncmp(wdata.data(), msg_full, wdata.size()) == 0);
}

void ostream_large_msg_test()
{
    int fd = bp_sys::allocfd();
    dio::ostream stream(fd);
    dio::streambuf *buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    char msg[IOSTREAM_BUFSIZE + 2];
    std::fill_n(msg, IOSTREAM_BUFSIZE + 1, 'a');
    msg[IOSTREAM_BUFSIZE + 1] = '\0';

    assert(stream.write(msg));
    assert(buf->get_length() == 0);

    std::vector<char> wdata;
    bp_sys::extract_written_data(fd, wdata);

    assert(wdata.size() == sizeof(msg) - 1);
    assert(strncmp(wdata.data(), msg, wdata.size()) == 0);
}

void istream_basic_test()
{
    std::vector<char> wdata = { 'L', '1', '\n', 'L','2', '\n', '\n', 'L', '3' };
    bp_sys::supply_file_content("file", std::move(wdata));

    dio::istream stream;
    assert(stream.open_nx("file"));
    assert(stream.is_open());
    assert(stream.get_fd() >= 0);
    dio::streambuf *buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    std::string line;

    assert(dio::get_line(stream, line) == 2);
    assert(line.compare("L1") == 0);

    assert(dio::get_line(stream, line) == 2);
    assert(line.compare("L2") == 0);

    assert(dio::get_line(stream, line) == 1);
    assert(line.size() == 0);

    assert(dio::get_line(stream, line) == 2);
    assert(line.compare("L3") == 0);

    assert(dio::get_line_until_eof(stream, line) == 0);
    assert(stream.eof());
    assert(line.compare("L3") == 0);

    assert(stream.close());
}

void istream_buffer_boundary_test()
{
    int fd = bp_sys::allocfd();
    dio::istream stream(fd);
    dio::streambuf *buf = stream.get_buf();
    assert(buf != nullptr);
    assert(stream.good());

    std::array<char, IOSTREAM_BUFSIZE> msg;
    msg.fill('a');
    buf->append(msg.begin(), msg.size());

    std::fill_n(msg.begin(), 100, 'b');
    buf->consume(100);
    buf->append(msg.begin(), 100);

    std::string line;
    assert(dio::get_line(stream, line) == IOSTREAM_BUFSIZE);
    assert(strncmp(line.c_str(), msg.begin(), IOSTREAM_BUFSIZE));
}

#define RUN_TEST(name, spacing) \
    std::cout << #name "..." spacing << std::flush; \
    name(); \
    std::cout << "PASSED" << std::endl;

int main(int argc, char **argv)
{
    RUN_TEST(ostream_basic_test, "           ");
    RUN_TEST(ostream_write_buf_test, "       ");
    RUN_TEST(ostream_types_test, "           ");
    RUN_TEST(ostream_large_msg_test, "       ");
    RUN_TEST(istream_basic_test, "           ");
    RUN_TEST(istream_buffer_boundary_test, " ");

    // Early close cout/cerr/cin objects fds to avoid use-after-free
    // problems due to destruction of "usedfds" vector before those objects destruction.
    // See https://github.com/davmac314/dinit/pull/263#issuecomment-1936208515
    // and https://github.com/davmac314/dinit/pull/263#issuecomment-1937414602
    dio::cout.close_nx();
    dio::cerr.close_nx();
    dio::cin.close_nx();

    return 0;
}
