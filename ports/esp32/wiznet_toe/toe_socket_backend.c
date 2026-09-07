/*
 * W5500 hardware sockets as a socket backend.
 *
 * Each function mirrors the lwIP/POSIX call in socket_backend_t: it takes a
 * descriptor that toe_vfs.c registered with the ESP-IDF VFS and maps it to a
 * W5500 hardware socket (wiznet_toe.c).  Only sockets created while this
 * backend was the default arrive here, so a descriptor that is not ours is a
 * caller bug and gets EBADF.
 *
 * Listeners are the one place the chip's model and BSD's differ: the chip has
 * no backlog, the listening hardware socket itself becomes the connection it
 * accepts.  toe_backend_accept() papers over that so a program sees the usual
 * shape -- accept() returns a new descriptor for the connection and the
 * listener keeps its own, moved onto a fresh hardware socket (or dormant until
 * one is free; see toe_vfs.h).
 *
 * Adapted from wiztoe_wrap.c, which reached the same code through
 * -Wl,--wrap=lwip_* and therefore had to dispatch on the descriptor and fall
 * back to lwIP for anything that was not ours.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>       // close() -- see toe_backend_close

#include "lwip/sockets.h" // struct sockaddr_in, lwip_htons/htonl

#include "toe_port.h"     // toe_yield_1ms(), toe_time_us()
#include "toe_socket_backend.h"
#include "toe_vfs.h"
#include "wiznet_toe.h"

static void toe_fill_sockaddr(struct sockaddr *addr, socklen_t *addrlen, const uint8_t ip[4], uint16_t port) {
    if (addr == NULL || addrlen == NULL || *addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        return;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)(void *)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = lwip_htons(port);
    sin->sin_addr.s_addr = lwip_htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3]);
    *addrlen = sizeof(struct sockaddr_in);
}

static void toe_ip_from_sockaddr(const struct sockaddr *name, uint8_t ip[4], uint16_t *port) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    uint32_t a = lwip_ntohl(sin->sin_addr.s_addr);
    ip[0] = (uint8_t)(a >> 24);
    ip[1] = (uint8_t)(a >> 16);
    ip[2] = (uint8_t)(a >> 8);
    ip[3] = (uint8_t)a;
    *port = lwip_ntohs(sin->sin_port);
}

// Descriptor -> hardware socket number, or -1 with errno set: EBADF for a
// descriptor that is not ours, ENOTCONN for a dormant listener (it has no
// hardware socket right now; only accept(), select(), close() and the
// blocking-mode/timeout options mean anything on it).
static int toe_sn(int s) {
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        errno = toe_vfs_is_dormant_listener(s) ? ENOTCONN : EBADF;
    }
    return sn;
}

// SO_RCVTIMEO/SO_SNDTIMEO carry a struct timeval; the driver keeps milliseconds.
static int toe_timeval_to_ms(const void *optval, socklen_t optlen, uint32_t *ms) {
    if (optlen < (socklen_t)sizeof(struct timeval)) {
        errno = EINVAL;
        return -1;
    }
    const struct timeval *tv = (const struct timeval *)optval;
    *ms = (uint32_t)((tv->tv_sec * 1000) + (tv->tv_usec / 1000));
    return 0;
}

static void toe_ms_to_timeval(uint32_t ms, void *optval, socklen_t *optlen) {
    struct timeval *tv = (struct timeval *)optval;
    tv->tv_sec = (long)(ms / 1000);
    tv->tv_usec = (long)((ms % 1000) * 1000);
    *optlen = sizeof(struct timeval);
}

static int toe_backend_socket(int domain, int type, int protocol) {
    int sn = wiztoe_socket(domain, type, protocol);
    if (sn < 0) {
        errno = ENFILE;
        return -1;
    }
    int fd = toe_vfs_alloc_fd(sn);
    if (fd < 0) {
        wiztoe_close(sn);
        errno = ENFILE;
        return -1;
    }
    return fd;
}

static int toe_backend_close(int s) {
    if (!toe_vfs_owns_fd(s)) {
        errno = EBADF;
        return -1;
    }
    // Go through the close() syscall: our descriptors are registered with
    // permanent=false and a local fd, and only esp_vfs_close() frees such an
    // entry (esp_vfs_unregister_fd does not; it leaked one entry per socket
    // and exhausted the table after ~50 sockets).  close() also runs our VFS
    // .close op, which releases the hardware socket (if the descriptor has
    // one: a dormant listener does not).
    return close(s);
}

static int toe_backend_bind(int s, const struct sockaddr *name, socklen_t namelen) {
    (void)namelen;
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    if (wiztoe_bind(sn, lwip_ntohs(sin->sin_port)) < 0) {
        errno = EADDRINUSE;
        return -1;
    }
    return 0;
}

static int toe_backend_listen(int s, int backlog) {
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    if (wiztoe_listen(sn, backlog) < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return 0;
}

// A dormant listener needs a hardware socket back before it can accept.  In
// blocking mode wait for one up to SO_RCVTIMEO -- MicroPython slices a
// settimeout() into 100 ms SO_RCVTIMEO calls and retries, so returning at once
// would make settimeout(10) expire immediately; in non-blocking mode do not
// wait.  Returns the hardware socket number, or -1 when none came free.
static int toe_wait_for_dormant_listener(int s) {
    const wiztoe_socket_settings_t *settings = toe_vfs_dormant_listener_settings(s);
    uint32_t t0 = toe_time_us();
    for (;;) {
        int sn = toe_vfs_wake_dormant_listener(s);
        if (sn >= 0) {
            return sn;
        }
        if (settings->nonblock) {
            return -1;
        }
        if (settings->rcv_timeout_ms && (toe_time_us() - t0) >= settings->rcv_timeout_ms * 1000u) {
            return -1;
        }
        toe_yield_1ms();
    }
}

// The chip turns the listening hardware socket itself into the connection it
// accepts (there is no backlog).  To keep BSD's shape -- accept() returns a new
// descriptor, the listener stays usable -- the connection gets a fresh
// descriptor for that hardware socket, and the listener's descriptor is moved
// onto a new hardware socket listening with the same settings.  When every
// hardware socket is taken the listener goes dormant instead and the next
// accept()/select() on it retries, typically after the program has closed a
// connection.
//
// Between the accept and the new listener opening (a few SPI transactions)
// the port has no listening socket and a SYN arriving then is refused; the
// old model had the same gap at close(), and a much longer one.
static int toe_backend_accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    int sn = toe_vfs_sn_from_fd(s);
    if (sn < 0) {
        if (!toe_vfs_is_dormant_listener(s)) {
            errno = EBADF;
            return -1;
        }
        sn = toe_wait_for_dormant_listener(s);
        if (sn < 0) {
            errno = EWOULDBLOCK;
            return -1;
        }
    }
    int acc = wiztoe_accept(sn);
    if (acc == WIZTOE_ERR_TIMEOUT) {
        errno = EWOULDBLOCK;
        return -1;
    }
    if (acc < 0) {
        errno = EINVAL;
        return -1;
    }
    // acc == sn: the listener's hardware socket is the connection now.  Move
    // the listener first, so the connection's socket is free to be mapped.
    wiztoe_socket_settings_t settings;
    wiztoe_get_settings(sn, &settings);
    toe_vfs_relisten(s, &settings);
    int conn_fd = toe_vfs_alloc_fd(sn);
    if (conn_fd < 0) {
        // No descriptor to hand the connection out on: drop it.
        wiztoe_close(sn);
        errno = ENFILE;
        return -1;
    }
    uint8_t ip[4];
    uint16_t port;
    wiztoe_peer(sn, ip, &port);
    toe_fill_sockaddr(addr, addrlen, ip, port);
    return conn_fd;
}

static int toe_backend_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    (void)namelen;
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    uint8_t ip[4];
    uint16_t port;
    toe_ip_from_sockaddr(name, ip, &port);
    if (wiztoe_connect(sn, ip, port) < 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}

static int toe_backend_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    if (optval == NULL) {
        errno = EFAULT;
        return -1;
    }
    wiztoe_socket_settings_t *dormant = toe_vfs_dormant_listener_settings(s);
    if (dormant != NULL && level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
        // Remembered for the listener's next hardware socket (settimeout() on
        // a dormant listener lands here); other options need a live socket.
        uint32_t ms;
        if (toe_timeval_to_ms(optval, optlen, &ms) < 0) {
            return -1;
        }
        if (optname == SO_RCVTIMEO) {
            dormant->rcv_timeout_ms = ms;
        } else {
            dormant->snd_timeout_ms = ms;
        }
        return 0;
    }
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
            case SO_BROADCAST:
                // Harmless no-ops on hardware sockets.
                return 0;
            case SO_BINDTODEVICE:
                // The chip is the interface; every socket is already bound to it.
                return 0;
            case SO_KEEPALIVE:
                if (wiztoe_setsockopt(sn, WIZTOE_OPT_KEEPALIVE, optval, optlen) < 0) {
                    errno = EINVAL;
                    return -1;
                }
                return 0;
            case SO_RCVTIMEO:
            case SO_SNDTIMEO: {
                uint32_t ms;
                if (toe_timeval_to_ms(optval, optlen, &ms) < 0) {
                    return -1;
                }
                wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS : WIZTOE_OPT_SNDTIMEO_MS;
                if (wiztoe_setsockopt(sn, o, &ms, sizeof(ms)) < 0) {
                    errno = EINVAL;
                    return -1;
                }
                return 0;
            }
            default:
                break;
        }
    } else if (level == IPPROTO_TCP) {
        wiztoe_opt_t o;
        if (optname == TCP_NODELAY) {
            o = WIZTOE_OPT_NODELAY;
        } else if (optname == TCP_KEEPIDLE) {
            o = WIZTOE_OPT_KEEPIDLE;
        } else {
            errno = ENOPROTOOPT;
            return -1;
        }
        if (wiztoe_setsockopt(sn, o, optval, optlen) < 0) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    } else if (level == IPPROTO_IP) {
        if (optname == IP_MULTICAST_TTL || optname == IP_MULTICAST_IF || optname == IP_MULTICAST_LOOP) {
            return 0;
        }
        wiztoe_opt_t o;
        if (optname == IP_TTL) {
            o = WIZTOE_OPT_TTL;
        } else if (optname == IP_TOS) {
            o = WIZTOE_OPT_TOS;
        } else {
            errno = ENOPROTOOPT;
            return -1;
        }
        if (wiztoe_setsockopt(sn, o, optval, optlen) < 0) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

static int toe_backend_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen) {
    if (optval == NULL || optlen == NULL) {
        errno = EFAULT;
        return -1;
    }
    const wiztoe_socket_settings_t *dormant = toe_vfs_dormant_listener_settings(s);
    if (dormant != NULL && level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
        if (*optlen < (socklen_t)sizeof(struct timeval)) {
            errno = EINVAL;
            return -1;
        }
        toe_ms_to_timeval((optname == SO_RCVTIMEO) ? dormant->rcv_timeout_ms : dormant->snd_timeout_ms, optval, optlen);
        return 0;
    }
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_ERROR:
            case SO_TYPE:
            case SO_RCVBUF:
            case SO_SNDBUF: {
                if (*optlen < (socklen_t)sizeof(int)) {
                    errno = EINVAL;
                    return -1;
                }
                wiztoe_opt_t o = (optname == SO_ERROR) ? WIZTOE_OPT_ERROR
                    : (optname == SO_TYPE) ? WIZTOE_OPT_TYPE
                    : (optname == SO_RCVBUF) ? WIZTOE_OPT_RCVBUF
                    : WIZTOE_OPT_SNDBUF;
                size_t sz = (size_t)*optlen;
                if (wiztoe_getsockopt(sn, o, optval, &sz) < 0) {
                    errno = EINVAL;
                    return -1;
                }
                *optlen = (socklen_t)sz;
                return 0;
            }
            case SO_RCVTIMEO:
            case SO_SNDTIMEO: {
                if (*optlen < (socklen_t)sizeof(struct timeval)) {
                    errno = EINVAL;
                    return -1;
                }
                uint32_t ms = 0;
                size_t sz = sizeof(ms);
                wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS : WIZTOE_OPT_SNDTIMEO_MS;
                if (wiztoe_getsockopt(sn, o, &ms, &sz) < 0) {
                    errno = EINVAL;
                    return -1;
                }
                toe_ms_to_timeval(ms, optval, optlen);
                return 0;
            }
            default:
                break;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == IP_TTL || optname == IP_TOS) {
            if (*optlen < (socklen_t)sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            wiztoe_opt_t o = (optname == IP_TTL) ? WIZTOE_OPT_TTL : WIZTOE_OPT_TOS;
            size_t sz = (size_t)*optlen;
            if (wiztoe_getsockopt(sn, o, optval, &sz) < 0) {
                errno = EINVAL;
                return -1;
            }
            *optlen = (socklen_t)sz;
            return 0;
        }
    }
    errno = ENOPROTOOPT;
    return -1;
}

// The chip has no non-blocking mode; wiznet_toe.c emulates it, and O_NONBLOCK
// must be honoured because settimeout(0) is implemented as fcntl(O_NONBLOCK)
// with the expectation that the very next recv/send returns EWOULDBLOCK.
static int toe_backend_fcntl(int s, int cmd, int val) {
    wiztoe_socket_settings_t *dormant = toe_vfs_dormant_listener_settings(s);
    if (dormant != NULL) {
        // Remembered for the listener's next hardware socket.
        if (cmd == F_SETFL) {
            dormant->nonblock = (val & O_NONBLOCK) != 0;
            return 0;
        }
        if (cmd == F_GETFL) {
            return dormant->nonblock ? O_NONBLOCK : 0;
        }
        errno = ENOSYS;
        return -1;
    }
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    if (cmd == F_SETFL) {
        wiztoe_set_nonblock(sn, (val & O_NONBLOCK) != 0);
        return 0;
    }
    if (cmd == F_GETFL) {
        return wiztoe_get_nonblock(sn) ? O_NONBLOCK : 0;
    }
    errno = ENOSYS;
    return -1;
}

static ssize_t toe_backend_write(int s, const void *data, size_t size) {
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    int n = wiztoe_send(sn, data, size);
    if (n == WIZTOE_ERR_TIMEOUT) {
        errno = EWOULDBLOCK;
        return -1;
    }
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    return n;
}

static ssize_t toe_backend_recvfrom(int s, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
    (void)flags; // MSG_PEEK and MSG_DONTWAIT are not supported by the chip
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    uint8_t ip[4];
    uint16_t port = 0;
    int n;
    if (wiztoe_is_udp(sn)) {
        n = wiztoe_recvfrom(sn, mem, len, ip, &port);
    } else {
        n = wiztoe_recv(sn, mem, len);
        wiztoe_peer(sn, ip, &port);
    }
    if (n == WIZTOE_ERR_TIMEOUT) {
        errno = EWOULDBLOCK;
        return -1;
    }
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    toe_fill_sockaddr(from, fromlen, ip, port);
    return n;
}

static ssize_t toe_backend_sendto(int s, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen) {
    (void)flags;
    (void)tolen;
    int sn = toe_sn(s);
    if (sn < 0) {
        return -1;
    }
    int n;
    if (to == NULL || !wiztoe_is_udp(sn)) {
        n = wiztoe_send(sn, data, size);
    } else {
        uint8_t ip[4];
        uint16_t port;
        toe_ip_from_sockaddr(to, ip, &port);
        n = wiztoe_sendto(sn, data, size, ip, port);
    }
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    return n;
}

static int toe_backend_join_multicast_group(int s, const uint8_t *mreq) {
    (void)s;
    (void)mreq;
    // Multicast membership is not implemented for hardware sockets.
    errno = EOPNOTSUPP;
    return -1;
}

const socket_backend_t socket_backend_wiznet_toe = {
    .socket = toe_backend_socket,
    .close = toe_backend_close,
    .bind = toe_backend_bind,
    .listen = toe_backend_listen,
    .accept = toe_backend_accept,
    .connect = toe_backend_connect,
    .setsockopt = toe_backend_setsockopt,
    .getsockopt = toe_backend_getsockopt,
    .fcntl = toe_backend_fcntl,
    .write = toe_backend_write,
    .recvfrom = toe_backend_recvfrom,
    .sendto = toe_backend_sendto,
    .join_multicast_group = toe_backend_join_multicast_group,
};
