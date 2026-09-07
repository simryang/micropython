// VFS integration for W5500 TOE hardware sockets -- see toe_vfs.h for why.
// Modelled on components/vfs/vfs_eventfd.c, the in-tree example of a
// non-socket driver that owns its own fds and joins select().

#include "toe_vfs.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>       // close() -- frees a permanent=false esp_vfs fd

#include "esp_vfs.h"

#include "wiznet_toe.h"

#define TOE_VFS_MAX_SOCK 8   // W5500 has 8 hardware sockets

static esp_vfs_id_t s_vfs_id = -1;
static int s_fd_of_sn[TOE_VFS_MAX_SOCK];   // global fd per sn, -1 when free

// One armed select() per (sn, waiter). Freed in end_select.
typedef struct toe_select_args {
    int sn;
    fd_set *readfds;      // NULL if this fd was not in the caller's read set
    fd_set *writefds;
    fd_set *errorfds;
    struct toe_select_args *next;
} toe_select_args_t;

int toe_vfs_sn_from_fd(int fd) {
    if (fd < 0) {
        return -1;
    }
    for (int sn = 0; sn < TOE_VFS_MAX_SOCK; sn++) {
        if (s_fd_of_sn[sn] == fd) {
            return sn;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- VFS ops
// These receive the LOCAL fd (== sn), because that is what we passed to
// esp_vfs_register_fd_with_local_fd().

static ssize_t toe_vfs_write(int sn, const void *data, size_t size) {
    int n = wiztoe_send(sn, data, size);
    return (n < 0) ? -1 : n;
}

static ssize_t toe_vfs_read(int sn, void *dst, size_t size) {
    int n = wiztoe_recv(sn, dst, size);
    return (n < 0) ? -1 : n;
}

static int toe_vfs_close(int sn) {
    // Reached only through the close() syscall (esp_vfs_close), which frees the
    // fd-table entry itself once we return. We close the hardware socket and
    // drop our own sn->fd shadow so the next socket() on this sn gets a fresh
    // fd. The listener re-arm case never routes through close() (see
    // toe_backend_close), so a socket reaching here is genuinely going away.
    int r = wiztoe_close(sn);
    if (sn >= 0 && sn < TOE_VFS_MAX_SOCK) {
        s_fd_of_sn[sn] = -1;
    }
    return (r < 0) ? -1 : 0;
}

// MicroPython polls with a zero timeout (modsocket.c's socket_stream_ioctl
// passes timeval{0,0}, and _socket_read_data() puts one such select() in
// front of every recv) and does its own retry loop, so a synchronous
// readiness snapshot is all that is needed here.
//
// Why the semaphore is signalled UNCONDITIONALLY below (2026-09-03):
// esp_vfs_select() (components/vfs/vfs.c, "ticks_to_wait = ... + 1") rounds
// a zero timeout up to ONE SCHEDULER TICK before it checks the semaphore, so
// a not-ready snapshot cost 10 ms at CONFIG_FREERTOS_HZ=100 -- measured
// 9.96 ms per empty poll(0) on a socket with zero traffic
// (D:\esp32s3-lab\debug\toe-select-tick\baseline_before_fix.log). lwIP
// sockets never see this because they take the socket_select() shortcut.
// Signalling regardless of readiness makes select(0) return at once with
// the (still accurate) fd bits; nothing in this firmware relies on
// esp_vfs_select() to BLOCK on a TOE fd: recv/accept/send wait inside
// wiznet_toe.c, connect is synchronous there, and MicroPython's poll()
// paces itself with mp_event_wait_ms(). An event-driven wake (W5500 INT
// pin -> esp_vfs_select_triggered_isr) is still the proper long-term fix.
static esp_err_t toe_vfs_start_select(int nfds, fd_set *readfds, fd_set *writefds,
    fd_set *exceptfds, esp_vfs_select_sem_t signal_sem, void **end_select_args) {

    toe_select_args_t *list = NULL;
    bool should_trigger = false;
    int max_sn = (nfds < TOE_VFS_MAX_SOCK) ? nfds : TOE_VFS_MAX_SOCK;

    for (int sn = 0; sn < max_sn; sn++) {
        bool want_rd = FD_ISSET(sn, readfds);
        bool want_wr = FD_ISSET(sn, writefds);
        bool want_er = FD_ISSET(sn, exceptfds);
        if (!want_rd && !want_wr && !want_er) {
            continue;
        }
        if (s_fd_of_sn[sn] < 0) {
            // Not one of ours (stale bit); make sure we don't report it ready.
            FD_CLR(sn, readfds);
            FD_CLR(sn, writefds);
            FD_CLR(sn, exceptfds);
            continue;
        }

        toe_select_args_t *args = calloc(1, sizeof(toe_select_args_t));
        if (!args) {
            *end_select_args = list;
            return ESP_ERR_NO_MEM;
        }
        args->sn = sn;
        args->readfds = want_rd ? readfds : NULL;
        args->writefds = want_wr ? writefds : NULL;
        args->errorfds = want_er ? exceptfds : NULL;
        args->next = list;
        list = args;

        int rd = 0, wr = 0, er = 0;
        wiztoe_poll(sn, &rd, &wr, &er);

        // Leave the bit set only where the socket is actually ready; VFS
        // returns these sets to the caller as-is.
        if (want_rd && rd) {
            should_trigger = true;
        } else {
            FD_CLR(sn, readfds);
        }
        if (want_wr && wr) {
            should_trigger = true;
        } else {
            FD_CLR(sn, writefds);
        }
        if (want_er && er) {
            should_trigger = true;
        } else {
            FD_CLR(sn, exceptfds);
        }
    }

    *end_select_args = list;
    (void)should_trigger;           // readiness is already in the fd sets
    esp_vfs_select_triggered(signal_sem);   // never let vfs.c sleep a tick
    return ESP_OK;
}

static esp_err_t toe_vfs_end_select(void *end_select_args) {
    toe_select_args_t *args = (toe_select_args_t *)end_select_args;
    while (args) {
        toe_select_args_t *next = args->next;
        free(args);
        args = next;
    }
    return ESP_OK;
}

// ------------------------------------------------------------ registration

bool toe_vfs_register(void) {
    if (s_vfs_id != -1) {
        return true;
    }
    for (int sn = 0; sn < TOE_VFS_MAX_SOCK; sn++) {
        s_fd_of_sn[sn] = -1;
    }

    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .write = &toe_vfs_write,
        .read = &toe_vfs_read,
        .close = &toe_vfs_close,
        #ifdef CONFIG_VFS_SUPPORT_SELECT
        .start_select = &toe_vfs_start_select,
        .end_select = &toe_vfs_end_select,
        #endif
    };
    esp_err_t err = esp_vfs_register_with_id(&vfs, NULL, &s_vfs_id);
    if (err != ESP_OK) {
        printf("wiznettoe: esp_vfs_register_with_id failed: %d\n", (int)err);
        s_vfs_id = -1;
        return false;
    }
    return true;
}

int toe_vfs_alloc_fd(int sn) {
    if (s_vfs_id == -1 || sn < 0 || sn >= TOE_VFS_MAX_SOCK) {
        return -1;
    }
    if (s_fd_of_sn[sn] >= 0) {
        return s_fd_of_sn[sn];   // already mapped
    }
    int fd = -1;
    // permanent=false is mandatory here: permanent=true marks the fd as a
    // socket fd, and esp_vfs_select() would then call our (absent)
    // socket_select without a NULL check. See toe_vfs.h.
    esp_err_t err = esp_vfs_register_fd_with_local_fd(s_vfs_id, sn, false, &fd);
    if (err != ESP_OK) {
        printf("wiznettoe: esp_vfs_register_fd_with_local_fd(sn=%d) failed: %d\n", sn, (int)err);
        return -1;
    }
    s_fd_of_sn[sn] = fd;
    return fd;
}

// Drop every fd this driver still has mapped, freeing its esp_vfs fd-table
// entry. Used at bring-up/teardown to clear fds left over from a previous
// session whose hardware sockets were already reset. close() is the only way
// to free a permanent=false, local-fd registration (esp_vfs_unregister_fd
// frees permanent==true entries only). close() -> esp_vfs_close routes back
// through toe_vfs_close (harmless: the hardware socket is already gone) and
// then frees the fd-table entry.
void toe_vfs_release_all(void) {
    if (s_vfs_id == -1) {
        return;
    }
    for (int sn = 0; sn < TOE_VFS_MAX_SOCK; sn++) {
        int fd = s_fd_of_sn[sn];
        if (fd >= 0) {
            s_fd_of_sn[sn] = -1;
            close(fd);
        }
    }
}
