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
int  wiztoe_accept(int fd);                                /* listener becomes the connection */
int  wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port);
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
 * a peer-closed TCP socket reports readable too, so recv() can return EOF. */
void wiztoe_poll(int fd, int *readable, int *writable, int *err);

/* True while the software socket slot is still allocated. wiztoe_close() has
 * two outcomes: an accepted listener is RE-ARMED (slot kept, so the fd must
 * stay valid for the next accept()), anything else is fully closed (slot
 * freed). Callers that own an fd mapping must check this to decide whether to
 * release the fd. MicroPython addition (not in wsm_driver). */
int  wiztoe_is_used(int fd);

/* O_NONBLOCK state (wrapped lwip_fcntl). When set, wiztoe_recv/recvfrom/send
 * return WIZTOE_ERR_TIMEOUT immediately instead of waiting. */
int  wiztoe_set_nonblock(int fd, int on);
int  wiztoe_get_nonblock(int fd);

/* True when fd is an accepted listener, i.e. wiztoe_close() would RE-ARM it on
 * the same hardware socket instead of freeing it. The backend's close checks
 * this to keep the fd registered across a re-arm (see toe_backend_close). */
int  wiztoe_is_rearming_listener(int fd);

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
