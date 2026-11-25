/* Live, side-effect-free readiness handshake for the stock WMT ROM service. */
#ifndef T70_WMT_READY_H
#define T70_WMT_READY_H

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define T70_WMT_READY_NAME "teclast-t70pro-wmt-control"
#define T70_WMT_READY_REQUEST "PING"
#define T70_WMT_READY_REPLY "READY"
#define T70_WMT_READY_TIMEOUT_MS 250

static inline uid_t t70_wmt_expected_uid(void) {
#if defined(WMT_CONTROL_TEST) || defined(BT_BRIDGE_TEST)
    return getuid();
#else
    return 0;
#endif
}

static inline int t70_wmt_ready_address(struct sockaddr_un *address, socklen_t *length) {
    const size_t name_length = sizeof(T70_WMT_READY_NAME) - 1;
    if (name_length + 1 > sizeof(address->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    address->sun_path[0] = '\0';
    memcpy(address->sun_path + 1, T70_WMT_READY_NAME, name_length);
    *length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_length);
    return 0;
}

static inline int t70_wmt_ready_server(void) {
    struct sockaddr_un address;
    socklen_t length;
    if (t70_wmt_ready_address(&address, &length)) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&address, length) || listen(fd, 4)) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}

static inline int64_t t70_wmt_ready_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static inline int t70_wmt_ready_wait(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = t70_wmt_ready_now_ms();
        if (now < 0) return -1;
        int64_t remaining = deadline - now;
        if (remaining <= 0) { errno = ETIMEDOUT; return 0; }
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd p = {fd, events, 0};
        int ready = poll(&p, 1, timeout);
        if (ready > 0) {
            if (p.revents & events) return 1;
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENOTCONN; return -1; }
            continue;
        }
        if (!ready) { errno = ETIMEDOUT; return 0; }
        if (errno != EINTR) return -1;
    }
}

static inline ssize_t t70_wmt_ready_io(int fd, void *buffer, size_t length,
                                       int writing, int64_t deadline) {
    for (;;) {
        int64_t now = t70_wmt_ready_now_ms();
        if (now < 0) return -1;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        ssize_t count = writing
            ? send(fd, buffer, length, MSG_NOSIGNAL)
            : recv(fd, buffer, length, MSG_TRUNC);
        if (count >= 0) {
            if (!writing && !count) return 0;
            if ((size_t)count != length) { errno = EIO; return -1; }
            return count;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        int ready = t70_wmt_ready_wait(fd, writing ? POLLOUT : POLLIN, deadline);
        if (ready != 1) return -1;
    }
}

static inline int t70_wmt_ready_reply(int fd) {
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) ||
        peer_size != sizeof(peer) || peer.uid != t70_wmt_expected_uid()) {
        close(fd); return 0;
    }
    int64_t now = t70_wmt_ready_now_ms();
    if (now < 0 || t70_wmt_ready_wait(fd, POLLIN, now + 100) != 1) { close(fd); return 0; }
    char request[sizeof(T70_WMT_READY_REQUEST)];
    ssize_t count = t70_wmt_ready_io(fd, request, sizeof(T70_WMT_READY_REQUEST) - 1,
                                     0, now + 100);
    if (count == (ssize_t)(sizeof(T70_WMT_READY_REQUEST) - 1) &&
        !memcmp(request, T70_WMT_READY_REQUEST, sizeof(T70_WMT_READY_REQUEST) - 1)) {
        (void)t70_wmt_ready_io(fd, (void *)T70_WMT_READY_REPLY,
                               sizeof(T70_WMT_READY_REPLY) - 1, 1, now + 100);
        /* Diagnostic clients may disconnect at any point; never stop WMT for that. */
    }
    close(fd);
    return 0;
}

static inline int t70_wmt_ready_accept(int server) {
    /* Return to serve() after at most one handshake. Its poll checks the
     * driver command channel first, even when other probes are queued. */
    int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd >= 0) return t70_wmt_ready_reply(fd);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
        errno == ECONNABORTED) return 0;
    return -1;
}

static inline int t70_wmt_ready(void) {
    struct sockaddr_un address;
    socklen_t length;
    if (t70_wmt_ready_address(&address, &length)) return -1;
    int last_error = ENOTCONN;
    int64_t start = t70_wmt_ready_now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + T70_WMT_READY_TIMEOUT_MS;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int64_t now = t70_wmt_ready_now_ms();
        if (now < 0) return -1;
        if (now >= deadline) break;
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        int connected = 0;
        if (!connect(fd, (struct sockaddr *)&address, length)) connected = 1;
        else if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR) {
            int ready = t70_wmt_ready_wait(fd, POLLOUT, deadline);
            if (ready > 0) {
                int error = 0; socklen_t error_size = sizeof(error);
                if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) && !error) connected = 1;
                else last_error = error ? error : errno;
            } else last_error = errno;
        } else last_error = errno;
        if (connected) {
            struct ucred peer;
            socklen_t peer_size = sizeof(peer);
            if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) &&
                peer_size == sizeof(peer) && peer.uid == t70_wmt_expected_uid()) {
                ssize_t sent = t70_wmt_ready_io(fd, (void *)T70_WMT_READY_REQUEST,
                                                sizeof(T70_WMT_READY_REQUEST) - 1, 1, deadline);
                if (sent == (ssize_t)(sizeof(T70_WMT_READY_REQUEST) - 1)) {
                    int ready = t70_wmt_ready_wait(fd, POLLIN, deadline);
                    if (ready > 0) {
                        char reply[sizeof(T70_WMT_READY_REPLY)];
                        ssize_t count = t70_wmt_ready_io(fd, reply,
                            sizeof(T70_WMT_READY_REPLY) - 1, 0, deadline);
                        if (count == (ssize_t)(sizeof(T70_WMT_READY_REPLY) - 1) &&
                            !memcmp(reply, T70_WMT_READY_REPLY, sizeof(T70_WMT_READY_REPLY) - 1)) {
                            close(fd); return 0;
                        }
                    }
                    last_error = errno ? errno : ENOTCONN;
                } else last_error = sent < 0 ? errno : EIO;
            } else last_error = EPERM;
        }
        close(fd);
        now = t70_wmt_ready_now_ms();
        if (now < 0) return -1;
        if (now < deadline) {
            struct pollfd pause = {-1, 0, 0};
            int delay = deadline - now < 10 ? (int)(deadline - now) : 10;
            while (poll(&pause, 0, delay) < 0 && errno == EINTR) {
                now = t70_wmt_ready_now_ms();
                if (now < 0 || now >= deadline) break;
            }
        }
    }
    int64_t end = t70_wmt_ready_now_ms();
    if (end >= deadline) last_error = ETIMEDOUT;
    errno = last_error;
    return -1;
}

#endif
