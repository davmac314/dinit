#ifndef CPBUFFER_H
#define CPBUFFER_H

#include <string>
#include <cstring>
#include <algorithm>

#include "baseproc-sys.h"

// control protocol buffer, a circular buffer with fixed capacity.
template <unsigned SIZE> class cpbuffer
{
    char buf[SIZE];
    unsigned cur_idx = 0;
    unsigned length = 0;  // number of elements in the buffer
    
    public:
    static constexpr unsigned get_size()
    {
        return SIZE;
    }

    unsigned get_length() noexcept
    {
        return length;
    }
    
    int get_free() noexcept
    {
        return SIZE - length;
    }
    
    char * get_ptr(int index)
    {
        unsigned pos = cur_idx + index;
        if (pos >= SIZE) pos -= SIZE;
    
        return &buf[pos];
    }
    
    char * get_buf_base()
    {
        return buf;
    }
    
    unsigned get_contiguous_length(char *ptr)
    {
        unsigned eidx = cur_idx + length;
        if (eidx >= SIZE) eidx -= SIZE;
        
        if (buf + eidx > ptr) {
            return (buf + eidx) - ptr;
        }
        else {
            return (buf + SIZE) - ptr;
        }
    }
    
    // Fill by reading from the given fd, return positive if some was read or -1 on error.
    int fill(int fd) noexcept
    {
        unsigned pos = cur_idx + length;
        if (pos >= SIZE) pos -= SIZE;
        unsigned max_count = std::min(SIZE - pos, SIZE - length);
        ssize_t r = bp_sys::read(fd, buf + pos, max_count);
        if (r >= 0) {
            length += r;
        }
        return r;
    }
    
    // Fill by reading up to the specified amount of bytes from the given fd,
    // Return is the number of bytes read, 0 on end-of-file or -1 on error.
    int fill(int fd, unsigned limit) noexcept
    {
        unsigned pos = cur_idx + length;
        if (pos >= SIZE) pos -= SIZE;
        unsigned max_count = std::min(SIZE - pos, SIZE - length);
        max_count = std::min(max_count, limit);
        ssize_t r = bp_sys::read(fd, buf + pos, max_count);
        if (r >= 0) {
            length += r;
        }
        return r;
    }

    // fill by reading from the given fd, until at least the specified number of bytes are in
    // the buffer. Return 0 if end-of-file reached before fill complete, or -1 on error.
    int fill_to(int fd, unsigned rlength) noexcept
    {
        while (length < rlength) {
            int r = fill(fd);
            if (r <= 0) return r;
        }
        return 1;
    }
    
    // Trim the buffer to the specified length (must be less than current length)
    void trim_to(unsigned new_length)
    {
        length = new_length;
    }
    
    char operator[](int idx) noexcept
    {
        unsigned dest_idx = cur_idx + idx;
        if (dest_idx >= SIZE) dest_idx -= SIZE;
        return buf[dest_idx];
    }
    
    // Remove the given number of bytes from the start of the buffer.
    void consume(unsigned amount) noexcept
    {
        cur_idx += amount;
        if (cur_idx >= SIZE) cur_idx -= SIZE;
        length -= amount;
    }
    
    // Extract bytes from the buffer. The bytes remain in the buffer.
    void extract(void *dest, unsigned index, unsigned length) noexcept
    {
        index += cur_idx;
        if (index >= SIZE) index -= SIZE;
        if (index + length > SIZE) {
            // wrap-around copy
            int half = SIZE - index;
            std::memcpy(dest, buf + index, half);
            std::memcpy(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(dest) + half),
                    buf, length - half);
        }
        else {
            std::memcpy(dest, buf + index, length);
        }
    }
    
    // Extract string of given length from given index
    // Throws:  std::bad_alloc on allocation failure
    std::string extract_string(unsigned index, unsigned length)
    {
        index += cur_idx;
        if (index >= SIZE) index -= SIZE;
        if (index + length > SIZE) {
            std::string r(buf + index, SIZE - index);
            r.insert(r.end(), buf, buf + length - (SIZE - index));
            return r;
        }
        else {
            return std::string(buf + index, length);
        }
    }
    
    // Append characters to the buffer. Caller must make certain there
    // is enough space to contain the characters first.
    void append(const char * s, unsigned len) noexcept
    {
        unsigned index = cur_idx + length;
        if (index >= SIZE) index -= SIZE;

        length += len; // (before we destroy len)
        
        unsigned max = SIZE - index;
        std::memcpy(buf + index, s, std::min(max, len));
        if (len > max) {
            // Wrapped around buffer: copy the rest
            s += max;
            len -= max;
            std::memcpy(buf, s, len);
        }
    }

    // reset the index and length.
    void reset()
    {
        cur_idx = 0;
        length = 0;
    }
};

#endif
