// VFS integration for W5500 TOE hardware sockets -- see toe_vfs.h for why.
// Modelled on components/vfs/vfs_eventfd.c, the in-tree example of a
// non-socket driver that owns its own fds and joins select().

#include "toe_vfs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>       // close() -- frees a permanent=false esp_vfs fd

#include "esp_vfs.h"

#include "wiznet_toe.h"

#define TOE_VFS_MAX_SOCK 8   // W5500 has 8 hardware sockets

// Descriptor slots. The slot index is the driver-local fd that ESP-IDF hands
// back to the VFS ops below (esp_vfs_register_fd_with_local_fd), so it is fixed
// for the life of the descriptor; the hardware socket behind it can change.
//
// 8 hardware sockets + 8 dormant listeners. A listener goes dormant only when
// accept() turned its socket into a connection and no other socket was free,
// so dormant listeners never outnumber live connections, which never
// outnumber hardware sockets.
#define TOE_VFS_MAX_FDS (2 * TOE_VFS_MAX_SOCK)

typedef struct {
    int fd;                 // global fd, -1 while the slot is free
    int sn;                 // hardware socket, -1 while a dormant listener
    bool dormant_listener;
    wiztoe_socket_settings_t listener_settings;   // valid while dormant_listener
} toe_vfs_slot_t;

static esp_vfs_id_t s_vfs_id = -1;
static toe_vfs_slot_t s_slot[TOE_VFS_MAX_FDS];

// One armed select() per (slot, waiter). Freed in end_select.
typedef struct toe_select_args {
    int slot;
    fd_set *readfds;      // NULL if this fd was not in the caller's read set
    fd_set *writefds;
    fd_set *errorfds;
    struct toe_select_args *next;
} toe_select_args_t;

static int toe_vfs_slot_from_fd(int fd) {
    if (fd < 0) {
        return -1;
    }
    for (int i = 0; i < TOE_VFS_MAX_FDS; i++) {
        if (s_slot[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static int toe_vfs_slot_from_sn(int sn) {
    for (int i = 0; i < TOE_VFS_MAX_FDS; i++) {
        if (s_slot[i].fd >= 0 && s_slot[i].sn == sn) {
            return i;
        }
    }
    return -1;
}

static void toe_vfs_slot_free(toe_vfs_slot_t *slot) {
    slot->fd = -1;
    slot->sn = -1;
    slot->dormant_listener = false;
}

int toe_vfs_sn_from_fd(int fd) {
    int i = toe_vfs_slot_from_fd(fd);
    return (i < 0) ? -1 : s_slot[i].sn;
}

bool toe_vfs_owns_fd(int fd) {
    return toe_vfs_slot_from_fd(fd) >= 0;
}

bool toe_vfs_is_dormant_listener(int fd) {
    int i = toe_vfs_slot_from_fd(fd);
    return i >= 0 && s_slot[i].dormant_listener;
}

wiztoe_socket_settings_t *toe_vfs_dormant_listener_settings(int fd) {
    int i = toe_vfs_slot_from_fd(fd);
    if (i < 0 || !s_slot[i].dormant_listener) {
        return NULL;
    }
    return &s_slot[i].listener_settings;
}

// Puts a listening hardware socket behind the slot, or parks the slot as a
// dormant listener when none is free. Returns the socket number or -1.
static int toe_vfs_slot_listen(toe_vfs_slot_t *slot, const wiztoe_socket_settings_t *settings) {
    int sn = wiztoe_listen_with(settings);
    if (sn >= 0) {
        slot->sn = sn;
        slot->dormant_listener = false;
        return sn;
    }
    slot->sn = -1;
    slot->dormant_listener = true;
    if (settings != &slot->listener_settings) {
        slot->listener_settings = *settings;
    }
    return -1;
}

int toe_vfs_relisten(int fd, const wiztoe_socket_settings_t *settings) {
    int i = toe_vfs_slot_from_fd(fd);
    if (i < 0 || settings == NULL) {
        return -1;
    }
    return toe_vfs_slot_listen(&s_slot[i], settings);
}

int toe_vfs_wake_dormant_listener(int fd) {
    int i = toe_vfs_slot_from_fd(fd);
    if (i < 0 || !s_slot[i].dormant_listener) {
        return -1;
    }
    return toe_vfs_slot_listen(&s_slot[i], &s_slot[i].listener_settings);
}

// ---------------------------------------------------------------- VFS ops
// These receive the LOCAL fd (== slot index), because that is what we passed
// to esp_vfs_register_fd_with_local_fd().

static int toe_vfs_live_sn(int slot) {
    if (slot < 0 || slot >= TOE_VFS_MAX_FDS || s_slot[slot].fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (s_slot[slot].sn < 0) {
        errno = ENOTCONN;   // dormant listener: nothing to read or write
        return -1;
    }
    return s_slot[slot].sn;
}

static ssize_t toe_vfs_write(int slot, const void *data, size_t size) {
    int sn = toe_vfs_live_sn(slot);
    if (sn < 0) {
        return -1;
    }
    int n = wiztoe_send(sn, data, size);
    return (n < 0) ? -1 : n;
}

static ssize_t toe_vfs_read(int slot, void *dst, size_t size) {
    int sn = toe_vfs_live_sn(slot);
    if (sn < 0) {
        return -1;
    }
    int n = wiztoe_recv(sn, dst, size);
    return (n < 0) ? -1 : n;
}

static int toe_vfs_close(int slot) {
    // Reached only through the close() syscall (esp_vfs_close), which frees the
    // fd-table entry itself once we return. Free the slot first so the next
    // socket() on this hardware socket gets a fresh fd, then close the
    // hardware socket (a dormant listener has none).
    if (slot < 0 || slot >= TOE_VFS_MAX_FDS) {
        return -1;
    }
    int sn = s_slot[slot].sn;
    toe_vfs_slot_free(&s_slot[slot]);
    if (sn < 0) {
        return 0;
    }
    return (wiztoe_close(sn) < 0) ? -1 : 0;
}

// MicroPython polls with a zero timeout (modsocket.c's socket_stream_ioctl
// passes timeval{0,0}, and _socket_read_data() puts one such select() in
// front of every recv) and does its own retry loop, so a synchronous
// readiness snapshot is all that is needed here.
//
// The fd sets hold LOCAL fds (slot indexes) while `nfds` is the caller's
// GLOBAL count, so it says nothing about which slots are set: walk them all.
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
    (void)nfds;

    toe_select_args_t *list = NULL;

    for (int slot = 0; slot < TOE_VFS_MAX_FDS; slot++) {
        bool want_rd = FD_ISSET(slot, readfds);
        bool want_wr = FD_ISSET(slot, writefds);
        bool want_er = FD_ISSET(slot, exceptfds);
        if (!want_rd && !want_wr && !want_er) {
            continue;
        }

        int sn = s_slot[slot].sn;
        if (s_slot[slot].fd >= 0 && s_slot[slot].dormant_listener) {
            // A poll()-driven server never calls accept() until we report the
            // listener readable, so this is where a dormant listener gets its
            // hardware socket back once the program has closed a connection.
            sn = toe_vfs_wake_dormant_listener(s_slot[slot].fd);
        }
        if (s_slot[slot].fd < 0 || sn < 0) {
            // Not one of ours (stale bit), or still dormant: not ready.
            FD_CLR(slot, readfds);
            FD_CLR(slot, writefds);
            FD_CLR(slot, exceptfds);
            continue;
        }

        toe_select_args_t *args = calloc(1, sizeof(toe_select_args_t));
        if (!args) {
            *end_select_args = list;
            return ESP_ERR_NO_MEM;
        }
        args->slot = slot;
        args->readfds = want_rd ? readfds : NULL;
        args->writefds = want_wr ? writefds : NULL;
        args->errorfds = want_er ? exceptfds : NULL;
        args->next = list;
        list = args;

        int rd = 0, wr = 0, er = 0;
        wiztoe_poll(sn, &rd, &wr, &er);

        // Leave the bit set only where the socket is actually ready; VFS
        // returns these sets to the caller as-is.
        if (!(want_rd && rd)) {
            FD_CLR(slot, readfds);
        }
        if (!(want_wr && wr)) {
            FD_CLR(slot, writefds);
        }
        if (!(want_er && er)) {
            FD_CLR(slot, exceptfds);
        }
    }

    *end_select_args = list;
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
    for (int i = 0; i < TOE_VFS_MAX_FDS; i++) {
        toe_vfs_slot_free(&s_slot[i]);
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
    if (toe_vfs_slot_from_sn(sn) >= 0) {
        // One descriptor per hardware socket; a second one is a caller bug.
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < TOE_VFS_MAX_FDS; i++) {
        if (s_slot[i].fd < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("wiznettoe: out of descriptor slots\n");
        return -1;
    }
    int fd = -1;
    // permanent=false is mandatory here: permanent=true marks the fd as a
    // socket fd, and esp_vfs_select() would then call our (absent)
    // socket_select without a NULL check. See toe_vfs.h.
    esp_err_t err = esp_vfs_register_fd_with_local_fd(s_vfs_id, slot, false, &fd);
    if (err != ESP_OK) {
        printf("wiznettoe: esp_vfs_register_fd_with_local_fd(sn=%d) failed: %d\n", sn, (int)err);
        return -1;
    }
    s_slot[slot].fd = fd;
    s_slot[slot].sn = sn;
    s_slot[slot].dormant_listener = false;
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
    for (int i = 0; i < TOE_VFS_MAX_FDS; i++) {
        int fd = s_slot[i].fd;
        if (fd >= 0) {
            close(fd);   // toe_vfs_close frees the slot
        }
    }
}
