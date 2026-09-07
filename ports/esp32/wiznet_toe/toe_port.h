// Neutral (plain-C) port helpers used by wiznet_toe.c.
//
// wiznet_toe.c must NOT include FreeRTOS/ESP/POSIX headers: it includes the
// ioLibrary "socket.h", whose socket()/close()/recv()/send() names would
// clash with POSIX/newlib declarations pulled in transitively. So the
// RTOS-coupled bits (yield, time) live in toe_port.c and are reached through
// these two plain prototypes.
//
// Ported verbatim from wsm_driver
// (D:\esp32s3-lab\wsm_driver\port\ioLibrary_Driver\inc\toe_port.h).

#ifndef TOE_PORT_H
#define TOE_PORT_H

#include <stdint.h>

// Block the calling task ~1 ms (FreeRTOS vTaskDelay), yielding to others.
void toe_yield_1ms(void);

// Free-running microsecond counter (esp_timer), used only for ephemeral-port
// randomization in wiztoe_connect().
uint32_t toe_time_us(void);

#endif // TOE_PORT_H
