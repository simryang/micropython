// wiznet_toe bring-up self-test module.
// `import wiznettoe; wiznettoe.selftest()` -> W5500 VERSIONR register value.
// Real hardware value must be 0x04 (per datasheet); anything else means
// SPI wiring/reset is wrong. `wiznettoe.net_init(mac, ip, subnet, gw)` brings
// the chip's hardware TCP/IP up with a static identity (step 3). This module
// is bring-up scaffolding only — no socket-layer glue yet (that's step 4).

#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "toe_net_bringup.h"
#include "toe_spi_port.h"
#include "wiznet_toe/Ethernet/W5500/w5500.h"

static mp_obj_t wiznettoe_selftest(void) {
    if (!toe_spi_port_init()) {
        mp_raise_OSError(MP_ENODEV);  // detail already printed by toe_spi_port_init
    }
    toe_spi_port_reset();
    uint8_t version = getVERSIONR();
    return mp_obj_new_int(version);
}
static MP_DEFINE_CONST_FUN_OBJ_0(wiznettoe_selftest_obj, wiznettoe_selftest);

// Raw CS+SPI VERSIONR read bypassing wizchip_conf.c/w5500.c, to isolate
// glue bugs from wiring/electrical issues.
static mp_obj_t wiznettoe_diag(void) {
    if (!toe_spi_port_init()) {
        mp_raise_OSError(MP_ENODEV);
    }
    toe_spi_port_reset();
    uint8_t version = toe_spi_port_raw_versionr();
    return mp_obj_new_int(version);
}
static MP_DEFINE_CONST_FUN_OBJ_0(wiznettoe_diag_obj, wiznettoe_diag);

// Dumps PHY link state + every hardware socket's status/port registers, so a
// stuck accept()/connect() can be told apart from a dead link or a socket
// that never actually reached LISTEN.
static mp_obj_t wiznettoe_sockstat(void) {
    uint8_t phy = getPHYCFGR();
    printf("PHYCFGR=0x%02x (link=%s)\n", phy, (phy & PHYCFGR_LNK_ON) ? "UP" : "DOWN");
    for (int sn = 0; sn < 8; sn++) {
        printf("  sn%d: SR=0x%02x MR=0x%02x PORT=%u RX_RSR=%u\n",
            sn, getSn_SR(sn), getSn_MR(sn), getSn_PORT(sn), getSn_RX_RSR(sn));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(wiznettoe_sockstat_obj, wiznettoe_sockstat);

// "1.2.3.4" -> {1,2,3,4}. Manual parse (no sscanf dependency).
static void parse_ipv4(mp_obj_t obj, uint8_t out[4]) {
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

static mp_obj_t format_ipv4(const uint8_t ip[4]) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return mp_obj_new_str(buf, (size_t)n);
}

// net_init(mac, ip, subnet, gateway, dns=None)
// mac: 6-byte bytes-like object. ip/subnet/gateway/dns: "a.b.c.d" strings.
static mp_obj_t wiznettoe_net_init(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t mac_buf;
    mp_get_buffer_raise(args[0], &mac_buf, MP_BUFFER_READ);
    if (mac_buf.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("mac must be 6 bytes"));
    }

    wiz_NetInfo info = {0};
    memcpy(info.mac, mac_buf.buf, 6);
    parse_ipv4(args[1], info.ip);
    parse_ipv4(args[2], info.sn);
    parse_ipv4(args[3], info.gw);
    if (n_args >= 5 && args[4] != mp_const_none) {
        parse_ipv4(args[4], info.dns);
    }
    info.dhcp = NETINFO_STATIC;

    if (!toe_net_bringup(&info)) {
        mp_raise_OSError(MP_ENODEV);  // detail already printed by toe_net_bringup
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wiznettoe_net_init_obj, 4, 5, wiznettoe_net_init);

// Reads back (ip, subnet, gateway) as set on the chip, for step-3 verification.
static mp_obj_t wiznettoe_ifconfig(void) {
    wiz_NetInfo info = {0};
    wizchip_getnetinfo(&info);
    mp_obj_t items[3] = {
        format_ipv4(info.ip),
        format_ipv4(info.sn),
        format_ipv4(info.gw),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(wiznettoe_ifconfig_obj, wiznettoe_ifconfig);

static const mp_rom_map_elem_t wiznettoe_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wiznettoe) },
    { MP_ROM_QSTR(MP_QSTR_selftest), MP_ROM_PTR(&wiznettoe_selftest_obj) },
    { MP_ROM_QSTR(MP_QSTR_diag), MP_ROM_PTR(&wiznettoe_diag_obj) },
    { MP_ROM_QSTR(MP_QSTR_sockstat), MP_ROM_PTR(&wiznettoe_sockstat_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_init), MP_ROM_PTR(&wiznettoe_net_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&wiznettoe_ifconfig_obj) },
};
static MP_DEFINE_CONST_DICT(wiznettoe_module_globals, wiznettoe_module_globals_table);

const mp_obj_module_t wiznettoe_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&wiznettoe_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_wiznettoe, wiznettoe_module);
