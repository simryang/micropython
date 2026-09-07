// DHCP client for the W5500 in TOE mode.
//
// Uses ioLibrary's DHCP_run() (Internet/DHCP/dhcp.c) on a hardware socket
// reserved out of the pool, matching upstream MicroPython's own WIZnet5k
// driver (extmod/network_wiznet5k.c, WIZNET5K_PROVIDED_STACK path).
//
// Note this deliberately does NOT go through our __wrap_lwip_* socket layer:
// ioLibrary's DHCP talks to the chip registers directly. That is fine because
// the socket it uses is reserved via wiztoe_socket_reserve(), so our fd layer
// can never hand it out. (wsm_driver's example took the opposite route --
// RFC 2131 over BSD sockets -- only because it had to serve a software-lwIP
// backend from the same source. We have no such constraint.)

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_DHCP_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_DHCP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TOE_DHCP_OK = 0,
    TOE_DHCP_NO_SOCKET,     // all 8 hardware sockets are in use
    TOE_DHCP_TIMEOUT,       // no server answered within the retry budget
    TOE_DHCP_FAILED,        // server refused, or an address conflict
} toe_dhcp_result_t;

// Runs a full DISCOVER/OFFER/REQUEST/ACK exchange, blocking until it settles.
// On success the chip is already configured (ioLibrary's default ip_assign
// writes SIPR/SUBR/GAR) and the lease is copied into the out-params.
// Starts the 1 s lease timer so renewals keep working afterwards.
toe_dhcp_result_t toe_dhcp_acquire(uint32_t timeout_ms,
    uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4]);

// Stops the lease timer and frees the reserved hardware socket.
void toe_dhcp_stop(void);

#endif
