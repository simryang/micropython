// DNS resolver for the W5500 in TOE mode.
//
// Backed by ioLibrary's DNS_run() (Internet/DNS/dns.c) on a hardware socket
// borrowed from the pool for the duration of the query.
//
// Plain C types only -- no lwIP headers -- so that toe_socket_backend.c can
// call this while also including lwip/sockets.h. (Same split as wiznet_toe.c
// vs toe_socket_backend.c: only the ioLibrary-side TU sees ioLibrary's socket
// names.)

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_DNS_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_DNS_H

#include <stdbool.h>
#include <stdint.h>

// Resolves `name` to an IPv4 address using `server`. Blocks for up to
// MAX_DNS_RETRY * DNS_WAIT_TIME seconds (ioLibrary defaults: 2 * 3 s).
// Returns false on timeout, refusal, or if no hardware socket is free.
bool toe_dns_resolve(const uint8_t server[4], const char *name, uint8_t out_ip[4]);

// Reads the DNS server currently configured on the chip (set by ifconfig()
// or taken from the DHCP lease). Returns false if it is 0.0.0.0.
bool toe_dns_server(uint8_t server[4]);

#endif
