/*
 * Copyright (c) 2024 WIZnet Co.,Ltd
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WIZnet TOE backend implementation (see wiznet_toe.h). Ported from
 * WIZnet-PICO-LWIP-TOE-C (port/lwip/wiznet_toe.c) to ESP-IDF via wsm_driver:
 *   - sleep_ms(1)   -> toe_yield_1ms()   (FreeRTOS vTaskDelay, see toe_port.h)
 *   - time_us_32()  -> toe_time_us()     (esp_timer)
 *
 * This is the ONLY TU (besides ioLibrary itself) that talks to the ioLibrary
 * driver. ioLibrary is built with WIZCHIP_PREFIXED_EXPORTS (see
 * esp32_common.cmake), so its socket API is reached as
 * WIZCHIP_EXPORT(name)(...) -- wizchip_socket(), wizchip_close() and so on --
 * and cannot collide with the POSIX names from newlib and lwIP. It includes
 * NO FreeRTOS/POSIX headers, only <string.h> + ioLibrary + toe_port.h.
 *
 * Vendored verbatim from wsm_driver
 * (D:\esp32s3-lab\wsm_driver\port\ioLibrary_Driver\src\wiznet_toe.c).
 */
#include <string.h>

#include "toe_iolibrary.h"
#include "socket.h"            /* ioLibrary socket API (hardware sockets) */

/* Declared here rather than including toe_net_bringup.h: that header pulls in
 * esp_netif, and this TU deliberately includes no ESP-IDF/POSIX headers (see
 * the close=wiz_close note above). Defined in toe_net_bringup.c. */
extern int toe_net_usable_socks(void);

#include "wiznet_toe.h"
#include "toe_port.h"          /* toe_yield_1ms(), toe_time_us() */

#ifndef WIZTOE_MAX_SOCK
#define WIZTOE_MAX_SOCK _WIZCHIP_SOCK_NUM_   /* 8 on W5500 */
#endif

typedef struct {
    uint8_t used;
    uint8_t is_udp;
    uint8_t opened;
    uint8_t listening;
    uint8_t nodelay;
    uint8_t nonblock;          /* O_NONBLOCK via fcntl -- see wiztoe_set_nonblock */
    uint16_t port;
    uint32_t rcv_timeout_ms;
    uint32_t snd_timeout_ms;
    uint8_t dst_ip[4];
    uint16_t dst_port;
    uint8_t connected;
} toe_sock_t;

static toe_sock_t g_toe[WIZTOE_MAX_SOCK];

static int toe_fd_valid(int fd) {
    return (fd >= 0) && (fd < WIZTOE_MAX_SOCK) && g_toe[fd].used;
}

static uint8_t toe_open_flag(int fd) {
    return g_toe[fd].nodelay ? SF_TCP_NODELAY : 0;
}

static int toe_listener_open(int fd);   /* defined with wiztoe_listen() below */

/* Open the hardware socket for a UDP fd. */
static int toe_open_udp(int fd) {
    if (WIZCHIP_EXPORT(socket)((uint8_t)fd, Sn_MR_UDP, g_toe[fd].port, 0) != fd) {
        return -1;
    }

    g_toe[fd].opened = 1;
    return 0;
}

void wiztoe_network_init(const uint8_t ip[4], const uint8_t mask[4],
    const uint8_t gw[4], const uint8_t mac[6]) {
    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));
    memcpy(ni.mac, mac, 6);
    memcpy(ni.ip, ip, 4);
    memcpy(ni.sn, mask, 4);
    memcpy(ni.gw, gw, 4);
    ni.dhcp = NETINFO_STATIC;
    #if (_WIZCHIP_ > W5500)
    {
        uint8_t syslock = SYS_NET_LOCK;
        ctlwizchip(CW_SYS_UNLOCK, &syslock);
    }
    #endif
    ctlnetwork(CN_SET_NETINFO, (void *)&ni);
}

int wiztoe_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)protocol;

    if (type != 1 /* SOCK_STREAM */ && type != 2 /* SOCK_DGRAM */) {
        return -1;
    }

    /* Only sockets that bring-up gave a buffer to. Raising the per-socket
     * buffer size spends the chip's fixed 16KB faster, leaving fewer of
     * them -- a 0KB socket would open but never carry data. */
    int usable = toe_net_usable_socks();
    if (usable > WIZTOE_MAX_SOCK) {
        usable = WIZTOE_MAX_SOCK;
    }

    for (int sn = 0; sn < usable; sn++)
    {
        if (!g_toe[sn].used) {
            memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
            g_toe[sn].used = 1;
            g_toe[sn].is_udp = (type == 2);
            return sn;                        /* fd == sn */
        }
    }
    return -1;
}

/* O_NONBLOCK, set through the socket backend's fcntl. MicroPython's
 * _socket_settimeout sets it for settimeout(0) and clears it otherwise, and
 * expects recv/send to return EWOULDBLOCK immediately instead of waiting. */
int wiztoe_set_nonblock(int fd, int on) {
    if (!toe_fd_valid(fd)) {
        return -1;
    }
    g_toe[fd].nonblock = (on != 0);
    return 0;
}

int wiztoe_get_nonblock(int fd) {
    return toe_fd_valid(fd) && g_toe[fd].nonblock;
}

int wiztoe_is_udp(int fd) {
    return toe_fd_valid(fd) && g_toe[fd].is_udp;
}

void wiztoe_poll(int fd, int *readable, int *writable, int *err) {
    *readable = 0;
    *writable = 0;
    *err = 0;

    if (!toe_fd_valid(fd)) {
        *err = 1;
        return;
    }

    if (g_toe[fd].is_udp) {
        if (g_toe[fd].opened) {
            *readable = (getSn_RX_RSR((uint8_t)fd) > 0);
            *writable = 1;
        }
        return;
    }

    uint8_t sr = getSn_SR((uint8_t)fd);

    if (g_toe[fd].listening) {
        if (sr == SOCK_CLOSED) {
            /* The peer aborted the handshake (RST, or a SYN scan) and the
             * chip closed the listener. Put it back, or a poll()-driven
             * server would never see this port again. */
            if (toe_listener_open(fd) < 0) {
                *err = 1;
                return;
            }
            sr = getSn_SR((uint8_t)fd);
        }
        /* Readable exactly when accept() would return at once. */
        *readable = (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT);
        return;
    }

    switch (sr) {
        case SOCK_ESTABLISHED:
            *readable = (getSn_RX_RSR((uint8_t)fd) > 0);
            *writable = (getSn_TX_FSR((uint8_t)fd) > 0);
            break;
        case SOCK_CLOSE_WAIT:
            /* Peer closed. Report readable so a waiting recv() wakes and
             * returns 0 (EOF) rather than hanging until timeout. */
            *readable = 1;
            *err = 1;
            break;
        case SOCK_CLOSED:
            *err = 1;
            break;
        default:
            /* SOCK_INIT / SYNSENT / SYNRECV / FIN_WAIT etc: still settling. */
            break;
    }
}

int wiztoe_bind(int fd, uint16_t port) {
    if (!toe_fd_valid(fd)) {
        return -1;
    }

    g_toe[fd].port = port;

    if (g_toe[fd].is_udp) {
        if (toe_open_udp(fd) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Put hardware socket fd into LISTEN on g_toe[fd].port. Shared by listen(),
 * by accept()/poll() when the chip dropped a listener to SOCK_CLOSED, and by
 * wiztoe_listen_with(). */
static int toe_listener_open(int fd) {
    if (WIZCHIP_EXPORT(socket)((uint8_t)fd, Sn_MR_TCP, g_toe[fd].port, toe_open_flag(fd)) != fd) {
        return -1;
    }
    g_toe[fd].opened = 1;

    /* Port 0 let the chip pick one; record it so the listener re-created
     * after accept() serves the same port, not a fresh random one. */
    g_toe[fd].port = getSn_PORT((uint8_t)fd);

    if (WIZCHIP_EXPORT(listen)((uint8_t)fd) != SOCK_OK) {
        return -1;
    }

    g_toe[fd].listening = 1;
    return 0;
}

int wiztoe_listen(int fd, int backlog) {
    (void)backlog;

    if (!toe_fd_valid(fd) || g_toe[fd].is_udp) {
        return -1;
    }

    return toe_listener_open(fd);
}

int wiztoe_accept(int fd) {
    if (!toe_fd_valid(fd) || !g_toe[fd].listening) {
        return -1;
    }

    /* Wall-clock deadline, not an iteration count. toe_yield_1ms() is
     * vTaskDelay(pdMS_TO_TICKS(1)) which at CONFIG_FREERTOS_HZ=100 rounds to
     * vTaskDelay(0) -- a bare yield of microseconds, NOT 1 ms. Counting loop
     * iterations as milliseconds (the old `++waited >= rcv_timeout_ms`) fired
     * accept(timeout=2s) after ~2000 quick spins instead of 2 seconds. Same
     * bug class as the recv() wait loop below; measure elapsed time instead. */
    uint32_t t0 = toe_time_us();
    for (;;)
    {
        uint8_t sr = getSn_SR((uint8_t)fd);

        if (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT) {
            /* This hardware socket is the connection from here on. A caller
             * that wants to keep accepting opens a new listener with
             * wiztoe_listen_with(); this one no longer is. */
            g_toe[fd].listening = 0;
            return fd;
        }
        if (sr == SOCK_CLOSED) {
            /* Handshake aborted by the peer: back to LISTEN. */
            if (toe_listener_open(fd) < 0) {
                return -1;
            }
        }
        if (g_toe[fd].nonblock) {
            return WIZTOE_ERR_TIMEOUT;      /* setblocking(False): don't wait */
        }
        if (g_toe[fd].rcv_timeout_ms &&
            (toe_time_us() - t0) >= g_toe[fd].rcv_timeout_ms * 1000u) {
            return WIZTOE_ERR_TIMEOUT;
        }
        toe_yield_1ms();
    }
}

int wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port) {
    if (!toe_fd_valid(fd)) {
        return -1;
    }

    if (g_toe[fd].is_udp) {
        memcpy(g_toe[fd].dst_ip, ip, 4);
        g_toe[fd].dst_port = port;
        g_toe[fd].connected = 1;
        return 0;
    }

    /* Randomized ephemeral local port to avoid TIME_WAIT 4-tuple reuse after a
     * reset (ioLibrary's static sock_any_port restarts at 0xC000 each boot). */
    uint16_t lport = g_toe[fd].port;
    if (lport == 0) {
        lport = (uint16_t)(0xC000u + (toe_time_us() % 0x3FF0u));
        g_toe[fd].port = lport;
    }
    if (WIZCHIP_EXPORT(socket)((uint8_t)fd, Sn_MR_TCP, lport, toe_open_flag(fd)) != fd) {
        return -1;
    }
    g_toe[fd].opened = 1;

    return (WIZCHIP_EXPORT(connect)((uint8_t)fd, (uint8_t *)ip, port) == SOCK_OK) ? 0 : -1;
}

int wiztoe_send(int fd, const void *buf, size_t len) {
    if (!toe_fd_valid(fd) || g_toe[fd].is_udp) {
        return -1;
    }
    if (len > 0xFFFF) {
        len = 0xFFFF;
    }

    /* ioLibrary's send() waits for TX buffer space in a while(1) with no
     * yield and, in blocking mode, no exit -- the same pathology as its
     * disconnect(). A peer that stops reading therefore hung the whole
     * scheduler (verified: settimeout(2) send into a silent peer never
     * returned). So wait for space HERE, yielding, against a wall-clock
     * deadline, and hand send() only what already fits: its internal wait
     * then never spins. Partial sends are fine -- MicroPython's stream
     * layer loops. */
    uint32_t t0 = toe_time_us();
    uint16_t free_sz;
    for (;;)
    {
        uint8_t sr = getSn_SR((uint8_t)fd);
        if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) {
            return -1;
        }
        free_sz = (uint16_t)getSn_TX_FSR((uint8_t)fd);
        if (free_sz > 0) {
            break;
        }
        if (g_toe[fd].nonblock) {
            return WIZTOE_ERR_TIMEOUT;         /* -> EWOULDBLOCK, immediately */
        }
        if (g_toe[fd].snd_timeout_ms &&
            (toe_time_us() - t0) >= g_toe[fd].snd_timeout_ms * 1000u) {
            return WIZTOE_ERR_TIMEOUT;
        }
        toe_yield_1ms();
    }
    if (len > free_sz) {
        len = free_sz;
    }

    for (;;)
    {
        int32_t n = WIZCHIP_EXPORT(send)((uint8_t)fd, (uint8_t *)buf, (uint16_t)len);
        if (n != SOCK_BUSY) {
            return (n < 0) ? -1 : (int)n;
        }
        /* Previous SEND command still in flight (SENDOK pending). Retry
         * under the same deadline rather than spinning inside ioLibrary. */
        if (g_toe[fd].nonblock) {
            return WIZTOE_ERR_TIMEOUT;
        }
        if (g_toe[fd].snd_timeout_ms &&
            (toe_time_us() - t0) >= g_toe[fd].snd_timeout_ms * 1000u) {
            return WIZTOE_ERR_TIMEOUT;
        }
        toe_yield_1ms();
    }
}

int wiztoe_recv(int fd, void *buf, size_t len) {
    if (!toe_fd_valid(fd) || g_toe[fd].is_udp) {
        return -1;
    }
    if (len > 0xFFFF) {
        len = 0xFFFF;
    }

    /* Wall-clock deadline, not an iteration count: toe_yield_1ms() often
     * returns in well under a millisecond (tick-boundary rounding), so a
     * counted loop fired timeouts ~4x early (measured 497 ms for a nominal
     * 2 s). MicroPython sets SO_RCVTIMEO to 100 ms slices and re-calls, so
     * the accuracy of its overall settimeout() rides on this. */
    uint32_t t0 = toe_time_us();
    for (;;)
    {
        if (getSn_RX_RSR((uint8_t)fd) > 0) {
            break;
        }
        if (getSn_SR((uint8_t)fd) != SOCK_ESTABLISHED) {
            /* Peer closed. The RX_RSR read above and this state read are
             * separate SPI transactions, so the payload can land in between:
             * re-check before declaring EOF or it is silently dropped.
             * (Seen with HTTP/1.0 servers that send the body and FIN
             * back-to-back -- recv() returned b'' 1-2 times in 10.) */
            if (getSn_RX_RSR((uint8_t)fd) > 0) {
                break;
            }
            return 0;                          /* EOF */
        }
        if (g_toe[fd].nonblock) {
            return WIZTOE_ERR_TIMEOUT;         /* -> EWOULDBLOCK, immediately */
        }
        if (g_toe[fd].rcv_timeout_ms &&
            (toe_time_us() - t0) >= g_toe[fd].rcv_timeout_ms * 1000u) {
            return WIZTOE_ERR_TIMEOUT;
        }
        /* Always yield so the ESP-IDF idle task / watchdog run. */
        toe_yield_1ms();
    }

    int32_t n = WIZCHIP_EXPORT(recv)((uint8_t)fd, (uint8_t *)buf, (uint16_t)len);
    if (n == SOCKERR_SOCKSTATUS || n == SOCKERR_SOCKCLOSED) {
        return 0;                              /* EOF */
    }
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_sendto(int fd, const void *buf, size_t len,
    const uint8_t ip[4], uint16_t port) {
    if (!toe_fd_valid(fd) || !g_toe[fd].is_udp) {
        return -1;
    }
    if (len > 0xFFFF) {
        len = 0xFFFF;
    }

    if (!g_toe[fd].opened) {
        if (WIZCHIP_EXPORT(socket)((uint8_t)fd, Sn_MR_UDP, g_toe[fd].port, 0) != fd) {
            return -1;
        }
        g_toe[fd].opened = 1;
    }

    int32_t n = WIZCHIP_EXPORT(sendto)((uint8_t)fd, (uint8_t *)buf, (uint16_t)len,
        (uint8_t *)ip, port);
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_recvfrom(int fd, void *buf, size_t len, uint8_t ip[4], uint16_t *port) {
    if (!toe_fd_valid(fd) || !g_toe[fd].is_udp || !g_toe[fd].opened) {
        return -1;
    }
    if (len > 0xFFFF) {
        len = 0xFFFF;
    }

    uint32_t t0 = toe_time_us();       /* wall clock, same reason as wiztoe_recv */
    for (;;)
    {
        if (getSn_RX_RSR((uint8_t)fd) > 0) {
            break;
        }
        if (getSn_SR((uint8_t)fd) != SOCK_UDP) {
            /* Same two-transaction race as wiztoe_recv(): a datagram may have
             * landed between the RX_RSR read and this one. Drain first. */
            if (getSn_RX_RSR((uint8_t)fd) > 0) {
                break;
            }
            return -1;
        }
        if (g_toe[fd].nonblock) {
            return WIZTOE_ERR_TIMEOUT;
        }
        if (g_toe[fd].rcv_timeout_ms &&
            (toe_time_us() - t0) >= g_toe[fd].rcv_timeout_ms * 1000u) {
            return WIZTOE_ERR_TIMEOUT;
        }
        toe_yield_1ms();
    }

    int32_t n = WIZCHIP_EXPORT(recvfrom)((uint8_t)fd, (uint8_t *)buf, (uint16_t)len, ip, port);
    return (n < 0) ? -1 : (int)n;
}

void wiztoe_peer(int fd, uint8_t ip[4], uint16_t *port) {
    if (!toe_fd_valid(fd)) {
        memset(ip, 0, 4);
        *port = 0;
        return;
    }
    if (g_toe[fd].is_udp) {
        memcpy(ip, g_toe[fd].dst_ip, 4);
        *port = g_toe[fd].dst_port;
        return;
    }
    getSn_DIPR((uint8_t)fd, ip);
    *port = getSn_DPORT((uint8_t)fd);
}

void wiztoe_getsockname(int fd, uint8_t ip[4], uint16_t *port) {
    wiz_NetInfo ni;
    if (!toe_fd_valid(fd)) {
        memset(ip, 0, 4);
        *port = 0;
        return;
    }
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(ip, ni.ip, 4);
    *port = g_toe[fd].port;
}

void wiztoe_local_ip(uint8_t ip[4]) {
    wiz_NetInfo ni;
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(ip, ni.ip, 4);
}

void wiztoe_local_mac(uint8_t mac[6]) {
    wiz_NetInfo ni;
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(mac, ni.mac, 6);
}

void wiztoe_reset_sockets(void) {
    memset(g_toe, 0, sizeof(g_toe));
}

int wiztoe_socket_reserve(void) {
    for (int sn = 0; sn < WIZTOE_MAX_SOCK; sn++)
    {
        if (!g_toe[sn].used) {
            memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
            g_toe[sn].used = 1;
            return sn;
        }
    }
    return -1;
}

void wiztoe_socket_release(int sn) {
    if (sn >= 0 && sn < WIZTOE_MAX_SOCK) {
        WIZCHIP_EXPORT(close)((uint8_t)sn);
        memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
    }
}

/* Graceful close, bounded and yielding.
 *
 * ioLibrary's disconnect() cannot be used here: it spins on Sn_SR with no
 * yield until the socket reaches SOCK_CLOSED, escaping only on a
 * retransmission timeout. That deadlocks whenever the peer is blocked writing
 * into a window we stopped draining -- it never calls close(), so its FIN
 * never arrives; and our own FIN was already ACKed, so nothing is pending
 * retransmission and Sn_IR_TIMEOUT never fires either. The spin then starves
 * the scheduler, taking the USB-CDC REPL down with it and leaving the host's
 * COM port locked. (Reproduced 2026-09-02: connect, leave data unread,
 * close -> board hangs on the first call.)
 *
 * So drive the DISCON ourselves, wait with toe_yield_1ms(), and fall back to
 * an abortive close once the deadline passes. */
#define TOE_CLOSE_TIMEOUT_MS 1000

static void toe_tcp_disconnect_if_connected(int fd) {
    uint8_t sr = getSn_SR((uint8_t)fd);
    if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) {
        return;
    }

    setSn_CR((uint8_t)fd, Sn_CR_DISCON);
    while (getSn_CR((uint8_t)fd)) {
        ;                         /* command latch, clears in microseconds */

    }
    /* Wall-clock deadline. The old `waited < TOE_CLOSE_TIMEOUT_MS` counted loop
     * iterations as ms, but toe_yield_1ms() is a bare yield (see wiztoe_accept),
     * so 1000 iterations elapsed in ~75 ms, not the intended 1 s -- an abortive
     * close 13x sooner than documented. */
    uint32_t t0 = toe_time_us();
    for (;;)
    {
        if (getSn_SR((uint8_t)fd) == SOCK_CLOSED) {
            return;               /* peer completed the 4-way close */
        }
        if (getSn_IR((uint8_t)fd) & Sn_IR_TIMEOUT) {
            break;
        }
        if ((toe_time_us() - t0) >= TOE_CLOSE_TIMEOUT_MS * 1000u) {
            break;
        }
        toe_yield_1ms();
    }

    WIZCHIP_EXPORT(close)((uint8_t)fd); /* abortive: the graceful path did not finish */
}

int wiztoe_close(int fd) {
    if (!toe_fd_valid(fd)) {
        return -1;
    }

    if (g_toe[fd].opened && !g_toe[fd].is_udp) {
        toe_tcp_disconnect_if_connected(fd);
    }
    if (g_toe[fd].opened) {
        WIZCHIP_EXPORT(close)((uint8_t)fd);
    }
    memset(&g_toe[fd], 0, sizeof(g_toe[fd]));
    return 0;
}

int wiztoe_get_settings(int fd, wiztoe_socket_settings_t *out) {
    if (!toe_fd_valid(fd) || out == NULL) {
        return -1;
    }

    out->port = g_toe[fd].port;
    out->nodelay = g_toe[fd].nodelay;
    out->nonblock = g_toe[fd].nonblock;
    out->rcv_timeout_ms = g_toe[fd].rcv_timeout_ms;
    out->snd_timeout_ms = g_toe[fd].snd_timeout_ms;
    /* Per-socket chip registers. They survive the OPEN that turned the
     * listener into a connection, so the connection still carries the
     * listener's values (inheriting them, as lwIP's accept() does too). */
    out->keepalive_timer = getSn_KPALVTR((uint8_t)fd);
    out->ttl = getSn_TTL((uint8_t)fd);
    out->tos = getSn_TOS((uint8_t)fd);
    return 0;
}

int wiztoe_listen_with(const wiztoe_socket_settings_t *settings) {
    if (settings == NULL) {
        return -1;
    }

    int fd = wiztoe_socket(0, 1 /* SOCK_STREAM */, 0);
    if (fd < 0) {
        return -1;                             /* every usable socket is taken */

    }
    g_toe[fd].port = settings->port;
    g_toe[fd].nodelay = settings->nodelay;
    g_toe[fd].nonblock = settings->nonblock;
    g_toe[fd].rcv_timeout_ms = settings->rcv_timeout_ms;
    g_toe[fd].snd_timeout_ms = settings->snd_timeout_ms;
    if (toe_listener_open(fd) < 0) {
        wiztoe_close(fd);
        return -1;
    }
    setSn_KPALVTR((uint8_t)fd, settings->keepalive_timer);
    setSn_TTL((uint8_t)fd, settings->ttl);
    setSn_TOS((uint8_t)fd, settings->tos);
    return fd;
}

int wiztoe_setsockopt(int fd, wiztoe_opt_t opt, const void *val, size_t len) {
    if (!toe_fd_valid(fd) || val == NULL || len == 0) {
        return -1;
    }

    int v = (len >= sizeof(int)) ? *(const int *)val : *(const uint8_t *)val;

    switch (opt)
    {
        case WIZTOE_OPT_KEEPALIVE:
            setSn_KPALVTR((uint8_t)fd, v ? 12 : 0);
            return 0;
        case WIZTOE_OPT_KEEPIDLE:
            if (v < 5) {
                v = 5;
            }
            if (v > 5 * 255) {
                v = 5 * 255;
            }
            setSn_KPALVTR((uint8_t)fd, (uint8_t)(v / 5));
            return 0;
        case WIZTOE_OPT_NODELAY:
            g_toe[fd].nodelay = (v != 0);
            return 0;
        case WIZTOE_OPT_TTL:
            setSn_TTL((uint8_t)fd, (uint8_t)v);
            return 0;
        case WIZTOE_OPT_TOS:
            setSn_TOS((uint8_t)fd, (uint8_t)v);
            return 0;
        case WIZTOE_OPT_RCVTIMEO_MS:
            if (len < sizeof(uint32_t)) {
                return -1;
            }
            g_toe[fd].rcv_timeout_ms = *(const uint32_t *)val;
            return 0;
        case WIZTOE_OPT_SNDTIMEO_MS:
            if (len < sizeof(uint32_t)) {
                return -1;
            }
            g_toe[fd].snd_timeout_ms = *(const uint32_t *)val;
            return 0;
        default:
            return -1;
    }
}

int wiztoe_getsockopt(int fd, wiztoe_opt_t opt, void *val, size_t *len) {
    if (!toe_fd_valid(fd) || val == NULL || len == NULL || *len < sizeof(int)) {
        return -1;
    }

    int *out = (int *)val;

    switch (opt)
    {
        case WIZTOE_OPT_ERROR:
            *out = 0;
            break;
        case WIZTOE_OPT_TYPE:
            *out = g_toe[fd].is_udp ? 2 : 1;
            break;
        case WIZTOE_OPT_RCVBUF:
            *out = (int)getSn_RxMAX((uint8_t)fd);
            break;
        case WIZTOE_OPT_SNDBUF:
            *out = (int)getSn_TxMAX((uint8_t)fd);
            break;
        case WIZTOE_OPT_TTL:
            *out = (int)getSn_TTL((uint8_t)fd);
            break;
        case WIZTOE_OPT_TOS:
            *out = (int)getSn_TOS((uint8_t)fd);
            break;
        case WIZTOE_OPT_RCVTIMEO_MS:
            *(uint32_t *)val = g_toe[fd].rcv_timeout_ms;
            break;
        case WIZTOE_OPT_SNDTIMEO_MS:
            *(uint32_t *)val = g_toe[fd].snd_timeout_ms;
            break;
        default:
            return -1;
    }
    *len = sizeof(int);
    return 0;
}
