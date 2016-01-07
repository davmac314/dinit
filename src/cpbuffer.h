#ifndef CPBUFFER_H
#define CPBUFFER_H

#include <cstring>

// control protocol buffer, a circular buffer with 1024-byte capacity.
class CPBuffer
{
    char buf[1024];
    int cur_idx = 0;
    int length = 0;  // number of elements in the buffer
    
    public:
    int get_length() noexcept
    {
        return length;
    }
    
    // fill by reading from the given fd, return positive if some was read or -1 on error.
    int fill(int fd) noexcept
    {
        int pos = cur_idx + length;
        if (pos >= 1024) pos -= 1024;
        int max_count = std::min(1024 - pos, 1024 - length);
        ssize_t r = read(fd, buf + pos, max_count);
        if (r >= 0) {
            length += r;
        }
        return r;
    }
    
    // fill by readin from the given fd, until at least the specified number of bytes are in
    // the buffer. Return 0 if end-of-file reached before fill complete, or -1 on error.
    int fillTo(int fd, int rlength) noexcept
    {
        while (length < rlength) {
            int r = fill(fd);
            if (r <= 0) return r;
        }
        return 1;
    }
    
    int operator[](int idx) noexcept
    {
        int dest_idx = cur_idx + idx;
        if (dest_idx > 1024) dest_idx -= 1024;
        return buf[dest_idx];
    }
    
    void consume(int amount) noexcept
    {
        cur_idx += amount;
        if (cur_idx >= 1024) cur_idx -= 1024;
        length -= amount;
    }
    
    void extract(char *dest, int index, int length) noexcept
    {
        index += cur_idx;
        if (index >= 1024) index -= 1024;
        if (index + length > 1024) {
            // wrap-around copy
            int half = 1024 - index;
            std::memcpy(dest, buf + index, half);
            std::memcpy(dest + half, buf, length - half);
        }
        else {
            std::memcpy(dest, buf + index, length);
        }
    }
    
    // Extract string of give length from given index
    // Throws:  std::bad_alloc on allocation failure
    std::string extract_string(int index, int length)
    {
        index += cur_idx;
        if (index >= 1024) index -= 1024;
        if (index + length > 1024) {
            std::string r(buf + index, 1024 - index);
            r.insert(r.end(), buf, buf + length - (1024 - index));
            return r;
        }
        else {
            return std::string(buf + index, length);
        }
    }
};

#endif
