// W5500 TOE bring-up: SPI + the chip's own hardware TCP/IP stack.
// Ported from wsm_driver's port/backend/src/net_backend_toe.c
// (D:\esp32s3-lab\wsm_driver\port\backend\src\net_backend_toe.c), with the
// SPI bring-up swapped for our own toe_spi_port_init()/toe_spi_port_reset()
// (already proven on real hardware in step 2) instead of wsm_driver_spi_*.

#include "toe_net_bringup.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "toe_spi_port.h"
#include "wiznet_toe/Ethernet/W5500/w5500.h"

#include "toe_socket_backend.h"
#include "toe_vfs.h"
#include "wiznet_toe.h"

// Per-socket buffer sizes (KB). The W5500 has 16KB of TX and 16KB of RX
// buffer to divide among its 8 sockets, so size and socket count trade off
// directly: 2KB x 8 (the default) uses the whole budget, 4KB leaves only 4
// usable sockets, 8KB only 2.
//
// Size matters because draining a socket's whole RX buffer in one read stalls
// on the TCP window update: measured 597 KB/s reading 1KB at a time against
// 204 KB/s reading the full 2KB. A bigger buffer moves that cliff up.
#define TOE_TX_BUF_KB 2
#define TOE_RX_BUF_KB 2
#define TOE_BUF_BUDGET_KB 16

static uint8_t s_tx_kb = TOE_TX_BUF_KB;
static uint8_t s_rx_kb = TOE_RX_BUF_KB;

static bool toe_buf_kb_valid(uint8_t kb) {
    return kb == 1 || kb == 2 || kb == 4 || kb == 8 || kb == 16;
}

// Sockets that actually get a buffer. The rest are left at 0KB by bring-up
// and must not be handed out -- a zero-buffer socket cannot carry data.
int toe_net_usable_socks(void) {
    int by_tx = TOE_BUF_BUDGET_KB / s_tx_kb;
    int by_rx = TOE_BUF_BUDGET_KB / s_rx_kb;
    int n = (by_tx < by_rx) ? by_tx : by_rx;
    return (n > _WIZCHIP_SOCK_NUM_) ? _WIZCHIP_SOCK_NUM_ : n;
}

uint8_t toe_net_get_tx_kb(void) {
    return s_tx_kb;
}

uint8_t toe_net_get_rx_kb(void) {
    return s_rx_kb;
}

// Recorded now, applied by the next bring-up: the sizes are written by
// wizchip_init(), which also resets every socket, so changing them under a
// live socket would silently break it.
bool toe_net_set_bufkb(uint8_t tx_kb, uint8_t rx_kb) {
    if (!toe_buf_kb_valid(tx_kb) || !toe_buf_kb_valid(rx_kb)) {
        return false;
    }
    s_tx_kb = tx_kb;
    s_rx_kb = rx_kb;
    return true;
}

static bool s_net_up;
static esp_netif_t *s_shadow;
static uint8_t s_mac[6];
static bool s_mac_set;

bool toe_net_is_up(void) {
    return s_net_up;
}

void toe_net_get_mac(uint8_t mac[6]) {
    if (!s_mac_set) {
        // No explicit MAC: use the ESP32's own efuse Ethernet MAC, the same
        // source esp_eth uses. Guarantees a unique, vendor-assigned address
        // per board instead of a hardcoded literal that would collide when
        // two boards run the same example.
        if (esp_read_mac(s_mac, ESP_MAC_ETH) != ESP_OK) {
            memset(s_mac, 0, sizeof(s_mac));
        }
        s_mac_set = true;
    }
    memcpy(mac, s_mac, 6);
}

bool toe_net_set_mac(const uint8_t mac[6]) {
    memcpy(s_mac, mac, 6);
    s_mac_set = true;
    if (s_net_up) {
        setSHAR(s_mac);
    }
    return true;
}

void toe_net_get_ipinfo(uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4]) {
    // Read from the chip: on TOE the chip owns the identity, so its
    // registers are authoritative -- not the shadow netif's copy.
    wiz_NetInfo info = {0};
    wizchip_getnetinfo(&info);
    memcpy(ip, info.ip, 4);
    memcpy(sn, info.sn, 4);
    memcpy(gw, info.gw, 4);
    memcpy(dns, info.dns, 4);
}

bool toe_net_link_up(void) {
    return (getPHYCFGR() & PHYCFGR_LNK_ON) != 0;
}

static void toe_net_sync_shadow(const uint8_t ip[4], const uint8_t sn[4], const uint8_t gw[4]) {
    if (!s_shadow) {
        return;
    }
    esp_netif_ip_info_t info = {0};
    info.ip.addr = ESP_IP4TOADDR(ip[0], ip[1], ip[2], ip[3]);
    info.netmask.addr = ESP_IP4TOADDR(sn[0], sn[1], sn[2], sn[3]);
    info.gw.addr = ESP_IP4TOADDR(gw[0], gw[1], gw[2], gw[3]);
    esp_netif_set_ip_info(s_shadow, &info);
}

bool toe_net_set_ipinfo(const uint8_t ip[4], const uint8_t sn[4],
    const uint8_t gw[4], const uint8_t dns[4]) {
    if (!s_net_up) {
        return false;
    }
    wiz_NetInfo info = {0};
    toe_net_get_mac(info.mac);
    memcpy(info.ip, ip, 4);
    memcpy(info.sn, sn, 4);
    memcpy(info.gw, gw, 4);
    memcpy(info.dns, dns, 4);
    info.dhcp = NETINFO_STATIC;
    wizchip_setnetinfo(&info);
    toe_net_sync_shadow(ip, sn, gw);
    return true;
}

void toe_net_shutdown(void) {
    if (!s_net_up) {
        return;
    }
    // Hand new sockets back to lwIP before tearing the chip down, so a socket()
    // racing with this does not get a hardware socket we are about to kill.
    socket_backend_set_default(&socket_backend_lwip);
    wiztoe_reset_sockets();
    toe_vfs_release_all();
    toe_spi_port_hold_reset();
    s_net_up = false;
}

bool toe_net_bringup(const wiz_NetInfo *net_info) {
    // esp_netif_init() is idempotent in ESP-IDF (safe if network.LAN/WLAN
    // already called it). The default event loop is already created
    // unconditionally in main.c before the MicroPython task starts, so we
    // must NOT create it again here -- a second esp_event_loop_create_default()
    // call returns ESP_ERR_INVALID_STATE, which would abort under
    // ESP_ERROR_CHECK.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        printf("wiznettoe: esp_netif_init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    // Shadow netif: no driver, no data. Exists only so esp_netif's lwIP
    // socket VFS fd-range gets registered, which the step-4 wrap layer
    // needs for close()/fcntl() on TOE fds to route correctly.
    // Created once; the chip config below re-runs on every call so that
    // net_init() is a usable "start over from a clean chip" entry point.
    if (!s_shadow) {
        esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
        esp_netif_config_t netif_cfg = {
            .base = &base,
            .driver = NULL,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
        };
        s_shadow = esp_netif_new(&netif_cfg);
        if (!s_shadow) {
            printf("wiznettoe: esp_netif_new (shadow) failed\n");
            return false;
        }
        esp_netif_dhcpc_stop(s_shadow);
    }
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ESP_IP4TOADDR(net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
    ip.netmask.addr = ESP_IP4TOADDR(net_info->sn[0], net_info->sn[1], net_info->sn[2], net_info->sn[3]);
    ip.gw.addr = ESP_IP4TOADDR(net_info->gw[0], net_info->gw[1], net_info->gw[2], net_info->gw[3]);
    esp_netif_set_ip_info(s_shadow, &ip);

    if (!toe_spi_port_init()) {
        return false;  // reason already printed by toe_spi_port_init
    }
    toe_spi_port_reset();
    if (getVERSIONR() != 0x04) {
        printf("wiznettoe: unexpected VERSIONR (chip not responding as W5500)\n");
        return false;
    }

    // Sockets past the budget stay at 0KB; wiztoe_socket() will not hand them
    // out (it asks toe_net_usable_socks()).
    uint8_t tx[8] = {0}, rx[8] = {0};
    int usable = toe_net_usable_socks();
    for (int i = 0; i < usable; i++) {
        tx[i] = s_tx_kb;
        rx[i] = s_rx_kb;
    }
    if (wizchip_init(tx, rx) != 0) {
        printf("wiznettoe: wizchip_init failed\n");
        return false;
    }

    // The reset above closed every hardware socket, so the backend's
    // software socket table must be dropped to match. Otherwise a fd left
    // behind by an interrupted script keeps both its table slot and its port,
    // and the next socket() lands on a different hardware socket while the
    // dead one still holds the port -- incoming SYNs go to the dead socket.
    wiztoe_reset_sockets();
    if (!toe_vfs_register()) {
        return false;  // reason already printed by toe_vfs_register
    }
    toe_vfs_release_all();   // fds mapping to the now-dead hardware sockets
    // From here on new sockets are W5500 hardware sockets.  Before this point
    // (and in a firmware that never brings the interface up) they are lwIP's.
    socket_backend_set_default(&socket_backend_wiznet_toe);

    wizchip_setnetinfo((wiz_NetInfo *)net_info);

    printf("wiznettoe: TOE up: %u.%u.%u.%u\n",
        net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
    s_net_up = true;
    return true;
}
