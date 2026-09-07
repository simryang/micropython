# This is the common ESP-IDF "main component" CMakeLists.txt contents for MicroPython.
#
# This file is included directly from a main_${IDF_TARGET}/CMakeLists.txt file
# (or included from an out-of-tree main component CMakeLists.txt for out-of-tree
# builds.)

# Set location of base MicroPython directory.
if(NOT MICROPY_DIR)
    get_filename_component(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
endif()

# Set location of the ESP32 port directory.
if(NOT MICROPY_PORT_DIR)
    get_filename_component(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32 ABSOLUTE)
endif()

# RISC-V specific inclusions
if(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND MICROPY_SOURCE_LIB
        ${MICROPY_DIR}/shared/runtime/gchelper_native.c
        ${MICROPY_DIR}/shared/runtime/gchelper_rv32i.s
    )
endif()

if(NOT DEFINED MICROPY_PY_TINYUSB)
    if(CONFIG_IDF_TARGET_ESP32S2 OR CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4)
        set(MICROPY_PY_TINYUSB ON)
    endif()
endif()

# Enable error text compression by default.
if(NOT MICROPY_ROM_TEXT_COMPRESSION)
    set(MICROPY_ROM_TEXT_COMPRESSION ON)
endif()

# Include core source components.
include(${MICROPY_DIR}/py/py.cmake)

# CMAKE_BUILD_EARLY_EXPANSION is set during the component-discovery phase of
# `idf.py build`, so none of the extmod/usermod (and in reality, most of the
# micropython) rules need to happen. Specifically, you cannot invoke add_library.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Enable extmod components that will be configured by extmod.cmake.
    # A board may also have enabled additional components.
    if (NOT DEFINED MICROPY_PY_BTREE)
        set(MICROPY_PY_BTREE ON)
    endif()

    include(${MICROPY_DIR}/py/usermod.cmake)
    include(${MICROPY_DIR}/extmod/extmod.cmake)
endif()

list(APPEND MICROPY_QSTRDEFS_PORT
    ${MICROPY_PORT_DIR}/qstrdefsport.h
)

list(APPEND MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/readline/readline.c
    ${MICROPY_DIR}/shared/netutils/netutils.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
    ${MICROPY_DIR}/shared/runtime/interrupt_char.c
    ${MICROPY_DIR}/shared/runtime/mpirq.c
    ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/runtime/pyexec.c
)

list(APPEND MICROPY_SOURCE_LIB
    ${MICROPY_DIR}/lib/littlefs/lfs1.c
    ${MICROPY_DIR}/lib/littlefs/lfs1_util.c
    ${MICROPY_DIR}/lib/littlefs/lfs2.c
    ${MICROPY_DIR}/lib/littlefs/lfs2_util.c
    ${MICROPY_DIR}/lib/mbedtls_errors/esp32_mbedtls_errors.c
    ${MICROPY_DIR}/lib/oofatfs/ff.c
    ${MICROPY_DIR}/lib/oofatfs/ffunicode.c
)

list(APPEND MICROPY_SOURCE_DRIVERS
    ${MICROPY_DIR}/drivers/bus/softspi.c
    ${MICROPY_DIR}/drivers/dht/dht.c
)

if(MICROPY_PY_TINYUSB)
    string(TOUPPER OPT_MCU_${IDF_TARGET} tusb_mcu)

    list(APPEND MICROPY_DEF_TINYUSB
        CFG_TUSB_MCU=${tusb_mcu}
    )

    list(APPEND MICROPY_SOURCE_TINYUSB
        ${MICROPY_DIR}/shared/tinyusb/mp_usbd.c
        ${MICROPY_DIR}/shared/tinyusb/mp_usbd_cdc.c
        ${MICROPY_DIR}/shared/tinyusb/mp_usbd_descriptor.c
        ${MICROPY_DIR}/shared/tinyusb/mp_usbd_runtime.c
    )

    list(APPEND MICROPY_INC_TINYUSB
        ${MICROPY_DIR}/shared/tinyusb/
    )

    # Build the Espressif tinyusb component with MicroPython shared/tinyusb/tusb_config.h
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    target_include_directories(${tusb_lib} PRIVATE
        ${MICROPY_DIR}/shared/tinyusb
        ${MICROPY_DIR}
        ${MICROPY_PORT_DIR}
        ${MICROPY_BOARD_DIR})
endif()

list(APPEND MICROPY_SOURCE_PORT
    panichandler.c
    adc.c
    main.c
    ppp_set_auth.c
    uart.c
    usb.c
    usb_serial_jtag.c
    gccollect.c
    mphalport.c
    fatfs_port.c
    help.c
    machine_bitstream.c
    machine_timer.c
    machine_pin.c
    machine_touchpad.c
    machine_dac.c
    machine_i2c.c
    network_common.c
    network_lan.c
    network_ppp.c
    network_wlan.c
    mpnimbleport.c
    modsocket.c
    lwip_patch.c
    modesp.c
    esp32_nvs.c
    esp32_partition.c
    esp32_pcnt.c
    esp32_rmt.c
    esp32_ulp.c
    esp32_ldo.c
    modesp32.c
    machine_hw_spi.c
    mpthreadport.c
    machine_rtc.c
    machine_sdcard.c
    modespnow.c

    # wiznet_toe: vendored ioLibrary_Driver core for W5500 TOE (hardware TCP offload).
    wiznet_toe/Ethernet/socket.c
    wiznet_toe/Ethernet/wizchip_conf.c
    wiznet_toe/Ethernet/W5500/w5500.c
    # Step 2: SPI/GPIO bring-up glue + selftest module.
    wiznet_toe/toe_spi_port.c
    # Step 3: static IP bring-up (wizchip_init/setnetinfo + shadow esp_netif).
    wiznet_toe/toe_net_bringup.c
    wiznet_toe/modwiznettoe.c
)
list(TRANSFORM MICROPY_SOURCE_PORT PREPEND ${MICROPY_PORT_DIR}/)

# wiznet_toe's close=wiz_close rename and the step-4 socket wrap are
# registered further down, after idf_component_register()/MICROPY_TARGET
# exist -- set_source_files_properties() errors as "not scriptable" if called
# during ESP-IDF's early component-requirements discovery pass, which runs
# this file's early portion before any target exists.
list(APPEND MICROPY_SOURCE_PORT ${CMAKE_BINARY_DIR}/pins.c)

list(APPEND MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_USERMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_LIB}
    ${MICROPY_SOURCE_PORT}
    ${MICROPY_SOURCE_BOARD}
    ${MICROPY_SOURCE_TINYUSB}
)

list(APPEND IDF_COMPONENTS
    app_update
    bootloader_support
    bt
    driver
    esp_adc
    esp_app_format
    esp_mm
    esp_common
    esp_eth
    esp_event
    esp_hw_support
    esp_netif
    esp_partition
    esp_pm
    esp_psram
    esp_ringbuf
    esp_rom
    esp_system
    esp_timer
    esp_wifi
    freertos
    hal
    heap
    log
    lwip
    mbedtls
    newlib
    nvs_flash
    sdmmc
    soc
    spi_flash
    ulp
    usb
    vfs
)

if($ENV{IDF_VERSION} VERSION_GREATER_EQUAL "5.4")
    list(APPEND IDF_COMPONENTS
        esp_driver_touch_sens)
endif()

# Provide the default LD fragment if not set
if (MICROPY_USER_LDFRAGMENTS)
    set(MICROPY_LDFRAGMENTS ${MICROPY_USER_LDFRAGMENTS})
endif()

if (UPDATE_SUBMODULES)
    # ESP-IDF checks if some paths exist before CMake does. Some paths don't
    # yet exist if this is an UPDATE_SUBMODULES pass on a brand new checkout, so remove
    # any path which might not exist yet. A "real" build will not set UPDATE_SUBMODULES.
    unset(MICROPY_SOURCE_TINYUSB)
    unset(MICROPY_SOURCE_EXTMOD)
    unset(MICROPY_SOURCE_LIB)
    unset(MICROPY_INC_TINYUSB)
    unset(MICROPY_INC_CORE)
endif()

# Register the main IDF component.
idf_component_register(
    SRCS
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_LIB}
        ${MICROPY_SOURCE_DRIVERS}
        ${MICROPY_SOURCE_PORT}
        ${MICROPY_SOURCE_BOARD}
        ${MICROPY_SOURCE_TINYUSB}
    INCLUDE_DIRS
        ${MICROPY_INC_CORE}
        ${MICROPY_INC_USERMOD}
        ${MICROPY_INC_TINYUSB}
        ${MICROPY_PORT_DIR}
        ${MICROPY_BOARD_DIR}
        ${CMAKE_BINARY_DIR}
    LDFRAGMENTS
        ${MICROPY_LDFRAGMENTS}
    REQUIRES
        ${IDF_COMPONENTS}
)

# Set the MicroPython target as the current (main) IDF component target.
set(MICROPY_TARGET ${COMPONENT_TARGET})

# Define mpy-cross flags, for use with frozen code.
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    set(MICROPY_CROSS_FLAGS -march=xtensawin)
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    if (CONFIG_IDF_TARGET_ESP32P4)
        set(MICROPY_CROSS_FLAGS "-march=rv32imc -march-flags=zcmp")
    else()
        set(MICROPY_CROSS_FLAGS -march=rv32imc)
    endif()
endif()

# Set compile options for this port.
target_compile_definitions(${MICROPY_TARGET} PUBLIC
    ${MICROPY_DEF_COMPONENT}
    ${MICROPY_DEF_CORE}
    ${MICROPY_DEF_BOARD}
    ${MICROPY_DEF_TINYUSB}
    MICROPY_VFS_FAT=1
    MICROPY_VFS_LFS2=1
    FFCONF_H=\"${MICROPY_OOFATFS_DIR}/ffconf.h\"
    LFS1_NO_MALLOC LFS1_NO_DEBUG LFS1_NO_WARN LFS1_NO_ERROR LFS1_NO_ASSERT
    LFS2_NO_MALLOC LFS2_NO_DEBUG LFS2_NO_WARN LFS2_NO_ERROR LFS2_NO_ASSERT
)

# Disable some warnings to keep the build output clean.
target_compile_options(${MICROPY_TARGET} PUBLIC
    ${MICROPY_COMPILE_COMPONENT}
    -Wno-clobbered
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
)

# User C modules don't pick up certain compile options set by the IDF, most
# importantly the optimisation level.  So set them here.
idf_build_get_property(idf_compile_options COMPILE_OPTIONS)
target_compile_options(usermod INTERFACE ${idf_compile_options})

# Additional include directories needed for private NimBLE headers.
target_include_directories(${MICROPY_TARGET} PUBLIC
    ${IDF_PATH}/components/bt/host/nimble/nimble
)

# wiznet_toe: ioLibrary_Driver headers include each other with bare relative
# paths (e.g. W5500/w5500.h includes "wizchip_conf.h"), so the Ethernet/ dir
# itself must be on the include path. _WIZCHIP_=5500 selects the W5500 branch
# in wizchip_conf.h at compile time (step 1: build verification only).
target_include_directories(${MICROPY_TARGET} PUBLIC
    ${MICROPY_PORT_DIR}/wiznet_toe/Ethernet
    # DHCP/DNS sources include each other's headers by bare name too.
    ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DHCP
    ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DNS
)
target_compile_definitions(${MICROPY_TARGET} PUBLIC
    _WIZCHIP_=5500
)

# wiznet_toe: ioLibrary's Ethernet/socket.c defines a global close(uint8_t)
# that would hijack newlib's POSIX close() and break the VFS -> lwip_close
# path the moment it's actually reachable (see the
# MICROPY_WIZNET_TOE_SOCKET_WRAP block below -- with -Wl,--gc-sections this
# symbol is otherwise dropped as dead code, which is why steps 1-3 linked
# cleanly without this rename). Rename close -> wiz_close CONSISTENTLY across
# every TU that calls ioLibrary's close() (they call each other's close), so
# ioLibrary stays internally consistent while POSIX close() is left to
# newlib/VFS. Applied unconditionally (harmless when unreachable). Ported
# from wsm_driver's CMakeLists.txt.
set_source_files_properties(
    ${MICROPY_PORT_DIR}/wiznet_toe/Ethernet/socket.c
    ${MICROPY_PORT_DIR}/wiznet_toe/Ethernet/wizchip_conf.c
    ${MICROPY_PORT_DIR}/wiznet_toe/Ethernet/W5500/w5500.c
    PROPERTIES COMPILE_DEFINITIONS "close=wiz_close"
)

# Step 4: socket-layer wrap (wsm_driver's wiznet_toe.c + wiztoe_wrap.c,
# vendored verbatim). OFF by default: -Wl,--wrap=lwip_* is a GLOBAL linker
# rewrite that routes every socket() call in the WHOLE FIRMWARE (WiFi
# included) to the W5500 TOE hardware sockets. wsm_driver's own design
# assumes TOE owns every socket in the build; that's wrong for a MicroPython
# firmware that also wants WiFi/MACRAW sockets to keep working. Build a
# dedicated TOE-testing firmware with
# `idf.py -D MICROPY_WIZNET_TOE_SOCKET_WRAP=ON build` to turn this on.
# TODO(step 4+): runtime fd-range dispatch (fall through to __real_lwip_* for
# non-TOE fds) so one firmware can support both at once -- see
# D:\esp32s3-lab\docs\ARCHITECTURE_TOE.md.
option(MICROPY_WIZNET_TOE_SOCKET_WRAP
    "Route lwIP BSD sockets to WIZnet TOE hardware sockets. Exclusive with WiFi/MACRAW sockets in the same binary."
    OFF)

if(MICROPY_WIZNET_TOE_SOCKET_WRAP)
    target_sources(${MICROPY_TARGET} PRIVATE
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_port.c
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_vfs.c
        ${MICROPY_PORT_DIR}/wiznet_toe/wiznet_toe.c
        ${MICROPY_PORT_DIR}/wiznet_toe/wiztoe_wrap.c
        # Step 5: network.WIZNET_TOE Python face.
        ${MICROPY_PORT_DIR}/wiznet_toe/network_wiznet_toe.c
        # Step 6: DHCP client (ioLibrary DHCP_run on a reserved hw socket).
        ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DHCP/dhcp.c
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_dhcp.c
        # Step 7: DNS resolver behind a wrapped getaddrinfo().
        ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DNS/dns.c
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_dns.c
    )
    # network_wiznet_toe.c defines MP_QSTR_WIZNET_TOE etc, so it must be in
    # the qstr scan set or those names never get generated.
    list(APPEND MICROPY_SOURCE_QSTR
        ${MICROPY_PORT_DIR}/wiznet_toe/network_wiznet_toe.c
    )
    set_source_files_properties(
        ${MICROPY_PORT_DIR}/wiznet_toe/wiznet_toe.c
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_dhcp.c
        ${MICROPY_PORT_DIR}/wiznet_toe/toe_dns.c
        PROPERTIES COMPILE_DEFINITIONS "close=wiz_close"
    )
    # The vendored ioLibrary DHCP/DNS sources trip ESP-IDF's promoted
    # diagnostics (-Werror=format etc). A plain -Wno-error does not undo an
    # explicit -Werror=foo, so silence them wholesale like wsm_driver does --
    # and keep the close=wiz_close rename consistent with the rest of
    # ioLibrary, since dhcp.c calls socket.c's close().
    set_source_files_properties(
        ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DHCP/dhcp.c
        ${MICROPY_PORT_DIR}/wiznet_toe/Internet/DNS/dns.c
        PROPERTIES COMPILE_DEFINITIONS "close=wiz_close" COMPILE_OPTIONS "-w"
    )
    target_compile_definitions(${MICROPY_TARGET} PUBLIC
        MICROPY_WIZNET_TOE_SOCKET_WRAP=1
    )
    target_link_options(${MICROPY_TARGET} PUBLIC
        -Wl,--undefined=__wrap_lwip_socket
        -Wl,--wrap=lwip_socket
        -Wl,--wrap=lwip_bind
        -Wl,--wrap=lwip_listen
        -Wl,--wrap=lwip_accept
        -Wl,--wrap=lwip_connect
        -Wl,--wrap=lwip_send
        -Wl,--wrap=lwip_recv
        -Wl,--wrap=lwip_recvfrom
        -Wl,--wrap=lwip_sendto
        # MicroPython's modsocket.c sends via lwip_write() and adjusts blocking
        # via lwip_fcntl(); wsm_driver's original list had neither.
        -Wl,--wrap=lwip_write
        -Wl,--wrap=lwip_read
        -Wl,--wrap=lwip_fcntl
        -Wl,--wrap=lwip_close
        -Wl,--wrap=lwip_getsockname
        -Wl,--wrap=lwip_setsockopt
        -Wl,--wrap=lwip_getsockopt
        # Step 7: socket.getaddrinfo() reaches lwip_getaddrinfo() directly, so
        # without these the DNS query goes to lwIP (no route on TOE) and fails.
        -Wl,--wrap=lwip_getaddrinfo
        -Wl,--wrap=lwip_freeaddrinfo
    )
endif()

# Add additional extmod and usermod components.
if (MICROPY_PY_BTREE)
    target_link_libraries(${MICROPY_TARGET} $<TARGET_OBJECTS:micropy_extmod_btree>)
    target_link_libraries(${MICROPY_TARGET} "-u abort_")  # micropy_extmod_btree links to this symbol found in MICROPY_TARGET
endif()
target_link_libraries(${MICROPY_TARGET} usermod)

# Extra linker options
# (when wrap symbols are in standalone files, --undefined ensures
# the linker doesn't skip that file.)
target_link_options(${MICROPY_TARGET} PUBLIC
  # Patch LWIP memory pool allocators (see lwip_patch.c)
  -Wl,--undefined=memp_malloc
  -Wl,--wrap=memp_malloc
  -Wl,--wrap=memp_free

  # Enable the panic handler wrapper
  -Wl,--undefined=esp_panic_handler
  -Wl,--wrap=esp_panic_handler
)

# Collect all of the include directories and compile definitions for the IDF components,
# including those added by the IDF Component Manager via idf_components.yaml.
foreach(comp ${__COMPONENT_NAMES_RESOLVED})
    micropy_gather_target_properties(__idf_${comp})
    micropy_gather_target_properties(${comp})
endforeach()

# Include the main MicroPython cmake rules.
include(${MICROPY_DIR}/py/mkrules.cmake)

# Generate source files for named pins (requires mkrules.cmake for MICROPY_GENHDR_DIR).

set(GEN_PINS_PREFIX "${MICROPY_PORT_DIR}/boards/pins_prefix.c")
set(GEN_PINS_MKPINS "${MICROPY_PORT_DIR}/boards/make-pins.py")
set(GEN_PINS_SRC "${CMAKE_BINARY_DIR}/pins.c")
set(GEN_PINS_HDR "${MICROPY_GENHDR_DIR}/pins.h")

if(EXISTS "${MICROPY_BOARD_DIR}/pins.csv")
    set(GEN_PINS_BOARD_CSV "${MICROPY_BOARD_DIR}/pins.csv")
    set(GEN_PINS_BOARD_CSV_ARG --board-csv "${GEN_PINS_BOARD_CSV}")
endif()

target_sources(${MICROPY_TARGET} PRIVATE ${GEN_PINS_HDR})

add_custom_command(
    OUTPUT ${GEN_PINS_SRC} ${GEN_PINS_HDR}
    COMMAND ${Python3_EXECUTABLE} ${GEN_PINS_MKPINS} ${GEN_PINS_BOARD_CSV_ARG}
        --prefix ${GEN_PINS_PREFIX} --output-source ${GEN_PINS_SRC} --output-header ${GEN_PINS_HDR}
    DEPENDS
        ${MICROPY_MPVERSION}
        ${GEN_PINS_MKPINS}
        ${GEN_PINS_BOARD_CSV}
        ${GEN_PINS_PREFIX}
    VERBATIM
    COMMAND_EXPAND_LISTS
)
