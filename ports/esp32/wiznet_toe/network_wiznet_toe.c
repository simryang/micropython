// network.WIZNET_TOE -- MicroPython network-module face for the W5500 in TOE
// (hardware TCP/IP) mode.
//
// Deliberately shaped like network.LAN so a MACRAW script ports over with one
// changed line. The difference that cannot be hidden: on TOE the CHIP owns the
// IP stack, so ifconfig() reads back from the chip's registers rather than
// from an esp_netif. See docs/ARCHITECTURE_TOE.md.
//
//   import network
//   from machine import Pin, SPI
//   spi = SPI(1, baudrate=20_000_000, sck=Pin(12), mosi=Pin(11), miso=Pin(13))
//   nic = network.WIZNET_TOE(spi=spi, cs=Pin(10), reset=Pin(9))
//   nic.active(True)
//   nic.ifconfig(('192.168.7.23', '255.255.255.0', '192.168.7.1', '8.8.8.8'))
//   nic.isconnected()

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objtuple.h"
#include "py/runtime.h"

#include "extmod/modmachine.h"
#include "extmod/virtpin.h"

#include "modnetwork.h"   // ETH_* status codes, shared with network.LAN

#include "toe_dhcp.h"
#include "toe_net_bringup.h"
#include "toe_spi_port.h"
#include "toe_vfs.h"

// Upstream MicroPython's WIZnet5k driver uses ifconfig('dhcp') for exactly
// this; matching it keeps the two drivers' APIs interchangeable.
#define TOE_DHCP_TIMEOUT_MS 15000

typedef struct _wiznet_toe_obj_t {
    mp_obj_base_t base;
    // How the chip is wired, from the constructor.  Kept as plain numbers,
    // the way network.LAN keeps its pins, so nothing here is a GC root.
    toe_spi_port_config_t wiring;
    bool wired;
} wiznet_toe_obj_t;

const mp_obj_type_t network_wiznet_toe_type;

// Single chip on the board -> single object, like network.LAN.
static wiznet_toe_obj_t wiznet_toe_obj = { { &network_wiznet_toe_type } };

static void parse_ipv4_str(mp_obj_t obj, uint8_t out[4]) {
    size_t len;
    const char *s = mp_obj_str_get_data(obj, &len);
    int part = 0;
    unsigned val = 0;
    bool have_digit = false;
    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? s[i] : '.';
        if (c == '.') {
            if (part >= 4 || !have_digit || val > 255) {
                mp_raise_ValueError(MP_ERROR_TEXT("bad IPv4 string"));
            }
            out[part++] = (uint8_t)val;
            val = 0;
            have_digit = false;
        } else if (c >= '0' && c <= '9') {
            val = val * 10 + (unsigned)(c - '0');
            have_digit = true;
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("bad IPv4 string"));
        }
    }
    if (part != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad IPv4 string"));
    }
}

static mp_obj_t format_ipv4_str(const uint8_t ip[4]) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return mp_obj_new_str(buf, (size_t)n);
}

// WIZNET_TOE(spi=machine.SPI, cs=Pin, reset=Pin)
// WIZNET_TOE()  -- on a board whose definition supplies the wiring
//
// The wiring comes in the shape network.LAN takes for its SPI PHYs: an
// initialised machine.SPI object whose bus the chip hangs off, plus the pins
// this driver drives itself.  The SPI object's baudrate is the clock the
// chip is driven at.  Once wired, a call with no arguments returns the same
// object, so a script can reach a running interface without repeating the
// wiring.
//
// A board definition can supply the wiring instead, in the MICROPY_HW_WIZNET_*
// macros the WIZNET5K driver reads for the same purpose (see
// extmod/network_wiznet5k.c); a bare call then builds the SPI bus from them.
static mp_obj_t wiznet_toe_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    wiznet_toe_obj_t *self = &wiznet_toe_obj;
    if (n_args == 0 && n_kw == 0 && self->wired) {
        return MP_OBJ_FROM_PTR(self);
    }

    toe_spi_port_config_t wiring;
    uint32_t baudrate;
    #ifdef MICROPY_HW_WIZNET_SPI_ID
    if (n_args == 0 && n_kw == 0) {
        mp_obj_t sck = MP_OBJ_NEW_SMALL_INT(MICROPY_HW_WIZNET_SPI_SCK);
        mp_obj_t mosi = MP_OBJ_NEW_SMALL_INT(MICROPY_HW_WIZNET_SPI_MOSI);
        mp_obj_t miso = MP_OBJ_NEW_SMALL_INT(MICROPY_HW_WIZNET_SPI_MISO);
        mp_obj_t spi_args[] = {
            MP_OBJ_NEW_SMALL_INT(MICROPY_HW_WIZNET_SPI_ID),
            MP_OBJ_NEW_SMALL_INT(MICROPY_HW_WIZNET_SPI_BAUDRATE),
            MP_OBJ_NEW_QSTR(MP_QSTR_sck), mp_pin_make_new(NULL, 1, 0, &sck),
            MP_OBJ_NEW_QSTR(MP_QSTR_mosi), mp_pin_make_new(NULL, 1, 0, &mosi),
            MP_OBJ_NEW_QSTR(MP_QSTR_miso), mp_pin_make_new(NULL, 1, 0, &miso),
        };
        mp_obj_t spi = MP_OBJ_TYPE_GET_SLOT(&machine_spi_type, make_new)(&machine_spi_type, 2, 3, spi_args);
        wiring.host = machine_hw_spi_get_host(spi);
        wiring.cs_pin = MICROPY_HW_WIZNET_PIN_CS;
        wiring.reset_pin = MICROPY_HW_WIZNET_PIN_RST;
        baudrate = MICROPY_HW_WIZNET_SPI_BAUDRATE;
    } else
    #endif
    {
        enum { ARG_spi, ARG_cs, ARG_reset };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_spi, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
            { MP_QSTR_cs, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
            { MP_QSTR_reset, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
        };
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
        wiring.host = machine_hw_spi_get_host(args[ARG_spi].u_obj);
        wiring.cs_pin = machine_pin_get_id(args[ARG_cs].u_obj);
        wiring.reset_pin = machine_pin_get_id(args[ARG_reset].u_obj);
        baudrate = machine_hw_spi_get_baudrate(args[ARG_spi].u_obj);
    }

    bool rewired = wiring.host != self->wiring.host
        || wiring.cs_pin != self->wiring.cs_pin
        || wiring.reset_pin != self->wiring.reset_pin;
    if (rewired && toe_net_is_up()) {
        mp_raise_ValueError(MP_ERROR_TEXT("can't rewire while active"));
    }
    if (!toe_spi_port_set_clock(baudrate)) {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI baudrate out of range"));
    }
    self->wiring = wiring;
    self->wired = true;
    return MP_OBJ_FROM_PTR(self);
}

// active(True) brings the chip up with a placeholder 0.0.0.0 identity; the
// real address is applied by ifconfig(). That mirrors how network.LAN starts
// before DHCP has produced an address.
static mp_obj_t wiznet_toe_active(size_t n_args, const mp_obj_t *args) {
    wiznet_toe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args > 1) {
        if (mp_obj_is_true(args[1])) {
            if (!toe_net_is_up()) {
                uint8_t mac[6];
                toe_net_get_mac(mac);
                if (!toe_net_bringup(mac, &self->wiring)) {
                    mp_raise_OSError(MP_ENODEV);  // detail printed by bring-up
                }
            }
        } else {
            // Stop DHCP first: it holds a reserved hardware socket and a 1 s
            // timer that would otherwise keep poking a chip held in reset.
            toe_dhcp_stop();
            toe_net_shutdown();
        }
    }
    return mp_obj_new_bool(toe_net_is_up());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wiznet_toe_active_obj, 1, 2, wiznet_toe_active);

// ifconfig() -> (ip, subnet, gateway, dns), read straight from the chip.
// ifconfig((ip, subnet, gateway, dns)) writes it.
static mp_obj_t wiznet_toe_ifconfig(size_t n_args, const mp_obj_t *args) {
    if (!toe_net_is_up()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("interface not active"));
    }

    if (n_args == 1) {
        uint8_t ip[4], sn[4], gw[4], dns[4];
        toe_net_get_ipinfo(ip, sn, gw, dns);
        mp_obj_t items[4] = {
            format_ipv4_str(ip), format_ipv4_str(sn),
            format_ipv4_str(gw), format_ipv4_str(dns),
        };
        return mp_obj_new_tuple(4, items);
    }

    // ifconfig('dhcp') -- same spelling as upstream network_wiznet5k.c.
    if (args[1] == MP_OBJ_NEW_QSTR(MP_QSTR_dhcp)) {
        uint8_t ip[4], sn[4], gw[4], dns[4];
        toe_dhcp_result_t r = toe_dhcp_acquire(TOE_DHCP_TIMEOUT_MS, ip, sn, gw, dns);
        if (r != TOE_DHCP_OK) {
            // ioLibrary's DHCP writes SIPR/SUBR/GAR itself on success, so
            // there is nothing to roll back on failure.
            mp_raise_OSError(r == TOE_DHCP_NO_SOCKET ? MP_ENFILE : MP_ETIMEDOUT);
        }
        // DHCP does not carry the DNS server into the chip's own register,
        // so write the whole lease back through the normal path to keep the
        // chip and the shadow netif consistent.
        toe_net_set_ipinfo(ip, sn, gw, dns);
        mp_obj_t items[4] = {
            format_ipv4_str(ip), format_ipv4_str(sn),
            format_ipv4_str(gw), format_ipv4_str(dns),
        };
        return mp_obj_new_tuple(4, items);
    }

    mp_obj_t *items;
    size_t len;
    mp_obj_get_array(args[1], &len, &items);
    if (len != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("ifconfig needs (ip, subnet, gateway, dns)"));
    }
    uint8_t ip[4], sn[4], gw[4], dns[4];
    parse_ipv4_str(items[0], ip);
    parse_ipv4_str(items[1], sn);
    parse_ipv4_str(items[2], gw);
    parse_ipv4_str(items[3], dns);
    if (!toe_net_set_ipinfo(ip, sn, gw, dns)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wiznet_toe_ifconfig_obj, 1, 2, wiznet_toe_ifconfig);

// Up, PHY link present, and an address actually configured. The last check
// matters because active(True) deliberately starts at 0.0.0.0.
static mp_obj_t wiznet_toe_isconnected(mp_obj_t self_in) {
    if (!toe_net_is_up() || !toe_net_link_up()) {
        return mp_obj_new_bool(false);
    }
    uint8_t ip[4], sn[4], gw[4], dns[4];
    toe_net_get_ipinfo(ip, sn, gw, dns);
    bool has_ip = (ip[0] | ip[1] | ip[2] | ip[3]) != 0;
    return mp_obj_new_bool(has_ip);
}
static MP_DEFINE_CONST_FUN_OBJ_1(wiznet_toe_isconnected_obj, wiznet_toe_isconnected);

// Returns one of the ETH_* codes that network.LAN.status() already uses
// (modnetwork.h). Reusing them rather than inventing a private enum means a
// MACRAW script's status checks port to TOE unchanged, same as its socket
// code does.
static mp_obj_t wiznet_toe_status(size_t n_args, const mp_obj_t *args) {
    if (n_args > 1) {
        // Follow network.WLAN.status(param): reject anything unknown loudly
        // instead of silently returning the no-arg answer. Nothing queryable
        // exists here yet, so every param is unknown.
        mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
    }

    if (!toe_net_is_up()) {
        return MP_OBJ_NEW_SMALL_INT(ETH_STOPPED);
    }
    if (!toe_net_link_up()) {
        return MP_OBJ_NEW_SMALL_INT(ETH_DISCONNECTED);
    }
    uint8_t ip[4], sn[4], gw[4], dns[4];
    toe_net_get_ipinfo(ip, sn, gw, dns);
    bool has_ip = (ip[0] | ip[1] | ip[2] | ip[3]) != 0;
    // ETH_CONNECTED = link is up but no address yet (the state active(True)
    // leaves us in until ifconfig()/DHCP runs).
    return MP_OBJ_NEW_SMALL_INT(has_ip ? ETH_GOT_IP : ETH_CONNECTED);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wiznet_toe_status_obj, 1, 2, wiznet_toe_status);

static mp_obj_t wiznet_toe_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args != 1 && kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("either pos or kw args are allowed"));
    }

    if (kwargs->used != 0) {
        for (size_t i = 0; i < kwargs->alloc; i++) {
            if (!mp_map_slot_is_filled(kwargs, i)) {
                continue;
            }
            switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
                case MP_QSTR_mac: {
                    mp_buffer_info_t bufinfo;
                    mp_get_buffer_raise(kwargs->table[i].value, &bufinfo, MP_BUFFER_READ);
                    if (bufinfo.len != 6) {
                        mp_raise_ValueError(MP_ERROR_TEXT("mac must be 6 bytes"));
                    }
                    toe_net_set_mac(bufinfo.buf);
                    break;
                }
                case MP_QSTR_sock_kb: {
                    // Per-socket TX+RX buffer. Takes effect on the next
                    // active(True) -- wizchip_init() writes it and resets
                    // every socket, so it cannot be changed under a live one.
                    mp_int_t kb = mp_obj_get_int(kwargs->table[i].value);
                    if (kb <= 0 || !toe_net_set_bufkb((uint8_t)kb, (uint8_t)kb)) {
                        mp_raise_ValueError(MP_ERROR_TEXT("sock_kb must be 1/2/4/8/16"));
                    }
                    break;
                }
                case MP_QSTR_spi_hz: {
                    // The SPI object's baudrate seeds the clock; this lets a
                    // faster rate be swept at runtime without rebuilding the
                    // SPI object.
                    mp_int_t hz = mp_obj_get_int(kwargs->table[i].value);
                    if (hz <= 0 || !toe_spi_port_set_clock((uint32_t)hz)) {
                        mp_raise_ValueError(MP_ERROR_TEXT("bad spi_hz"));
                    }
                    break;
                }
                default:
                    mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
            }
        }
        return mp_const_none;
    }

    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac: {
            uint8_t mac[6];
            toe_net_get_mac(mac);
            return mp_obj_new_bytes(mac, sizeof(mac));
        }
        case MP_QSTR_sock_kb:
            return MP_OBJ_NEW_SMALL_INT(toe_net_get_rx_kb());
        case MP_QSTR_usable_socks:
            // Drops as sock_kb rises: the chip's 16KB per direction is fixed.
            return MP_OBJ_NEW_SMALL_INT(toe_net_usable_socks());
        case MP_QSTR_spi_hz:
            return mp_obj_new_int_from_uint(toe_spi_port_get_clock());
        case MP_QSTR_spi_hz_actual:
            // What the peripheral divider actually produced. Throughput
            // figures must quote this, not the requested rate.
            return mp_obj_new_int_from_uint(toe_spi_port_actual_clock());
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wiznet_toe_config_obj, 1, wiznet_toe_config);

static const mp_rom_map_elem_t wiznet_toe_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&wiznet_toe_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&wiznet_toe_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&wiznet_toe_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&wiznet_toe_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&wiznet_toe_config_obj) },

    // The same ETH_* codes status() returns. They also live at module level
    // (network.ETH_*), but that block is compiled only when WLAN is enabled
    // (modnetwork_globals.h), so mirror them here to keep this class usable
    // on its own -- and because upstream's stated direction is to move such
    // constants onto the interface class, as WLAN already did with IF_STA.
    { MP_ROM_QSTR(MP_QSTR_ETH_INITIALIZED), MP_ROM_INT(ETH_INITIALIZED) },
    { MP_ROM_QSTR(MP_QSTR_ETH_STARTED), MP_ROM_INT(ETH_STARTED) },
    { MP_ROM_QSTR(MP_QSTR_ETH_STOPPED), MP_ROM_INT(ETH_STOPPED) },
    { MP_ROM_QSTR(MP_QSTR_ETH_CONNECTED), MP_ROM_INT(ETH_CONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_ETH_DISCONNECTED), MP_ROM_INT(ETH_DISCONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_ETH_GOT_IP), MP_ROM_INT(ETH_GOT_IP) },
};
static MP_DEFINE_CONST_DICT(wiznet_toe_locals_dict, wiznet_toe_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    network_wiznet_toe_type,
    MP_QSTR_WIZNET_TOE,
    MP_TYPE_FLAG_NONE,
    make_new, wiznet_toe_make_new,
    locals_dict, &wiznet_toe_locals_dict
    );
