// wiznet_toe SPI/GPIO glue for the W5500 TOE port.
// Implements the cris/cs/spi(burst) callbacks that ioLibrary_Driver's
// wizchip_conf.c needs, on top of ESP-IDF's spi_master driver.

#include "toe_spi_port.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wiznet_toe/Ethernet/wizchip_conf.h"

#define TOE_PIN_SCK  12
#define TOE_PIN_MOSI 11
#define TOE_PIN_MISO 13
#define TOE_PIN_CS   10
#define TOE_PIN_INT  14
#define TOE_PIN_RST  9

#define TOE_SPI_HOST SPI2_HOST
// Swept on real hardware 1..40 MHz: every rate read VERSIONR and the identity
// registers back correctly, so the original 1 MHz bring-up value was treating
// the wrong suspect -- the reset pulse width was what fixed the dead reads,
// not the clock. TCP receive throughput rises to ~10 MHz (77 -> 204 KB/s at a
// 2KB read) and is flat above it, so 20 MHz sits past the knee while keeping
// signal-integrity margin over the 40 MHz that also tested clean.
// Still runtime-settable via config(spi_hz=...).
#define TOE_SPI_CLOCK_HZ (20 * 1000 * 1000)

#define TOE_SPI_CLOCK_MIN_HZ (100 * 1000)
#define TOE_SPI_CLOCK_MAX_HZ (80 * 1000 * 1000)  // W5500 datasheet ceiling

static spi_device_handle_t s_spi_dev;
static bool s_initted = false;
static uint32_t s_clock_hz = TOE_SPI_CLOCK_HZ;

// Only suspends the scheduler (protects against other MicroPython threads
// touching the bus mid-transaction) -- deliberately does NOT disable
// interrupts, so the native-USB CDC ISR/task the REPL runs over keeps
// running even if an SPI transaction runs long.
static void toe_cris_enter(void) {
    vTaskSuspendAll();
}

static void toe_cris_exit(void) {
    xTaskResumeAll();
}

static void toe_cs_select(void) {
    gpio_set_level(TOE_PIN_CS, 0);
}

static void toe_cs_deselect(void) {
    gpio_set_level(TOE_PIN_CS, 1);
}

// A failed transfer must never pass for a good one. spi_device_polling_transmit
// rejects a transfer longer than the bus's max_transfer_sz with
// ESP_ERR_INVALID_ARG and moves no data at all; with the return value discarded
// (as it was until 2026-09-03) the caller's buffer simply kept its previous
// contents, so a 512 KB download completed with the right BYTE COUNT and
// entirely wrong bytes. That is what the long-unexplained "sock_kb=8 data
// corruption" was. The boundary measured exactly at SPI_MAX_DMA_LEN: a 4092-byte
// read is clean, 4093 corrupts. Evidence: buildC_burst_limit.log under
// D:/esp32s3-lab/debug/toe-select-tick. Bus setup now raises max_transfer_sz,
// and this check makes any future overrun loud instead of silent.
static void toe_spi_checked(esp_err_t err, const char *what, uint32_t len) {
    if (err != ESP_OK) {
        printf("wiznettoe: SPI %s of %u bytes failed: %s\n",
            what, (unsigned)len, esp_err_to_name(err));
    }
}

static void toe_spi_read_burst(uint8_t *buf, uint16_t len) {
    if (len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = (size_t)len * 8,
        .rxlength = (size_t)len * 8,
        .tx_buffer = NULL,
        .rx_buffer = buf,
    };
    toe_spi_checked(spi_device_polling_transmit(s_spi_dev, &t), "read burst", len);
}

static void toe_spi_write_burst(uint8_t *buf, uint16_t len) {
    if (len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = (size_t)len * 8,
        .tx_buffer = buf,
        .rx_buffer = NULL,
    };
    toe_spi_checked(spi_device_polling_transmit(s_spi_dev, &t), "write burst", len);
}

// WIZCHIP_READ() (w5500.c) always reads its single data byte through
// _read_byte(), even in burst mode -- only the address-frame write uses
// _write_burst there. Both byte- and burst-level callbacks must be
// registered or that call is a null function pointer (LoadStoreError).
static uint8_t toe_spi_read_byte(void) {
    uint8_t b = 0;
    spi_transaction_t t = {
        .length = 8,
        .rxlength = 8,
        .tx_buffer = NULL,
        .rx_buffer = &b,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
    return b;
}

static void toe_spi_write_byte(uint8_t wb) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &wb,
        .rx_buffer = NULL,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

uint32_t toe_spi_port_get_clock(void) {
    return s_clock_hz;
}

// Actual rate the SPI peripheral settled on. The ESP32 divides down from the
// 80 MHz source, so a requested 33 MHz really runs at 26.67 MHz -- throughput
// numbers must quote this, not the request.
uint32_t toe_spi_port_actual_clock(void) {
    if (!s_initted) {
        return 0;
    }
    int khz = 0;
    if (spi_device_get_actual_freq(s_spi_dev, &khz) != ESP_OK) {
        return 0;
    }
    return (uint32_t)khz * 1000u;
}

// Re-adds the SPI device at a new clock. Before init it just records the rate.
// On failure the previous rate is restored so the chip stays reachable --
// otherwise a bad rate would leave no device handle and no way back.
bool toe_spi_port_set_clock(uint32_t hz) {
    if (hz < TOE_SPI_CLOCK_MIN_HZ || hz > TOE_SPI_CLOCK_MAX_HZ) {
        return false;
    }
    if (!s_initted) {
        s_clock_hz = hz;
        return true;
    }
    if (spi_bus_remove_device(s_spi_dev) != ESP_OK) {
        return false;
    }

    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = (int)hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    if (spi_bus_add_device(TOE_SPI_HOST, &dev_conf, &s_spi_dev) == ESP_OK) {
        s_clock_hz = hz;
        return true;
    }

    dev_conf.clock_speed_hz = (int)s_clock_hz;
    if (spi_bus_add_device(TOE_SPI_HOST, &dev_conf, &s_spi_dev) != ESP_OK) {
        printf("wiznettoe: SPI device lost while restoring %u Hz\n", (unsigned)s_clock_hz);
        s_initted = false;
    }
    return false;
}

void toe_spi_port_hold_reset(void) {
    if (!s_initted) {
        return;
    }
    gpio_set_level(TOE_PIN_RST, 0);
}

void toe_spi_port_reset(void) {
    gpio_set_level(TOE_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));  // generous margin over datasheet's 500ns min pulse
    gpio_set_level(TOE_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // generous margin over datasheet's ~2ms PLL lock time
}

bool toe_spi_port_init(void) {
    if (s_initted) {
        return true;
    }

    gpio_config_t cs_rst_conf = {
        .pin_bit_mask = (1ULL << TOE_PIN_CS) | (1ULL << TOE_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_rst_conf);
    gpio_set_level(TOE_PIN_CS, 1);
    gpio_set_level(TOE_PIN_RST, 1);

    gpio_config_t int_conf = {
        .pin_bit_mask = (1ULL << TOE_PIN_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  // polled for now; switch to edge-irq once socket layer lands
    };
    gpio_config(&int_conf);

    spi_bus_config_t bus_conf = {
        .sclk_io_num = TOE_PIN_SCK,
        .mosi_io_num = TOE_PIN_MOSI,
        .miso_io_num = TOE_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // 0 would mean SPI_MAX_DMA_LEN (4092) with DMA enabled, and a socket
        // buffer can hand us a burst as large as the whole buffer -- up to
        // 16 KB at sock_kb=16. Anything past 4092 was rejected and silently
        // moved nothing (see toe_spi_checked). Size this to the largest burst
        // the chip's 16 KB RX/TX buffers can produce.
        .max_transfer_sz = 16 * 1024,
    };
    esp_err_t err = spi_bus_initialize(TOE_SPI_HOST, &bus_conf, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("wiznettoe: spi_bus_initialize failed: %s\n", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = (int)s_clock_hz,
        .mode = 0,
        .spics_io_num = -1,  // CS is driven manually via wizchip cs_sel/cs_desel callbacks
        .queue_size = 1,
    };
    err = spi_bus_add_device(TOE_SPI_HOST, &dev_conf, &s_spi_dev);
    if (err != ESP_OK) {
        printf("wiznettoe: spi_bus_add_device failed: %s\n", esp_err_to_name(err));
        spi_bus_free(TOE_SPI_HOST);
        return false;
    }

    reg_wizchip_cris_cbfunc(toe_cris_enter, toe_cris_exit);
    reg_wizchip_cs_cbfunc(toe_cs_select, toe_cs_deselect);
    reg_wizchip_spi_cbfunc(toe_spi_read_byte, toe_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(toe_spi_read_burst, toe_spi_write_burst);

    s_initted = true;
    return true;
}
