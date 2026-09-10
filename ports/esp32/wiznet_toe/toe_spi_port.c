// wiznet_toe SPI/GPIO glue for the W5500 TOE port.
// Implements the cris/cs/spi(burst) callbacks that ioLibrary_Driver's
// wizchip_conf.c needs, on top of ESP-IDF's spi_master driver.
//
// The bus itself belongs to the machine.SPI object the user hands to
// network.WIZNET_TOE(); this file adds the chip as one more device on it and
// drives CS and RESET by hand.

#include "toe_spi_port.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "toe_iolibrary.h"

// Rate the driver runs at until the machine.SPI object's baudrate seeds it
// (network.WIZNET_TOE() does that on construction), and the rate the examples
// ask for.  Swept on real hardware 1..40 MHz: every rate read VERSIONR and the
// identity registers back correctly, so the original 1 MHz bring-up value was
// treating the wrong suspect -- the reset pulse width was what fixed the dead
// reads, not the clock. TCP receive throughput rises to ~10 MHz (77 -> 204 KB/s
// at a 2KB read) and is flat above it, so 20 MHz sits past the knee while
// keeping signal-integrity margin over the 40 MHz that also tested clean.
// Still runtime-settable via config(spi_hz=...).
#define TOE_SPI_CLOCK_HZ (20 * 1000 * 1000)

#define TOE_SPI_CLOCK_MIN_HZ (100 * 1000)
#define TOE_SPI_CLOCK_MAX_HZ (80 * 1000 * 1000)  // W5500 datasheet ceiling

// Longest single transaction the bus takes.  machine.SPI sets the bus up with
// max_transfer_sz at its default, which with DMA is SPI_MAX_DMA_LEN, while a
// burst can be a whole socket buffer -- up to 16 KB at sock_kb=16.  So bursts
// are carried in transactions of at most this size.  ioLibrary holds CS low
// around the entire burst and the W5500 keeps auto-incrementing the address
// for as long as CS stays low, so the chip sees one frame however many
// transactions carry it.
#define TOE_SPI_TRANSACTION_MAX SPI_MAX_DMA_LEN

static toe_spi_port_config_t s_wiring;
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
    gpio_set_level(s_wiring.cs_pin, 0);
}

static void toe_cs_deselect(void) {
    gpio_set_level(s_wiring.cs_pin, 1);
}

// A failed transfer must never pass for a good one. spi_device_polling_transmit
// rejects a transfer longer than the bus's max_transfer_sz with
// ESP_ERR_INVALID_ARG and moves no data at all; with the return value discarded
// (as it was until 2026-09-03) the caller's buffer simply kept its previous
// contents, so a 512 KB download completed with the right BYTE COUNT and
// entirely wrong bytes. That is what the long-unexplained "sock_kb=8 data
// corruption" was. The boundary measured exactly at SPI_MAX_DMA_LEN: a 4092-byte
// read is clean, 4093 corrupts. Evidence: buildC_burst_limit.log under
// D:/esp32s3-lab/debug/toe-select-tick. Bursts are now split at that limit
// (TOE_SPI_TRANSACTION_MAX), and this check makes any future overrun loud
// instead of silent.
static void toe_spi_checked(esp_err_t err, const char *what, uint32_t len) {
    if (err != ESP_OK) {
        printf("wiznettoe: SPI %s of %u bytes failed: %s\n",
            what, (unsigned)len, esp_err_to_name(err));
    }
}

static void toe_spi_read_burst(uint8_t *buf, uint16_t len) {
    while (len > 0) {
        uint16_t n = len > TOE_SPI_TRANSACTION_MAX ? TOE_SPI_TRANSACTION_MAX : len;
        spi_transaction_t t = {
            .length = (size_t)n * 8,
            .rxlength = (size_t)n * 8,
            .tx_buffer = NULL,
            .rx_buffer = buf,
        };
        toe_spi_checked(spi_device_polling_transmit(s_spi_dev, &t), "read burst", n);
        buf += n;
        len -= n;
    }
}

static void toe_spi_write_burst(const uint8_t *buf, uint16_t len) {
    while (len > 0) {
        uint16_t n = len > TOE_SPI_TRANSACTION_MAX ? TOE_SPI_TRANSACTION_MAX : len;
        spi_transaction_t t = {
            .length = (size_t)n * 8,
            .tx_buffer = buf,
            .rx_buffer = NULL,
        };
        toe_spi_checked(spi_device_polling_transmit(s_spi_dev, &t), "write burst", n);
        buf += n;
        len -= n;
    }
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

// Adds the chip to the wiring's bus at the given clock.  CS is driven by the
// wizchip cs_sel/cs_desel callbacks, not by the peripheral, because one
// W5500 frame spans several transactions.
static bool toe_spi_add_device(uint32_t hz) {
    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = (int)hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(s_wiring.host, &dev_conf, &s_spi_dev);
    if (err != ESP_OK) {
        printf("wiznettoe: spi_bus_add_device failed: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
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
    if (toe_spi_add_device(hz)) {
        s_clock_hz = hz;
        return true;
    }
    if (!toe_spi_add_device(s_clock_hz)) {
        printf("wiznettoe: SPI device lost while restoring %u Hz\n", (unsigned)s_clock_hz);
        s_initted = false;
    }
    return false;
}

void toe_spi_port_hold_reset(void) {
    if (!s_initted) {
        return;
    }
    gpio_set_level(s_wiring.reset_pin, 0);
}

void toe_spi_port_reset(void) {
    if (!s_initted) {
        return;
    }
    gpio_set_level(s_wiring.reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));  // generous margin over datasheet's 500ns min pulse
    gpio_set_level(s_wiring.reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // generous margin over datasheet's ~2ms PLL lock time
}

static bool toe_wiring_equal(const toe_spi_port_config_t *a, const toe_spi_port_config_t *b) {
    return a->host == b->host && a->cs_pin == b->cs_pin && a->reset_pin == b->reset_pin;
}

bool toe_spi_port_init(const toe_spi_port_config_t *wiring) {
    if (s_initted && !toe_wiring_equal(&s_wiring, wiring)) {
        // Rewired while the interface was down: give the old bus and pins
        // back before taking the new ones.
        spi_bus_remove_device(s_spi_dev);
        gpio_reset_pin(s_wiring.cs_pin);
        gpio_reset_pin(s_wiring.reset_pin);
        s_initted = false;
    }
    s_wiring = *wiring;

    // Claim the CS and reset pins on every bring-up, not only the first one:
    // another driver may have taken them while this interface was down.
    // network.LAN (esp_eth) adds its own SPI device with a hardware CS on the
    // same GPIO, which routes the pad to the SPI peripheral's CS signal, and
    // from then on the software CS used here (gpio_set_level) does not reach
    // the pad until gpio_config() routes it back to the GPIO output register.
    // Seen as TOE -> LAN -> TOE failing with "unexpected VERSIONR".
    gpio_config_t cs_rst_conf = {
        .pin_bit_mask = (1ULL << s_wiring.cs_pin) | (1ULL << s_wiring.reset_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_rst_conf);
    gpio_set_level(s_wiring.cs_pin, 1);
    gpio_set_level(s_wiring.reset_pin, 1);

    if (s_initted) {
        return true;  // the SPI device and ioLibrary callbacks are still in place
    }

    if (!toe_spi_add_device(s_clock_hz)) {
        return false;
    }

    reg_wizchip_cris_cbfunc(toe_cris_enter, toe_cris_exit);
    reg_wizchip_cs_cbfunc(toe_cs_select, toe_cs_deselect);
    reg_wizchip_spi_cbfunc(toe_spi_read_byte, toe_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(toe_spi_read_burst, toe_spi_write_burst);

    s_initted = true;
    return true;
}
