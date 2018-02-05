#include <cstdint>

// Client library for Dinit clients


using handle_t = uint32_t;

class read_cp_exception
{
    public:
    int errcode;
    read_cp_exception(int err) : errcode(err) { }
};

// Fill a circular buffer from a file descriptor, until it contains at least _rlength_ bytes.
// Throws read_cp_exception if the requested number of bytes cannot be read, with:
//     errcode = 0   if end of stream (remote end closed)
//     errcode = errno   if another error occurred
// Note that EINTR is ignored (i.e. the read will be re-tried).
inline void fill_buffer_to(cpbuffer<1024> *buf, int fd, int rlength)
{
    do {
        int r = buf->fill_to(fd, rlength);
        if (r == -1) {
            if (errno != EINTR) {
                throw read_cp_exception(errno);
            }
        }
        else if (r == 0) {
            throw read_cp_exception(0);
        }
        else {
            return;
        }
    }
    while (true);
}

// Wait for a reply packet, skipping over any information packets that are received in the meantime.
inline void wait_for_reply(cpbuffer<1024> &rbuffer, int fd)
{
    fill_buffer_to(&rbuffer, fd, 1);

    while (rbuffer[0] >= 100) {
        // Information packet; discard.
        fill_buffer_to(&rbuffer, fd, 2);
        int pktlen = (unsigned char) rbuffer[1];

        rbuffer.consume(1);  // Consume one byte so we'll read one byte of the next packet
        fill_buffer_to(&rbuffer, fd, pktlen);
        rbuffer.consume(pktlen - 1);
    }
}

// Wait for an info packet. If any other reply packet comes, throw a read_cp_exception.
inline void wait_for_info(cpbuffer<1024> &rbuffer, int fd)
{
    fill_buffer_to(&rbuffer, fd, 2);

    if (rbuffer[0] < 100) {
        throw read_cp_exception(0);
    }

    int pktlen = (unsigned char) rbuffer[1];
    fill_buffer_to(&rbuffer, fd, pktlen);
}

// Write *all* the requested buffer and re-try if necessary until
// the buffer is written or an unrecoverable error occurs.
inline int write_all(int fd, const void *buf, size_t count)
{
    const char *cbuf = static_cast<const char *>(buf);
    int w = 0;
    while (count > 0) {
        int r = write(fd, cbuf, count);
        if (r == -1) {
            if (errno == EINTR) continue;
            return r;
        }
        w += r;
        cbuf += r;
        count -= r;
    }
    return w;
}
