// wiznet_toe SPI/GPIO glue for the W5500 TOE port.
// Pins are confirmed against the real ESP32-W5500-Dev-V1 schematic
// (same pins already verified working in the MACRAW guide):
// SCK=12 MOSI=11 MISO=13 CS=10 INT=14 RESET=9

#ifndef MICROPY_INCLUDED_ESP32_WIZNET_TOE_SPI_PORT_H
#define MICROPY_INCLUDED_ESP32_WIZNET_TOE_SPI_PORT_H

#include <stdbool.h>
#include <stdint.h>

// Initializes SPI bus + CS/RESET/INT GPIOs and registers the wizchip
// cris/cs/spi(burst) callbacks with ioLibrary_Driver. Safe to call once.
// Returns false (and logs the esp_err_t) if the SPI bus/device init fails.
bool toe_spi_port_init(void);

// Hardware-resets the W5500 via the RESET pin (active low, per datasheet).
void toe_spi_port_reset(void);

// Holds the W5500 in reset (RESET low) so it stops driving the link.
// Used by active(False); the next toe_spi_port_reset() releases it.
void toe_spi_port_hold_reset(void);

// SPI clock, in Hz. The compiled-in default is the conservative bring-up rate;
// set_clock() re-adds the SPI device so the rate can be swept at runtime
// without reflashing. Returns false (and keeps the old rate) if the value is
// out of range or the device could not be re-added.
uint32_t toe_spi_port_get_clock(void);
bool toe_spi_port_set_clock(uint32_t hz);

// Rate the peripheral actually settled on after its divider; 0 before init.
uint32_t toe_spi_port_actual_clock(void);

#endif
