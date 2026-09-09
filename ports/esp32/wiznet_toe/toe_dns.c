// DNS resolver glue -- see toe_dns.h.
// Compiled with close=wiz_close like the other ioLibrary-facing TUs.

#include "toe_dns.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "wiznet_toe.h"

#include "toe_iolibrary.h"
#include "dns.h"

static uint8_t s_buf[MAX_DNS_BUF_SIZE];
static esp_timer_handle_t s_tick;

// ioLibrary needs DNS_time_handler() once per second while a query is in
// flight; it drives the retransmit/timeout logic inside DNS_run().
static void toe_dns_tick(void *arg) {
    (void)arg;
    DNS_time_handler();
}

static bool toe_dns_timer_start(void) {
    if (!s_tick) {
        esp_timer_create_args_t args = {
            .callback = toe_dns_tick,
            .name = "wiztoe_dns",
        };
        if (esp_timer_create(&args, &s_tick) != ESP_OK) {
            s_tick = NULL;
            return false;
        }
    }
    return esp_timer_start_periodic(s_tick, 1000000) == ESP_OK;
}

static void toe_dns_timer_stop(void) {
    if (s_tick) {
        esp_timer_stop(s_tick);
    }
}

bool toe_dns_server(uint8_t server[4]) {
    wiz_NetInfo info = {0};
    wizchip_getnetinfo(&info);
    memcpy(server, info.dns, 4);
    return (server[0] | server[1] | server[2] | server[3]) != 0;
}

bool toe_dns_resolve(const uint8_t server[4], const char *name, uint8_t out_ip[4]) {
    if ((server[0] | server[1] | server[2] | server[3]) == 0) {
        return false;
    }

    // Borrow a hardware socket only for the duration of the query: there are
    // just 8, and DNS_run() closes the socket itself when it finishes.
    int sn = wiztoe_socket_reserve();
    if (sn < 0) {
        return false;
    }

    DNS_init((uint8_t)sn, s_buf);
    if (!toe_dns_timer_start()) {
        wiztoe_socket_release(sn);
        return false;
    }

    int8_t r = DNS_run((uint8_t *)server, (uint8_t *)name, out_ip);

    toe_dns_timer_stop();
    wiztoe_socket_release(sn);

    return r == 1;   // 1 = resolved, 0 = timeout, -1 = error
}
