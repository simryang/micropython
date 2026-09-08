include(boards/mpconfigboard_esp32s3_common.cmake)

# ESP32-S3-WROOM-1 module: 16 MiB flash, 8 MiB octal PSRAM.
list(APPEND SDKCONFIG_DEFAULTS
    boards/sdkconfig.240mhz
    boards/sdkconfig.spiram_oct
    boards/sdkconfig.flash_qio_80m
    boards/sdkconfig.csi
)

# The W5500 on this board is driven in hardware-socket (TOE) mode.
set(MICROPY_PY_NETWORK_WIZNET_TOE ON)
