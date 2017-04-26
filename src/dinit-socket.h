#ifndef _DINIT_SOCKET_H_INCLUDED
#define _DINIT_SOCKET_H_INCLUDED

#include <sys/socket.h>
#include <fcntl.h>

namespace {
#if !defined(SOCK_NONBLOCK) && !defined(SOCK_CLOEXEC)
    // make our own accept4 on systems that don't have it:
    constexpr int SOCK_NONBLOCK = 1;
    constexpr int SOCK_CLOEXEC = 2;
    inline int dinit_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    {
        int fd = accept(sockfd, addr, addrlen);
        if (fd == -1) {
            return -1;
        }

        if (flags & SOCK_CLOEXEC)  fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (flags & SOCK_NONBLOCK) fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
    }

    inline int dinit_socket(int domain, int type, int protocol, int flags)
    {
        int fd = socket(domain, type, protocol);
        if (fd == -1) {
            return -1;
        }

        if (flags & SOCK_CLOEXEC)  fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (flags & SOCK_NONBLOCK) fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
    }

    inline int dinit_socketpair(int domain, int type, int protocol, int socket_vector[2], int flags)
    {
        int r = socketpair(domain, type, protocol, socket_vector);
        if (r == -1) {
            return -1;
        }

        if (flags & SOCK_CLOEXEC) {
            fcntl(socket_vector[0], F_SETFD, FD_CLOEXEC);
            fcntl(socket_vector[1], F_SETFD, FD_CLOEXEC);
        }
        if (flags & SOCK_NONBLOCK) {
            fcntl(socket_vector[0], F_SETFL, O_NONBLOCK);
            fcntl(socket_vector[1], F_SETFL, O_NONBLOCK);
        }
        return 0;
    }

#else
    inline int dinit_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    {
        return accept4(sockfd, addr, addrlen, flags);
    }

    inline int dinit_socket(int domain, int type, int protocol, int flags)
    {
        return socket(domain, type | flags, protocol);
    }

    inline int dinit_socketpair(int domain, int type, int protocol, int socket_vector[2], int flags)
    {
        return socketpair(domain, type | flags, protocol, socket_vector);
    }
#endif
}

#endif
