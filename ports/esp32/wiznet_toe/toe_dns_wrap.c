/*
 * Linker --wrap glue for name resolution on the W5500's own stack.
 *
 * socket.getaddrinfo() calls lwip_getaddrinfo() directly, and lwIP resolves
 * through its own stack, which has no route while the W5500 interface is up.
 * Until the socket module gains a resolver hook, these two wraps (see
 * esp32_common.cmake) answer with ioLibrary's DNS_run() on a borrowed
 * hardware socket while the interface is up, and hand everything else to
 * lwIP.  The socket calls that used to live next to them moved to
 * toe_socket_backend.c, the socket_backend_t implementation.
 */
#include <netdb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/sockets.h" // struct sockaddr_in, lwip_htons/htonl

#include "toe_dns.h"
#include "toe_net_bringup.h"

int __real_lwip_getaddrinfo(const char *nodename, const char *servname, const struct addrinfo *hints, struct addrinfo **res);
void __real_lwip_freeaddrinfo(struct addrinfo *ai);

// Results we hand out must be freed by us, not by lwIP's memp pool, so keep a
// small registry of the ones alive.  getaddrinfo results are short-lived here.
#define TOE_AI_MAX 4
#define TOE_AI_NAME_MAX 128

typedef struct {
    struct addrinfo ai;
    struct sockaddr_in sa;
    char canonname[TOE_AI_NAME_MAX];
} toe_addrinfo_t;

static toe_addrinfo_t *s_toe_ai[TOE_AI_MAX];

static bool toe_ai_track(toe_addrinfo_t *p) {
    for (int i = 0; i < TOE_AI_MAX; i++) {
        if (s_toe_ai[i] == NULL) {
            s_toe_ai[i] = p;
            return true;
        }
    }
    return false;
}

static bool toe_ai_untrack(const struct addrinfo *ai) {
    for (int i = 0; i < TOE_AI_MAX; i++) {
        if (s_toe_ai[i] != NULL && &s_toe_ai[i]->ai == ai) {
            free(s_toe_ai[i]);
            s_toe_ai[i] = NULL;
            return true;
        }
    }
    return false;
}

// "a.b.c.d" -> bytes; false if the string is not a dotted quad.
static bool toe_parse_dotted_quad(const char *s, uint8_t out[4]) {
    int part = 0;
    unsigned val = 0;
    bool digit = false;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned)(*p - '0');
            if (val > 255) {
                return false;
            }
            digit = true;
        } else if (*p == '.' || *p == '\0') {
            if (!digit || part >= 4) {
                return false;
            }
            out[part++] = (uint8_t)val;
            val = 0;
            digit = false;
            if (*p == '\0') {
                break;
            }
        } else {
            return false;
        }
    }
    return part == 4;
}

int __wrap_lwip_getaddrinfo(const char *nodename, const char *servname, const struct addrinfo *hints, struct addrinfo **res) {
    if (!toe_net_is_up() || nodename == NULL || res == NULL) {
        return __real_lwip_getaddrinfo(nodename, servname, hints, res);
    }

    uint8_t ip[4];
    if (!toe_parse_dotted_quad(nodename, ip)) {
        uint8_t server[4];
        if (!toe_dns_server(server)) {
            return EAI_FAIL;
        }
        if (!toe_dns_resolve(server, nodename, ip)) {
            return EAI_FAIL;
        }
    }

    toe_addrinfo_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return EAI_MEMORY;
    }
    if (!toe_ai_track(p)) {
        free(p);
        return EAI_MEMORY;
    }

    unsigned port = 0;
    if (servname != NULL) {
        for (const char *s = servname; *s >= '0' && *s <= '9'; s++) {
            port = port * 10 + (unsigned)(*s - '0');
        }
    }

    p->sa.sin_family = AF_INET;
    p->sa.sin_len = sizeof(p->sa);
    p->sa.sin_port = lwip_htons((uint16_t)port);
    p->sa.sin_addr.s_addr = lwip_htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3]);

    // modsocket.c reads ai_canonname back, so always provide one.
    size_t nl = strlen(nodename);
    if (nl >= sizeof(p->canonname)) {
        nl = sizeof(p->canonname) - 1;
    }
    memcpy(p->canonname, nodename, nl);
    p->canonname[nl] = '\0';

    p->ai.ai_family = AF_INET;
    p->ai.ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
    p->ai.ai_protocol = (hints && hints->ai_protocol) ? hints->ai_protocol : 0;
    p->ai.ai_addrlen = sizeof(p->sa);
    p->ai.ai_addr = (struct sockaddr *)&p->sa;
    p->ai.ai_canonname = p->canonname;
    p->ai.ai_next = NULL;

    *res = &p->ai;
    return 0;
}

void __wrap_lwip_freeaddrinfo(struct addrinfo *ai) {
    if (ai == NULL) {
        return;
    }
    if (toe_ai_untrack(ai)) {
        return; // one of ours
    }
    __real_lwip_freeaddrinfo(ai);
}
