// wiznet_toe SPI/GPIO glue for the W5500 TOE port.
//
// The SPI bus belongs to the machine.SPI object the user passes to
// network.WIZNET_TOE(); this file only adds the chip as a device on that
// bus and drives the CS and RESET pins itself.

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_SPI_PORT_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_SPI_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"

// How the chip is wired: the host of an initialised SPI bus and the two
// GPIOs the driver toggles.  CS is driven by software rather than by the
// SPI peripheral because ioLibrary holds it low across several transactions
// to make one W5500 frame.
typedef struct _toe_spi_port_config_t {
    spi_host_device_t host;
    int cs_pin;
    int reset_pin;
} toe_spi_port_config_t;

// Adds the chip as a device on the wiring's bus, sets up the CS/RESET GPIOs
// and registers the cris/cs/spi(burst) callbacks with ioLibrary_Driver.
// Calling it again with the same wiring is a no-op; with different wiring it
// moves the device.  Returns false (and logs the esp_err_t) on failure.
bool toe_spi_port_init(const toe_spi_port_config_t *wiring);

// Hardware-resets the W5500 via the RESET pin (active low, per datasheet).
void toe_spi_port_reset(void);

// Holds the W5500 in reset (RESET low) so it stops driving the link.
// Used by active(False); the next toe_spi_port_reset() releases it.
void toe_spi_port_hold_reset(void);

// SPI clock, in Hz.  Seeded from the machine.SPI object's baudrate when the
// interface object is constructed; set_clock() re-adds the SPI device so the
// rate can be swept at runtime (config(spi_hz=...)) without rebuilding the
// SPI object.  Returns false (and keeps the old rate) if the value is out of
// range or the device could not be re-added.
uint32_t toe_spi_port_get_clock(void);
bool toe_spi_port_set_clock(uint32_t hz);

// Rate the peripheral actually settled on after its divider; 0 before init.
uint32_t toe_spi_port_actual_clock(void);

#endif
