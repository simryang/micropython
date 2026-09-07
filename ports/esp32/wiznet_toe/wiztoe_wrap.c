/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linker --wrap glue: routes lwIP's BSD socket entry points to the WIZnet TOE
 * hardware-socket backend (wiznet_toe.c).
 *
 * ESP-IDF exposes socket()/recv()/... as static-inline wrappers around
 * lwip_socket()/lwip_recv()/... (LWIP_COMPAT_SOCKETS=0, confirmed in
 * components/lwip/port/include/lwipopts.h). We intercept those symbols with
 * `-Wl,--wrap=lwip_*` (see esp32_common.cmake), so MicroPython's own socket
 * module is unchanged. close() on a socket fd routes through the VFS to
 * lwip_close, which is likewise redirected here -- so close() re-arms the TOE
 * listener as expected.
 *
 * fd mapping: TOE fds come from our own VFS driver (toe_vfs.c), NOT from
 * `sn + LWIP_SOCKET_OFFSET` as in the wsm_driver original. That original
 * scheme put TOE fds inside lwIP's reserved fd window, which forced the
 * "TOE owns every socket" assumption and broke select(). With a separate fd
 * space every wrap can dispatch on the fd: ours -> wiztoe_*, anything else
 * -> __real_lwip_*, so real lwIP sockets (WiFi, mDNS, ...) keep working.
 *
 * Includes lwIP headers but NOT ioLibrary — no socket()/close() name clash.
 *
 * Derived from wsm_driver
 * (D:\esp32s3-lab\wsm_driver\port\ioLibrary_Driver\src\wiztoe_wrap.c).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

#include "lwip/sockets.h"     /* struct sockaddr_in, lwip_htons/htonl */

#include <unistd.h>           /* close() -- see __wrap_lwip_close */
/* LWIP_POSIX_SOCKETS_IO_NAMES==0 in this build, so lwip/sockets.h does NOT
 * macro-define close() to lwip_close(). Undef defensively anyway: if it ever
 * did, close(s) below would recurse into this wrapper. */
#undef close

#include "toe_dns.h"
#include "toe_vfs.h"
#include "wiznet_toe.h"

#include <netdb.h>
#include <stdlib.h>

/* Bring-up tracing; set to 0 once the socket path is proven. */
#define WIZTOE_WRAP_TRACE 0
#if WIZTOE_WRAP_TRACE
#define WRAP_TRACE(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define WRAP_TRACE(...) do { } while (0)
#endif

/* The untouched lwIP entry points, for fds that are not ours. */
int __real_lwip_socket(int domain, int type, int protocol);
int __real_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
int __real_lwip_listen(int s, int backlog);
int __real_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int __real_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);
ssize_t __real_lwip_send(int s, const void *data, size_t size, int flags);
ssize_t __real_lwip_recv(int s, void *mem, size_t len, int flags);
ssize_t __real_lwip_recvfrom(int s, void *mem, size_t len, int flags,
                             struct sockaddr *from, socklen_t *fromlen);
ssize_t __real_lwip_sendto(int s, const void *data, size_t size, int flags,
                           const struct sockaddr *to, socklen_t tolen);
ssize_t __real_lwip_write(int s, const void *data, size_t size);
ssize_t __real_lwip_read(int s, void *mem, size_t len);
int __real_lwip_fcntl(int s, int cmd, int val);
int __real_lwip_close(int s);
int __real_lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int __real_lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int __real_lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);
int __real_lwip_getaddrinfo(const char *nodename, const char *servname,
                            const struct addrinfo *hints, struct addrinfo **res);
void __real_lwip_freeaddrinfo(struct addrinfo *ai);

static void toe_fill_sockaddr(struct sockaddr *addr, socklen_t *addrlen,
                              const uint8_t ip[4], uint16_t port)
{
    if (addr == NULL || addrlen == NULL || *addrlen < (socklen_t)sizeof(struct sockaddr_in))
        return;
    struct sockaddr_in *sin = (struct sockaddr_in *)(void *)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = lwip_htons(port);
    sin->sin_addr.s_addr = lwip_htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                                      ((uint32_t)ip[2] << 8) | ip[3]);
    *addrlen = sizeof(struct sockaddr_in);
}

static void toe_ip_from_sockaddr(const struct sockaddr *name, uint8_t ip[4], uint16_t *port)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    uint32_t a = lwip_ntohl(sin->sin_addr.s_addr);
    ip[0] = (uint8_t)(a >> 24); ip[1] = (uint8_t)(a >> 16);
    ip[2] = (uint8_t)(a >> 8);  ip[3] = (uint8_t)a;
    *port = lwip_ntohs(sin->sin_port);
}

/* New sockets go to the TOE only once net_init() has brought the chip up.
 * Before that (and in a firmware that never calls it) socket() behaves
 * exactly as stock MicroPython. */
int __wrap_lwip_socket(int domain, int type, int protocol)
{
    if (!toe_vfs_claims_new_sockets()) {
        return __real_lwip_socket(domain, type, protocol);
    }
    int sn = wiztoe_socket(domain, type, protocol);
    if (sn < 0) { errno = ENFILE; return -1; }
    int fd = toe_vfs_alloc_fd(sn);
    if (fd < 0) { wiztoe_close(sn); errno = ENFILE; return -1; }
    WRAP_TRACE("wrap: socket(dom=%d type=%d proto=%d) -> sn=%d fd=%d\n",
        domain, type, protocol, sn, fd);
    errno = 0;
    return fd;
}

int __wrap_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_bind(s, name, namelen);
    }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    int r = wiztoe_bind(sn, lwip_ntohs(sin->sin_port));
    WRAP_TRACE("wrap: bind(fd=%d sn=%d port=%u) -> %d\n", s, sn, lwip_ntohs(sin->sin_port), r);
    if (r < 0) { errno = EADDRINUSE; return -1; }
    errno = 0;
    return 0;
}

int __wrap_lwip_listen(int s, int backlog)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_listen(s, backlog);
    }
    int r = wiztoe_listen(sn, backlog);
    WRAP_TRACE("wrap: listen(fd=%d sn=%d) -> %d\n", s, sn, r);
    if (r < 0) { errno = EOPNOTSUPP; return -1; }
    errno = 0;
    return 0;
}

int __wrap_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_accept(s, addr, addrlen);
    }
    int acc = wiztoe_accept(sn);
    if (acc == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (acc < 0) { errno = EINVAL; return -1; }
    uint8_t ip[4]; uint16_t port;
    wiztoe_peer(acc, ip, &port);
    toe_fill_sockaddr(addr, addrlen, ip, port);
    /* In the TOE model the listener itself becomes the connection, so the
     * accepted socket is the same hardware socket -- and therefore the same
     * fd. Returning the existing fd keeps the mapping 1:1. */
    int acc_fd = (acc == sn) ? s : toe_vfs_alloc_fd(acc);
    if (acc_fd < 0) { errno = ENFILE; return -1; }
    WRAP_TRACE("wrap: accept(fd=%d sn=%d) -> fd=%d\n", s, sn, acc_fd);
    errno = 0;
    return acc_fd;
}

int __wrap_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_connect(s, name, namelen);
    }
    uint8_t ip[4]; uint16_t port;
    toe_ip_from_sockaddr(name, ip, &port);
    if (wiztoe_connect(sn, ip, port) < 0) { errno = ECONNREFUSED; return -1; }
    errno = 0;
    return 0;
}

ssize_t __wrap_lwip_send(int s, const void *data, size_t size, int flags)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_send(s, data, size, flags);
    }
    int n = wiztoe_send(sn, data, size);
    if (n == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

/* MicroPython's modsocket.c sends through lwip_write() (via _socket_send /
 * socket_stream_write), not lwip_send(), and reads through lwip_recvfrom().
 * wsm_driver's original wrap list omitted write/read/fcntl because its own
 * example code called send()/recv() directly -- without these three, send()
 * falls through to the real lwIP write() on an fd that is not a real lwIP
 * socket and fails with EBADF. */
ssize_t __wrap_lwip_write(int s, const void *data, size_t size)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_write(s, data, size);
    }
    int n = wiztoe_send(sn, data, size);
    if (n == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_read(int s, void *mem, size_t len)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_read(s, mem, len);
    }
    int n = wiztoe_recv(sn, mem, len);
    if (n == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

/* TOE sockets are always blocking (the chip has no non-blocking mode and
 * wiztoe_* emulates timeouts by polling). Accept F_GETFL/F_SETFL so callers
 * that set O_NONBLOCK defensively -- MicroPython's _socket_settimeout does --
 * don't fail; the flag simply has no effect. */
int __wrap_lwip_fcntl(int s, int cmd, int val)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_fcntl(s, cmd, val);
    }
    /* O_NONBLOCK must be honoured, not ignored: MicroPython implements
     * settimeout(0) as fcntl(O_NONBLOCK) with retries=0 and expects the very
     * first recv/send to come back EWOULDBLOCK. Ignoring the flag left that
     * recv in the blocking wait -- with RCVTIMEO also 0, forever. */
    if (cmd == F_SETFL) {
        wiztoe_set_nonblock(sn, (val & O_NONBLOCK) != 0);
        errno = 0;
        return 0;
    }
    if (cmd == F_GETFL) {
        errno = 0;
        return wiztoe_get_nonblock(sn) ? O_NONBLOCK : 0;
    }
    errno = ENOSYS;
    return -1;
}

ssize_t __wrap_lwip_recv(int s, void *mem, size_t len, int flags)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_recv(s, mem, len, flags);
    }
    int n = wiztoe_recv(sn, mem, len);
    if (n == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_recvfrom(int s, void *mem, size_t len, int flags,
                             struct sockaddr *from, socklen_t *fromlen)
{
    (void)flags;
    int toe_fd = toe_vfs_sn_from_fd(s);
    if (toe_fd < 0) {
        return __real_lwip_recvfrom(s, mem, len, flags, from, fromlen);
    }
    uint8_t ip[4]; uint16_t port = 0;
    int n;
    if (wiztoe_is_udp(toe_fd)) {
        n = wiztoe_recvfrom(toe_fd, mem, len, ip, &port);
    } else {
        n = wiztoe_recv(toe_fd, mem, len);
        wiztoe_peer(toe_fd, ip, &port);
    }
    if (n == WIZTOE_ERR_TIMEOUT) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    toe_fill_sockaddr(from, fromlen, ip, port);
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_sendto(int s, const void *data, size_t size, int flags,
                           const struct sockaddr *to, socklen_t tolen)
{
    (void)flags;
    int toe_fd = toe_vfs_sn_from_fd(s);
    if (toe_fd < 0) {
        return __real_lwip_sendto(s, data, size, flags, to, tolen);
    }
    (void)tolen;
    int n;
    if (to == NULL || !wiztoe_is_udp(toe_fd)) {
        n = wiztoe_send(toe_fd, data, size);
    } else {
        uint8_t ip[4]; uint16_t port;
        toe_ip_from_sockaddr(to, ip, &port);
        n = wiztoe_sendto(toe_fd, data, size, ip, port);
    }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

int __wrap_lwip_close(int s)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_close(s);
    }
    /* Closing an accepted connection RE-ARMS the listener on the same hardware
     * socket and the same fd (TOE model), so the fd must stay registered. Do
     * that directly and keep the fd -- routing it through close() would let
     * esp_vfs_close free the fd-table entry the re-armed listener still needs. */
    if (wiztoe_is_rearming_listener(sn)) {
        int r = wiztoe_close(sn);
        if (r < 0) { errno = EBADF; return -1; }
        errno = 0;
        return 0;
    }
    /* Normal close: route through the close() syscall so esp_vfs_close frees
     * the fd-table entry. Our fds are registered permanent=false with a local
     * fd, and such a registration can ONLY be freed by close() -- the previous
     * esp_vfs_unregister_fd path frees permanent==true entries only, so it
     * silently leaked one fd per socket and exhausted the table after ~50
     * sockets (OSError 23, measured 2026-09-03). close() also drives our .close
     * op (toe_vfs_close -> wiztoe_close), so the hardware socket is released
     * too. */
    return close(s);
}

int __wrap_lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        return __real_lwip_getsockname(s, name, namelen);
    }
    uint8_t ip[4]; uint16_t port;
    wiztoe_getsockname(sn, ip, &port);
    toe_fill_sockaddr(name, namelen, ip, port);
    errno = 0;
    return 0;
}

int __wrap_lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    int toe_fd = toe_vfs_sn_from_fd(s);
    if (toe_fd < 0) {
        return __real_lwip_setsockopt(s, level, optname, optval, optlen);
    }
    WRAP_TRACE("wrap: setsockopt(fd=%d sn=%d level=%d optname=%d)\n", s, toe_fd, level, optname);
    if (optval == NULL) { errno = EFAULT; return -1; }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
        case SO_BROADCAST:
            errno = 0; return 0;                     /* harmless no-op */
        case SO_BINDTODEVICE:
            /* Pinning a socket to a netif is meaningless here: the chip IS the
             * interface, so every TOE socket is already bound to it. */
            errno = 0; return 0;
        case SO_KEEPALIVE:
            if (wiztoe_setsockopt(toe_fd, WIZTOE_OPT_KEEPALIVE, optval, optlen) < 0) {
                errno = EINVAL; return -1;
            }
            errno = 0; return 0;
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            const struct timeval *tv = (const struct timeval *)optval;
            uint32_t ms;
            wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS
                                                      : WIZTOE_OPT_SNDTIMEO_MS;
            if (optlen < (socklen_t)sizeof(struct timeval)) { errno = EINVAL; return -1; }
            ms = (uint32_t)((tv->tv_sec * 1000) + (tv->tv_usec / 1000));
            if (wiztoe_setsockopt(toe_fd, o, &ms, sizeof(ms)) < 0) { errno = EINVAL; return -1; }
            errno = 0; return 0;
        }
        default: break;
        }
    } else if (level == IPPROTO_TCP) {
        wiztoe_opt_t o;
        if (optname == TCP_NODELAY)       o = WIZTOE_OPT_NODELAY;
        else if (optname == TCP_KEEPIDLE) o = WIZTOE_OPT_KEEPIDLE;
        else { errno = ENOPROTOOPT; return -1; }
        if (wiztoe_setsockopt(toe_fd, o, optval, optlen) < 0) { errno = EINVAL; return -1; }
        errno = 0; return 0;
    } else if (level == IPPROTO_IP) {
        wiztoe_opt_t o;
        if (optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP) {
            /* Not mapped on purpose -- see wiznet_toe.h comment on multicast. */
            errno = ENOPROTOOPT;
            return -1;
        }
        if (optname == IP_MULTICAST_TTL || optname == IP_MULTICAST_IF ||
            optname == IP_MULTICAST_LOOP) {
            errno = 0; return 0;
        }
        if (optname == IP_TTL)      o = WIZTOE_OPT_TTL;
        else if (optname == IP_TOS) o = WIZTOE_OPT_TOS;
        else { errno = ENOPROTOOPT; return -1; }
        if (wiztoe_setsockopt(toe_fd, o, optval, optlen) < 0) { errno = EINVAL; return -1; }
        errno = 0; return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int __wrap_lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    int toe_fd = toe_vfs_sn_from_fd(s);
    if (toe_fd < 0) {
        return __real_lwip_getsockopt(s, level, optname, optval, optlen);
    }
    if (optval == NULL || optlen == NULL) { errno = EFAULT; return -1; }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ERROR:
        case SO_TYPE:
        case SO_RCVBUF:
        case SO_SNDBUF: {
            wiztoe_opt_t o = (optname == SO_ERROR)  ? WIZTOE_OPT_ERROR
                           : (optname == SO_TYPE)   ? WIZTOE_OPT_TYPE
                           : (optname == SO_RCVBUF) ? WIZTOE_OPT_RCVBUF
                                                    : WIZTOE_OPT_SNDBUF;
            size_t sz = (size_t)*optlen;
            if (*optlen < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, optval, &sz) < 0) { errno = EINVAL; return -1; }
            *optlen = (socklen_t)sz;
            errno = 0; return 0;
        }
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            uint32_t ms = 0; size_t sz = sizeof(ms);
            struct timeval *tv;
            wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS
                                                      : WIZTOE_OPT_SNDTIMEO_MS;
            if (*optlen < (socklen_t)sizeof(struct timeval)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, &ms, &sz) < 0) { errno = EINVAL; return -1; }
            tv = (struct timeval *)optval;
            tv->tv_sec = (long)(ms / 1000);
            tv->tv_usec = (long)((ms % 1000) * 1000);
            *optlen = sizeof(struct timeval);
            errno = 0; return 0;
        }
        default: break;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == IP_TTL || optname == IP_TOS) {
            wiztoe_opt_t o = (optname == IP_TTL) ? WIZTOE_OPT_TTL : WIZTOE_OPT_TOS;
            size_t sz = (size_t)*optlen;
            if (*optlen < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, optval, &sz) < 0) { errno = EINVAL; return -1; }
            *optlen = (socklen_t)sz;
            errno = 0; return 0;
        }
    }
    errno = ENOPROTOOPT;
    return -1;
}

/* ------------------------------------------------------------------ DNS
 *
 * getaddrinfo() is NOT reachable through the socket wraps: MicroPython calls
 * lwip_getaddrinfo() directly, and lwIP resolves through its own stack, which
 * has no route on TOE. That is why socket.getaddrinfo() returned -202 before
 * this: the query went to lwIP and simply never got an answer.
 *
 * Wrapping it here, backed by ioLibrary's DNS_run() on a borrowed hardware
 * socket, makes socket.getaddrinfo() -- and therefore the stock HTTP examples
 * -- work unchanged on TOE.
 *
 * The results we hand out must be freed by us, while results that came from
 * __real_lwip_getaddrinfo must go back to lwIP. A tiny registry of the
 * pointers we allocated keeps the two apart; a magic value read from before
 * the returned pointer would be shorter but is out-of-bounds access on an
 * allocation we did not make.
 */

#define TOE_AI_MAX 4          /* getaddrinfo results are short-lived here */
#define TOE_AI_NAME_MAX 128

typedef struct {
    struct addrinfo ai;
    struct sockaddr_in sa;
    char canonname[TOE_AI_NAME_MAX];
} toe_addrinfo_t;

static toe_addrinfo_t *s_toe_ai[TOE_AI_MAX];

static bool toe_ai_track(toe_addrinfo_t *p) {
    for (int i = 0; i < TOE_AI_MAX; i++) {
        if (s_toe_ai[i] == NULL) {
            s_toe_ai[i] = p;
            return true;
        }
    }
    return false;
}

static bool toe_ai_untrack(const struct addrinfo *ai) {
    for (int i = 0; i < TOE_AI_MAX; i++) {
        if (s_toe_ai[i] != NULL && &s_toe_ai[i]->ai == ai) {
            free(s_toe_ai[i]);
            s_toe_ai[i] = NULL;
            return true;
        }
    }
    return false;
}

/* "1.2.3.4" -> 4 bytes. Returns false if not a plain dotted quad. */
static bool toe_parse_dotted_quad(const char *s, uint8_t out[4]) {
    int part = 0;
    unsigned val = 0;
    bool digit = false;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned)(*p - '0');
            if (val > 255) {
                return false;
            }
            digit = true;
        } else if (*p == '.' || *p == '\0') {
            if (!digit || part >= 4) {
                return false;
            }
            out[part++] = (uint8_t)val;
            val = 0;
            digit = false;
            if (*p == '\0') {
                break;
            }
        } else {
            return false;
        }
    }
    return part == 4;
}

int __wrap_lwip_getaddrinfo(const char *nodename, const char *servname,
                            const struct addrinfo *hints, struct addrinfo **res) {
    if (!toe_vfs_claims_new_sockets() || nodename == NULL || res == NULL) {
        return __real_lwip_getaddrinfo(nodename, servname, hints, res);
    }

    uint8_t ip[4];
    if (!toe_parse_dotted_quad(nodename, ip)) {
        uint8_t server[4];
        if (!toe_dns_server(server)) {
            return EAI_FAIL;
        }
        if (!toe_dns_resolve(server, nodename, ip)) {
            return EAI_FAIL;
        }
    }

    toe_addrinfo_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return EAI_MEMORY;
    }
    if (!toe_ai_track(p)) {
        free(p);
        return EAI_MEMORY;
    }

    unsigned port = 0;
    if (servname != NULL) {
        for (const char *s = servname; *s >= '0' && *s <= '9'; s++) {
            port = port * 10 + (unsigned)(*s - '0');
        }
    }

    p->sa.sin_family = AF_INET;
    p->sa.sin_len = sizeof(p->sa);
    p->sa.sin_port = lwip_htons((uint16_t)port);
    p->sa.sin_addr.s_addr = lwip_htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                                       ((uint32_t)ip[2] << 8) | ip[3]);

    /* MicroPython dereferences ai_canonname unconditionally (modsocket.c
     * compares it against "0.0.0.0"), so it must never be NULL. */
    size_t nl = strlen(nodename);
    if (nl >= sizeof(p->canonname)) {
        nl = sizeof(p->canonname) - 1;
    }
    memcpy(p->canonname, nodename, nl);
    p->canonname[nl] = '\0';

    p->ai.ai_family = AF_INET;
    p->ai.ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
    p->ai.ai_protocol = (hints && hints->ai_protocol) ? hints->ai_protocol : 0;
    p->ai.ai_addrlen = sizeof(p->sa);
    p->ai.ai_addr = (struct sockaddr *)&p->sa;
    p->ai.ai_canonname = p->canonname;
    p->ai.ai_next = NULL;

    *res = &p->ai;
    return 0;
}

void __wrap_lwip_freeaddrinfo(struct addrinfo *ai) {
    if (ai == NULL) {
        return;
    }
    if (toe_ai_untrack(ai)) {
        return;      /* one of ours */
    }
    __real_lwip_freeaddrinfo(ai);
}
