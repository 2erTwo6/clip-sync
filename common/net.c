#define _GNU_SOURCE
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint64_t net_get_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

void net_put_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

uint64_t net_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int set_nonblock(int fd, int nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (nb) fl |= O_NONBLOCK;
    else fl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

int net_set_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;

    /* Also let the kernel eventually notice a dead peer if the app-level
       heartbeat is somehow delayed. Failure is non-fatal. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 30, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    return 0;
}

int net_connect_tcp(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints;
    struct addrinfo *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host ? host : "?", port, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (set_nonblock(fd, 1) < 0) { close(fd); fd = -1; continue; }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            set_nonblock(fd, 0);
            break;
        }
        if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0) { close(fd); fd = -1; continue; }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd); fd = -1; continue;
        }
        set_nonblock(fd, 0);
        break;
    }
    freeaddrinfo(res);
    return fd;
}

int net_listen_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host ? host : "*", port, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 8) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int net_accept(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int net_send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int net_recv_all(int fd, void *data, size_t len) {
    uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int net_send_msg(int fd, uint32_t type, const void *payload, uint32_t len) {
    if (len > CLIPSYNC_MAX_PAYLOAD) return -1;
    uint8_t hdr[12];
    put_be32(hdr, CLIPSYNC_MAGIC);
    put_be32(hdr + 4, type);
    put_be32(hdr + 8, len);
    if (net_send_all(fd, hdr, sizeof(hdr)) < 0) return -1;
    if (len && net_send_all(fd, payload, len) < 0) return -1;
    return 0;
}

int net_recv_msg(int fd, uint32_t *type, uint8_t **payload, uint32_t *len) {
    uint8_t hdr[12];
    if (net_recv_all(fd, hdr, sizeof(hdr)) < 0) return -1;
    if (get_be32(hdr) != CLIPSYNC_MAGIC) return -2;
    uint32_t t = get_be32(hdr + 4);
    uint32_t l = get_be32(hdr + 8);
    if (l > CLIPSYNC_MAX_PAYLOAD) return -2;
    uint8_t *p = NULL;
    if (l) {
        p = malloc(l);
        if (!p) return -3;
        if (net_recv_all(fd, p, l) < 0) { free(p); return -1; }
    }
    *type = t;
    *payload = p;
    *len = l;
    return 0;
}
