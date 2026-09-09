// The port's one way into ioLibrary_Driver's headers (lib/wiznet5k).
//
// ioLibrary's W5500 register map defines MR (the chip's mode register) and
// SOCK_STREAM/SOCK_DGRAM (its socket types).  Those names are also the Xtensa
// special register MR that ESP-IDF's FreeRTOS port brings in, and lwIP's
// socket type constants, so a translation unit that sees both gets a
// "redefined" warning for each.  The port never uses the Xtensa or lwIP
// meaning in a unit that talks to the chip, so drop them here and let
// ioLibrary's definitions stand.  Include this after every ESP-IDF and lwIP
// header, and include ioLibrary's other headers (socket.h, dhcp.h, dns.h)
// after it.

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_IOLIBRARY_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_IOLIBRARY_H

#ifdef MR
#undef MR
#endif
#ifdef SOCK_STREAM
#undef SOCK_STREAM
#endif
#ifdef SOCK_DGRAM
#undef SOCK_DGRAM
#endif

#include "wizchip_conf.h"

#endif
