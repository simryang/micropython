// DHCP client glue -- see toe_dhcp.h for why this uses ioLibrary's DHCP_run()
// rather than the BSD-socket route wsm_driver's example took.
//
// Like wiznet_toe.c this TU includes the ioLibrary headers, so it is compiled
// with close=wiz_close and must not pull in POSIX/FreeRTOS socket headers.

#include "toe_dhcp.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "toe_port.h"
#include "wiznet_toe.h"

#include "toe_iolibrary.h"
#include "dhcp.h"

// ioLibrary's RIP_MSG is 236 bytes of BOOTP header + 312 bytes of options.
#define TOE_DHCP_BUF_SIZE 548

static uint8_t s_buf[TOE_DHCP_BUF_SIZE];
static int s_sn = -1;
static esp_timer_handle_t s_tick;

// ioLibrary requires DHCP_time_handler() once per second; it drives both the
// retransmit backoff and the lease-renewal clock.
static void toe_dhcp_tick(void *arg) {
    (void)arg;
    DHCP_time_handler();
}

static bool toe_dhcp_start_timer(void) {
    if (s_tick) {
        return true;
    }
    esp_timer_create_args_t args = {
        .callback = toe_dhcp_tick,
        .name = "wiztoe_dhcp",
    };
    if (esp_timer_create(&args, &s_tick) != ESP_OK) {
        s_tick = NULL;
        return false;
    }
    if (esp_timer_start_periodic(s_tick, 1000000) != ESP_OK) {
        esp_timer_delete(s_tick);
        s_tick = NULL;
        return false;
    }
    return true;
}

void toe_dhcp_stop(void) {
    if (s_tick) {
        esp_timer_stop(s_tick);
        esp_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_sn >= 0) {
        DHCP_stop();
        wiztoe_socket_release(s_sn);
        s_sn = -1;
    }
}

toe_dhcp_result_t toe_dhcp_acquire(uint32_t timeout_ms,
    uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4]) {

    // Start clean: a previous attempt may still hold a socket and a timer.
    toe_dhcp_stop();

    s_sn = wiztoe_socket_reserve();
    if (s_sn < 0) {
        return TOE_DHCP_NO_SOCKET;
    }

    DHCP_init((uint8_t)s_sn, s_buf);
    if (!toe_dhcp_start_timer()) {
        wiztoe_socket_release(s_sn);
        s_sn = -1;
        return TOE_DHCP_NO_SOCKET;
    }

    // Wall-clock deadline, not an iteration count: toe_yield_1ms() is
    // vTaskDelay(pdMS_TO_TICKS(1)), which at CONFIG_FREERTOS_HZ=100 is
    // vTaskDelay(0) -- a bare yield of microseconds.  Counting iterations as
    // milliseconds made this "15 s" window about 1.65 s (measured: every
    // failure at 1,652 ms), shorter than ioLibrary's first retransmit at
    // DHCP_WAIT_TIME = 10 s, so one late or lost packet failed the lease.
    uint32_t t0 = toe_time_us();
    toe_dhcp_result_t result = TOE_DHCP_TIMEOUT;

    while ((toe_time_us() - t0) < timeout_ms * 1000u) {
        uint8_t st = DHCP_run();
        if (st == DHCP_IP_LEASED || st == DHCP_IP_ASSIGN || st == DHCP_IP_CHANGED) {
            getIPfromDHCP(ip);
            getSNfromDHCP(sn);
            getGWfromDHCP(gw);
            getDNSfromDHCP(dns);
            result = TOE_DHCP_OK;
            break;
        }
        if (st == DHCP_FAILED) {
            result = TOE_DHCP_FAILED;
            break;
        }
        toe_yield_1ms();
    }

    if (result != TOE_DHCP_OK) {
        toe_dhcp_stop();
        return result;
    }

    // Keep the socket and the 1 s timer alive so DHCP_run() can renew later.
    return TOE_DHCP_OK;
}
