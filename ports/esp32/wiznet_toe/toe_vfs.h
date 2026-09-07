// VFS integration for W5500 TOE hardware sockets.
//
// Why this exists (see docs/ARCHITECTURE_TOE.md):
// wsm_driver hands out TOE fds as `sn + LWIP_SOCKET_OFFSET`, which lands them
// inside lwIP's own reserved fd window [LWIP_SOCKET_OFFSET, MAX_FDS) --
// literally the same numbers real lwIP sockets use. That forces the
// "TOE owns every socket in the build" assumption, and it breaks select():
// ESP-IDF's VFS looks fd 54 up in s_fd_table, finds lwIP's socket driver, and
// hands it to lwip_select(), which has no such socket -> EIO.
//
// Instead we register TOE as an ordinary NON-socket VFS driver (the same way
// components/vfs/vfs_eventfd.c does) and let VFS allocate our fds from the
// bottom-up pool. Two things fall out of that:
//   1. select()/poll() work natively, through .start_select/.end_select --
//      no -Wl,--wrap=select needed.
//   2. TOE fds can no longer collide with lwIP socket fds, so the socket
//      backend (toe_socket_backend.c) maps a descriptor straight to its
//      hardware socket.
//
// NOTE: we must register with permanent=false. In ESP-IDF, permanent=true
// means "this is a socket fd", and esp_vfs_select() then dereferences the
// driver's socket_select/get_socket_select_semaphore with no NULL guard
// (vfs.c:1503,1516,1521) -- registering as a second socket driver would crash
// inside select(). Only one socket driver (lwIP) can exist.

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_VFS_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_VFS_H

#include <stdbool.h>

// Registers the TOE VFS driver. Idempotent; safe to call from net_init().
// Returns false (and logs) if registration fails.
bool toe_vfs_register(void);

// Claims a global fd for hardware socket `sn`. Returns the fd, or -1.
int toe_vfs_alloc_fd(int sn);

// Maps a global fd back to its hardware socket number, or -1 if this fd is
// not ours (i.e. it belongs to lwIP or another driver).
int toe_vfs_sn_from_fd(int fd);

// Releases every fd we hold, freeing each one's esp_vfs fd-table entry. Pairs
// with wiztoe_reset_sockets(): a chip reset invalidates all hardware sockets,
// so the fds mapping to them must go too or they leak out of the VFS fd table.
// (Per-fd release lives in the close() path -- see toe_vfs.c toe_vfs_close and
// toe_socket_backend.c toe_backend_close -- because a permanent=false registration
// can only be freed by close(), not esp_vfs_unregister_fd.)
void toe_vfs_release_all(void);

#endif
