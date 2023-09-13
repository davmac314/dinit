#ifndef DINIT_IOSTREAM_H
#define DINIT_IOSTREAM_H

#include <cstring>
#include "baseproc-sys.h"
#include "cpbuffer.h"

// ToDo: Desc

constexpr unsigned BUF_SIZE = 4096; // MAXMUM size of buffers

namespace dio {

template <int fd> class ostream
{
    private:

        cpbuffer<BUF_SIZE> buf; // Main buffer
        bool stdio_sync = false; // Make sure message writed into dest

        int write(const char *msg) {
            unsigned msg_len = strlen(msg);
            if (msg_len >= buf.get_free()) {
                // Discard messages
                buf.trim_to(0);
            }
            buf.append(msg, msg_len);

            struct iovec outiov[2];

            char *ptr = buf.get_ptr(0);
            unsigned len = buf.get_contiguous_length(ptr);
            char *creptr = ptr + len;  // contiguous region end
            char *eptr = std::find(ptr, creptr, '\n');

            bool will_complete = false; // will complete this message?
            if (eptr != creptr) {
                eptr++;
                will_complete = true;
            }

            len = eptr - ptr;

            outiov[0].iov_base = ptr;
            outiov[0].iov_len = len;
            int iovs_to_write = 1;

            // Do we need the second span?
            if (!will_complete && len != buf.get_length()) {
                ptr = buf.get_buf_base();
                creptr = ptr + buf.get_length() - len;
                eptr = std::find(ptr, creptr, '\n');
                if (eptr != creptr) {
                    eptr++; // include '\n'
                    // It should not ever be the case that we do not now have a complete message
                    will_complete = true;
                }
                outiov[1].iov_base = ptr;
                outiov[1].iov_len = eptr - ptr;
                len += outiov[1].iov_len;
                iovs_to_write = 2;
            }

            ssize_t r = bp_sys::writev(fd, outiov, iovs_to_write);

            if (r >= 0) {
                buf.consume(len);
            }
            else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) {
                return -1;
            }

            if (stdio_sync) {
                fsync(fd);
            }

            return outiov[0].iov_len + outiov[1].iov_len;
        }

    public:
        void sync_with_stdio(bool sync)
        {
            stdio_sync = sync;
        }
        void print(const char *msg)
        {
            write(msg);
        }
        void print(const std::string &msg)
        {
            write(msg.c_str());
        }
        void print(const int num)
        {
            char tmp[100]; // Is 100 good?
            sprintf(tmp, "%d", num);
            write(tmp);
        }
        void print(const unsigned num)
        {
            char tmp[100]; // Is 100 good?
            sprintf(tmp, "%d", num);
            write(tmp);
        }
        template <typename T> void print(T t)
        {
            print(t);
        }
        template <typename T, typename... Args> void print(T t, Args... args)
        {
            print(t);
            print(args...);
        }
};

ostream<1> cout;
template <typename... Args> void print(Args... args)
{
    cout.print(args...);
}
ostream<2> cerr;
template <typename... Args> void printerr(Args... args)
{
    cerr.print(args...);
}

} /* dio namespace */

#endif /* DINIT_IOSTREAM_H */

