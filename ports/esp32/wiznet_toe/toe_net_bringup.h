// W5500 TOE bring-up: SPI + the chip's own hardware TCP/IP stack.
//
// Also creates a "shadow" esp_netif (no driver, no data) that just holds a
// copy of the IPv4 identity. It is NOT what makes sockets work -- fd
// ownership lives in toe_vfs.c -- it only gives the rest of ESP-IDF
// something to look at. See docs/ARCHITECTURE_TOE.md.

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_NET_BRINGUP_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_NET_BRINGUP_H

#include <stdbool.h>
#include <stdint.h>

#include "toe_spi_port.h"

// Brings up SPI + chip hardware TCP/IP on the given wiring, with the given MAC
// and a 0.0.0.0 address (ifconfig() or DHCP supplies the real one later).
// Re-callable: every call resets the chip and reapplies, so it doubles as
// "start over from a clean chip". Returns false (with a printed reason) on
// failure.
bool toe_net_bringup(const uint8_t mac[6], const toe_spi_port_config_t *wiring);

// True once toe_net_bringup() has succeeded.
bool toe_net_is_up(void);

// Rewrites the chip's IPv4 identity without resetting it, and keeps the
// shadow netif in sync. Requires toe_net_is_up(). Used by ifconfig(...).
bool toe_net_set_ipinfo(const uint8_t ip[4], const uint8_t sn[4],
    const uint8_t gw[4], const uint8_t dns[4]);

// Reads the identity back from the chip's registers (authoritative).
void toe_net_get_ipinfo(uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4]);

// True when the W5500 PHY reports link up.
bool toe_net_link_up(void);

// Powers the chip down for active(False): closes sockets and holds reset.
void toe_net_shutdown(void);

// The MAC the chip is configured with. Defaults to the ESP32's own efuse
// Ethernet MAC (esp_read_mac(ESP_MAC_ETH)), so every board is unique without
// the caller inventing an address; override before bring-up with
// toe_net_set_mac().
void toe_net_get_mac(uint8_t mac[6]);
bool toe_net_set_mac(const uint8_t mac[6]);

// Per-socket TX/RX buffer size in KB (1/2/4/8/16). The chip has a fixed 16KB
// per direction, so a bigger buffer means fewer usable sockets -- see
// toe_net_usable_socks(). Recorded now, applied by the next bring-up, because
// wizchip_init() writes the sizes and resets every socket doing it.
bool toe_net_set_bufkb(uint8_t tx_kb, uint8_t rx_kb);
uint8_t toe_net_get_tx_kb(void);
uint8_t toe_net_get_rx_kb(void);

// How many hardware sockets the current buffer sizes leave usable (16/KB,
// capped at the chip's 8). Sockets past this got 0KB and must not be used.
int toe_net_usable_socks(void);

#endif
