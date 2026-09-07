// RTOS-coupled implementation of toe_port.h's plain-C prototypes.
// Kept in its own TU (separate from wiznet_toe.c) so wiznet_toe.c never has
// to include FreeRTOS/ESP headers alongside ioLibrary's socket.h.

#include "toe_port.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void toe_yield_1ms(void) {
    vTaskDelay(pdMS_TO_TICKS(1));
}

uint32_t toe_time_us(void) {
    return (uint32_t)esp_timer_get_time();
}
