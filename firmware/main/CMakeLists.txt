set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)

# Glob the portable core (including Pip care and motion wake), apps, and baked fonts
# picked up when files are added (matches the simulator's source discovery).
# ESP-IDF also reads this file in script mode, which forbids CONFIGURE_DEPENDS;
# enable it only for the real configure/generate pass.
set(CORE_GLOB_OPTIONS)
if(NOT CMAKE_SCRIPT_MODE_FILE)
    list(APPEND CORE_GLOB_OPTIONS CONFIGURE_DEPENDS)
endif()
file(GLOB CORE_SRCS ${CORE_GLOB_OPTIONS}
    "${CORE}/*.c"
    "${CORE}/apps/*.c"
    "${CORE}/fonts/*.c")

idf_component_register(
    SRCS
        "main.c"
        "hal_esp32.c"
        "board.c"
        "board_protocol.c"
        "weather_parse.c"
        "weather_service.c"
        "lcd_probe.c"
        "display_perf.c"
        "display_bench.c"
        ${CORE_SRCS}
    INCLUDE_DIRS "." "${CORE}"
    REQUIRES esp_app_format esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_driver_sdmmc fatfs sdmmc esp_adc esp_timer esp_wifi esp_event esp_netif nvs_flash bt esp_http_server esp_http_client mbedtls json
)
