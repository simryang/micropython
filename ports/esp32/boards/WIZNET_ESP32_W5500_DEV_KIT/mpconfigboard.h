#define MICROPY_HW_BOARD_NAME               "ESP32 W5500 Dev Kit"
#define MICROPY_HW_MCU_NAME                 "ESP32-S3"

// A CH340 on UART0 sits next to the native USB port.
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// The port's ESP32-S3 fallback puts I2C0 SCL on GPIO 9, which is the W5500's
// RESET on this board, so a pin-less machine.I2C(0) would reset the chip.
// Use the first free header pins instead (per the schematic; also free on
// the sister W6300 board).
#define MICROPY_HW_I2C0_SCL                 (5)
#define MICROPY_HW_I2C0_SDA                 (4)
#define MICROPY_HW_I2C1_SCL                 (7)
#define MICROPY_HW_I2C1_SDA                 (6)

// W5500 wiring, in the names the WIZNET5K driver reads for its board defaults
// (extmod/network_wiznet5k.c), so that network.WIZNET_TOE() needs no
// arguments.  SPI(1) is the FSPI bus and these are its IOMUX pins.  The
// chip's INT is on GPIO 14 and is not used by the TOE driver.
#define MICROPY_HW_WIZNET_SPI_ID            (1)
#define MICROPY_HW_WIZNET_SPI_BAUDRATE      (20 * 1000 * 1000)
#define MICROPY_HW_WIZNET_SPI_SCK           (12)
#define MICROPY_HW_WIZNET_SPI_MOSI          (11)
#define MICROPY_HW_WIZNET_SPI_MISO          (13)
#define MICROPY_HW_WIZNET_PIN_CS            (10)
#define MICROPY_HW_WIZNET_PIN_RST           (9)
