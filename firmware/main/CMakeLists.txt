set(dura_main_requires
    dura_board
    dura_meter
    dura_recipe
    dura_peers
    dura_coordinator
    dura_legacy_protocol
    nvs_flash
    esp_system
    esp_app_format
    esp_timer
    esp_driver_gpio
    esp_driver_uart
    spi_flash
    esp_wifi
    esp_netif
    esp_event
    network_provisioning
    esp_http_client
    esp_https_ota
    bootloader_support
    esp-tls
)

if(NOT CONFIG_DURA_APP_QEMU_NO_RADIO)
    list(APPEND dura_main_requires gama_blefob)
endif()

idf_component_register(
    SRCS "app_main.c" "dura_wifi_provisioning.c" "dura_ota.c"
    INCLUDE_DIRS "."
    REQUIRES ${dura_main_requires}
)
