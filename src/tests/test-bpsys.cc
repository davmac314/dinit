#include <vector>
#include <utility>
#include <algorithm>
#include <map>

#include <cstdlib>
#include <cerrno>

#include "baseproc-sys.h"

namespace {

std::vector<bool> usedfds = {true, true, true};

struct read_result
{
	read_result(int errcode_p) : errcode(errcode_p) {}

	read_result(std::vector<char> &data_p) : errcode(0), data(data_p) {}
	read_result(std::vector<char> &&data_p) : errcode(0), data(std::move(data_p)) {}

	int errcode; // errno return
	std::vector<char> data;  // data (if errcode == 0)
};

// map of fd to read results to supply for reads of that fd
std::map<int,std::vector<read_result>> read_data;

// map of data written to each fd
std::map<int,std::vector<char>> written_data;

} // anon namespace

namespace bp_sys {

int last_sig_sent = -1; // last signal number sent, accessible for tests.
pid_t last_forked_pid = 1;  // last forked process id (incremented each 'fork')

// Test helper methods:

// Allocate a file descriptor
int allocfd()
{
    auto f = std::find(usedfds.begin(), usedfds.end(), false);
    if (f == usedfds.end()) {
        int r = usedfds.size();
        usedfds.push_back(true);
        return r;
    }

    *f = true;
    return f - usedfds.begin();
}

// Supply data to be returned by read()
void supply_read_data(int fd, std::vector<char> &data)
{
	read_data[fd].emplace_back(data);
}

void supply_read_data(int fd, std::vector<char> &&data)
{
	read_data[fd].emplace_back(std::move(data));
}

// retrieve data written via write()
void extract_written_data(int fd, std::vector<char> &data)
{
	data = std::move(written_data[fd]);
}


// Mock implementations of system calls:

int pipe2(int fds[2], int flags)
{
    fds[0] = allocfd();
    fds[1] = allocfd();
    return 0;
}

int close(int fd)
{
    if (size_t(fd) >= usedfds.size()) abort();

    usedfds[fd] = false;
    return 0;
}

int kill(pid_t pid, int sig)
{
    last_sig_sent = sig;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
	std::vector<read_result> rrs = read_data[fd];
	if (rrs.empty()) {
		return 0;
	}

	read_result &rr = rrs.front();
	if (rr.errcode != 0) {
		errno = rr.errcode;
		// Remove the result record:
		auto i = rrs.begin();
		i++;
		rrs.erase(rrs.begin(), i);
		return -1;
	}

	auto dsize = rr.data.size();
	if (dsize <= count) {
		// Consume entire result:
		std::copy_n(rr.data.begin(), dsize, (char *)buf);
		// Remove the result record:
		auto i = rrs.begin();
		i++;
		rrs.erase(rrs.begin(), i);
		return dsize;
	}

	// Consume partial result:
	std::copy_n(rr.data.begin(), count, (char *)buf);
	rr.data.erase(rr.data.begin(), rr.data.begin() + count);
	return count;
}

ssize_t write(int fd, const void *buf, size_t count)
{
	std::vector<char> &wd = written_data[fd];
	wd.insert(wd.end(), (char *)buf, (char *)buf + count);
	return count;
}

}
