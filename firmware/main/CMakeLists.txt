set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

# Glob the portable core (including Pip care and motion wake), apps, and baked fonts
# picked up when files are added (matches the simulator's source discovery).
# ESP-IDF also reads this file in script mode, which forbids CONFIGURE_DEPENDS;
# enable it only for the real configure/generate pass.
set(CORE_GLOB_OPTIONS)
if(NOT CMAKE_SCRIPT_MODE_FILE)
    list(APPEND CORE_GLOB_OPTIONS CONFIGURE_DEPENDS)
endif()
# NOS (branch nothing-os): the new OS under nos/ plus only the core libraries it
# uses. The old UI (core/wos.c, core/apps, old faces) is no longer built.
set(NOS ${CMAKE_CURRENT_SOURCE_DIR}/../../nos)
file(GLOB_RECURSE NOS_SRCS ${CORE_GLOB_OPTIONS} "${NOS}/*.c")
set(CORE_SRCS
    "${CORE}/asset_job.c" "${CORE}/gfx.c" "${CORE}/font.c" "${CORE}/sprite.c" "${CORE}/anim.c"
    "${CORE}/battery.c" "${CORE}/pedometer.c" "${CORE}/walk.c" "${CORE}/notif.c"
    "${CORE}/motion_wake.c" "${CORE}/wake_service.c" "${CORE}/settings.c" "${CORE}/knock.c" "${CORE}/ephem.c")

idf_component_register(
    SRCS
        "nos_main.c"
        "sd_xfer.c"
        "ble_xfer.c"
        "nos_power.c"
        "nos_frame_wait.c"
        "nos_display_te.c"
        "nos_heap_diag.c"
        "nos_usb_diag.c"
        "nos_legacy.c"
        "hal_esp32.c"
        "board.c"
        "imu_hal.c"
        "board_protocol.c"
        "weather_parse.c"
        "weather_service.c"
        "voice_chat.c"
        "voice_audio.c"
        "lcd_probe.c"
        "display_perf.c"
        "display_bench.c"
        "ota.c"
        "ext/ext_sys.c"
        "ext/ext_haptic.c"
        "ext/ext_audio.c"
        "ext/ext_gps.c"
        "ext/ext_lte.c"
        "ext/ext_env.c"
        "ext/ext_light.c"
        "ext/ext_mag.c"
        "ext/ext_hr.c"
        ${CORE_SRCS}
        ${NOS_SRCS}
    INCLUDE_DIRS "." "${CORE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../nos" "${CMAKE_CURRENT_SOURCE_DIR}/../../nos/fonts" "${CMAKE_BINARY_DIR}/generated"
    EMBED_TXTFILES "watch_setup.html"
    REQUIRES esp_pm esp_app_format esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_driver_sdmmc esp_driver_i2s esp_driver_uart esp_driver_ledc fatfs sdmmc esp_adc esp_timer esp_wifi esp_event esp_netif nvs_flash bt esp_http_server esp_http_client esp_https_ota app_update spi_flash mbedtls json espcoredump
)

# Cached menu pixels must change whenever their artwork, fonts, or registry do.
# Verify on every build, including an incremental build that skips configure.
idf_build_get_property(NATIVE_MENU_PYTHON PYTHON)
add_custom_target(nos_native_menu_assets_check
    COMMAND "${NATIVE_MENU_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_native_menu_assets.py" --verify
    COMMENT "Verifying baked native menu artwork"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} nos_native_menu_assets_check)
# The menu art is a pack file, not C source. The B watch loads /sdcard/menu/native_menu_sharp.pack
# at boot (tools/sdsync.py pushes it); the 1.43C has no SD slot, so only that build embeds the
# same binary pack, and the Orrery sky catalogs (belt.bin, moons.bin) that the B watch reads from /sdcard/sky.
# Without the pack, either board falls back to the dynamic bake.
if(CONFIG_WATCH_BOARD_143C)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/sd/menu/native_menu_sharp.pack" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/sd/sky/belt.bin" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/sd/sky/moons.bin" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_MENU_PACK_EMBEDDED=1 NOS_SKY_EMBEDDED=1)
endif()

# Compile/link check of the strap drivers before main.c calls hal_ext_init():
target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_PSRAM_TRACKING)
# User-requested sharp performance firmware. Keep the normal renderer available
# to reference tests; the source/build identity records these definitions.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_RENDER_SHARP=1 NOS_RENDER_PROFILING=1)
# build/build_ext.ps1 passes -DEXT_FORCE_LINK=1 so --gc-sections keeps them.
if(EXT_FORCE_LINK)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,hal_ext_init" "-Wl,-u,hal_ext_debug")
endif()

# Until the NOS fonts are baked, ui.c borrows the old core fonts.
if(NOT EXISTS "${NOS}/fonts/nos_fonts.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_FONT_SHIM)
    file(GLOB CORE_FONT_SRCS "${CORE}/fonts/*.c")
    target_sources(${COMPONENT_LIB} PRIVATE ${CORE_FONT_SRCS})
endif()

# UI labels are fixed-size buffers; snprintf truncation there is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-error=comment)

# Diagnostic-only interposition: real SDK return values/arguments are unchanged.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=usb_serial_jtag_is_connected"
    "-Wl,--wrap=usb_serial_jtag_write_bytes"
    "-Wl,--wrap=heap_caps_malloc"
    "-Wl,--wrap=heap_caps_calloc"
    "-Wl,--wrap=heap_caps_realloc"
    "-Wl,--wrap=heap_caps_aligned_alloc")
