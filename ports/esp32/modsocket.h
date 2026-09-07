/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 WIZnet Co., LTD.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MICROPY_INCLUDED_ESP32_MODSOCKET_H
#define MICROPY_INCLUDED_ESP32_MODSOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "lwip/sockets.h"

// A socket backend is the TCP/IP stack that implements a socket's calls:
// lwIP, or a stack running inside a network interface chip.  The socket
// module drives every socket through its backend's table instead of calling
// lwIP directly, so such an interface can provide sockets as well.  The
// entries mirror the lwIP and POSIX socket calls: each one takes the
// descriptor returned by socket() and returns -1 with errno set on failure.
//
// A backend must register its descriptors with the ESP-IDF VFS, because the
// socket module relies on select() for polling and timeouts and select()
// dispatches on the descriptor.
typedef struct _socket_backend_t {
    int (*socket)(int domain, int type, int protocol);
    int (*close)(int s);
    int (*bind)(int s, const struct sockaddr *name, socklen_t namelen);
    int (*listen)(int s, int backlog);
    int (*accept)(int s, struct sockaddr *addr, socklen_t *addrlen);
    int (*connect)(int s, const struct sockaddr *name, socklen_t namelen);
    int (*setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
    int (*getsockopt)(int s, int level, int optname, void *optval, socklen_t *optlen);
    int (*fcntl)(int s, int cmd, int val);
    ssize_t (*write)(int s, const void *data, size_t size);
    ssize_t (*recvfrom)(int s, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
    ssize_t (*sendto)(int s, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen);
    // Join an IPv4 multicast group, as setsockopt(IP_ADD_MEMBERSHIP) does.
    // mreq holds the group address followed by the interface address, each 4
    // bytes in network byte order, like struct ip_mreq.
    int (*join_multicast_group)(int s, const uint8_t *mreq);
} socket_backend_t;

// lwIP, the backend of the WLAN, LAN and PPP interfaces.
extern const socket_backend_t socket_backend_lwip;

#endif // MICROPY_INCLUDED_ESP32_MODSOCKET_H
