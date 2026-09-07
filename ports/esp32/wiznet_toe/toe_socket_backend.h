// W5500 hardware sockets as a socket backend (see ports/esp32/modsocket.h).
#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_SOCKET_BACKEND_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_SOCKET_BACKEND_H

#include "modsocket.h"

// The W5500's own TCP/IP stack.  toe_net_bringup.c makes it the default
// backend for new sockets while the interface is up.  Every descriptor it
// hands out is registered with the ESP-IDF VFS by toe_vfs.c, so select()
// works on it like on any other descriptor.
extern const socket_backend_t socket_backend_wiznet_toe;

#endif // MICROPY_INCLUDED_ESP32_WIZNET_TOE_SOCKET_BACKEND_H
