/*
 * Copyright (c) 2024 WIZnet Co.,Ltd
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WIZnet TOE (TCP Offload Engine / hardwired TCP/IP) backend — neutral API.
 * Ported from WIZnet-PICO-LWIP-TOE-C (port/lwip/wiznet_toe.h).
 *
 * IMPORTANT: plain C types ONLY (no ioLibrary, no lwIP headers) so this can be
 * included by the socket backend (toe_socket_backend.c) without colliding with the ioLibrary
 * socket()/recv()/... names. wiznet_toe.c is the only TU that includes the
 * ioLibrary headers.
 *
 * File descriptors map 1:1 to W5500 hardware socket numbers (fd == sn, before
 * LWIP_SOCKET_OFFSET is applied by the caller).
 *
 * Vendored verbatim from wsm_driver
 * (D:\esp32s3-lab\wsm_driver\port\ioLibrary_Driver\inc\wiznet_toe.h).
 */
#ifndef _WIZNET_TOE_H_
#define _WIZNET_TOE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extra error return (besides -1): blocking call hit SO_RCVTIMEO.
 * The wrap layer maps this to errno EWOULDBLOCK. */
#define WIZTOE_ERR_TIMEOUT (-2)

/* Neutral option codes — the wrap layer maps (level, optname) to these. */
typedef enum {
    WIZTOE_OPT_KEEPALIVE,
    WIZTOE_OPT_KEEPIDLE,
    WIZTOE_OPT_NODELAY,
    WIZTOE_OPT_TTL,
    WIZTOE_OPT_TOS,
    WIZTOE_OPT_RCVTIMEO_MS,
    WIZTOE_OPT_SNDTIMEO_MS,
    WIZTOE_OPT_RCVBUF,
    WIZTOE_OPT_SNDBUF,
    WIZTOE_OPT_ERROR,
    WIZTOE_OPT_TYPE
} wiztoe_opt_t;

int  wiztoe_setsockopt(int fd, wiztoe_opt_t opt, const void *val, size_t len);
int  wiztoe_getsockopt(int fd, wiztoe_opt_t opt, void *val, size_t *len);

/* fd allocation / lifetime */
int  wiztoe_socket(int domain, int type, int protocol);   /* type 1=STREAM, 2=DGRAM */
int  wiztoe_close(int fd);

/* TCP */
int  wiztoe_bind(int fd, uint16_t port);
int  wiztoe_listen(int fd, int backlog);
/* Wait for a connection on listener fd (bounded by O_NONBLOCK / SO_RCVTIMEO,
 * WIZTOE_ERR_TIMEOUT when it runs out). On success the hardware socket fd IS
 * the connection -- the chip turns a listening socket into the connection it
 * accepted -- and fd is no longer a listener. The caller re-creates the
 * listener elsewhere with wiztoe_get_settings() + wiztoe_listen_with().
 * A connection the peer already half-closed (SOCK_CLOSE_WAIT) is accepted
 * too: its data is still in the RX buffer and recv() returns it, then EOF. */
int  wiztoe_accept(int fd);
int  wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port);

/* Everything about a socket that is not the hardware socket itself: what
 * wiztoe_listen_with() needs to re-create a listener on another hardware
 * socket after accept() turned the old one into a connection, or later, once a
 * hardware socket is free again (MicroPython addition, not in wsm_driver). */
typedef struct {
    uint16_t port;
    uint8_t  nodelay;
    uint8_t  nonblock;
    uint32_t rcv_timeout_ms;
    uint32_t snd_timeout_ms;
    uint8_t  keepalive_timer;   /* Sn_KPALVTR, units of 5 s; 0 = off */
    uint8_t  ttl;               /* Sn_TTL */
    uint8_t  tos;               /* Sn_TOS */
} wiztoe_socket_settings_t;

int  wiztoe_get_settings(int fd, wiztoe_socket_settings_t *out);
/* Open a fresh hardware socket listening with these settings. Returns its
 * number, or -1 when every usable hardware socket is taken. */
int  wiztoe_listen_with(const wiztoe_socket_settings_t *settings);
int  wiztoe_send(int fd, const void *buf, size_t len);
int  wiztoe_recv(int fd, void *buf, size_t len);           /* 0 = EOF */

/* UDP */
int  wiztoe_sendto(int fd, const void *buf, size_t len, const uint8_t ip[4], uint16_t port);
int  wiztoe_recvfrom(int fd, void *buf, size_t len, uint8_t ip[4], uint16_t *port);
/* No multicast join here. The chip latches the group's MAC when the socket
 * opens, so joining an already-bound socket means closing and reopening it --
 * a decision about the application's own traffic rather than something the
 * port layer should take on its behalf. */

/* Readiness for select()/poll(), decided from the chip's socket registers.
 * MicroPython addition (not in wsm_driver). Each out-param is set to 0/1.
 * A listener with a pending connection reports readable (accept won't block);
 * a peer-closed TCP socket reports readable too, so recv() can return EOF.
 * A listener the chip dropped to SOCK_CLOSED (handshake aborted by the peer,
 * e.g. a SYN scan) is put back into LISTEN here, as accept() also does. */
void wiztoe_poll(int fd, int *readable, int *writable, int *err);

/* O_NONBLOCK state (from the socket backend's fcntl). When set,
 * wiztoe_recv/recvfrom/send/accept return WIZTOE_ERR_TIMEOUT immediately
 * instead of waiting. */
int  wiztoe_set_nonblock(int fd, int on);
int  wiztoe_get_nonblock(int fd);

/* helpers */
int  wiztoe_is_udp(int fd);
void wiztoe_peer(int fd, uint8_t ip[4], uint16_t *port);
void wiztoe_getsockname(int fd, uint8_t ip[4], uint16_t *port);
void wiztoe_local_ip(uint8_t ip[4]);
void wiztoe_local_mac(uint8_t mac[6]);

/* raw hardware-socket reservation (for ioLibrary DHCP_run/DNS_run) */
int  wiztoe_socket_reserve(void);
void wiztoe_socket_release(int sn);

/* Drop the whole software socket table without touching the chip.
 * MicroPython addition (not in wsm_driver): a chip reset closes every
 * hardware socket, so anything still marked used here is stale. Without this,
 * a fd left open by an interrupted script keeps its table slot AND its port
 * reservation, and the next socket() lands on a different hardware socket
 * while the dead one still holds the port -- incoming SYNs then go to the
 * dead socket. Call right after resetting the chip. */
void wiztoe_reset_sockets(void);

/* Configure the chip's own network identity (TOE: the CHIP owns the IP). */
void wiztoe_network_init(const uint8_t ip[4], const uint8_t mask[4],
                         const uint8_t gw[4], const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* _WIZNET_TOE_H_ */
