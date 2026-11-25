idf_component_register(
  SRCS
    havpe_device.c
    havpe_dial.c
    havpe_portal.c
    havpe_reset_gesture.c
    havpe_ui.c
  PRIV_INCLUDE_DIRS .
  REQUIRES
    board
    audio
    capnweb
    capabilities
    core
    voice
    iterate_esp_idf
    esp_app_format
    esp_event
    esp_http_server
    esp_timer
    esp_wifi
    esp_netif
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    lwip
    mbedtls
    nvs_flash
    espressif__led_strip
    espressif__mdns)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/baked-sounds.cmake)
iterate_kit_add_baked_sounds()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
