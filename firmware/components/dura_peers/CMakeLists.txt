if(CONFIG_DURA_APP_M3000_LEGACY_ONLY)
    # M3000 retains the future peer API but excludes Wi-Fi and ESP-NOW from the
    # product binary.  The local implementation supplies only device identity
    # and explicit ESP_ERR_NOT_SUPPORTED results for network operations.
    set(dura_peers_src "dura_peers_local.c")
    set(dura_peers_requires esp_hw_support dura_recipe)
else()
    set(dura_peers_src "dura_peers.c")
    set(dura_peers_requires esp_wifi esp_event esp_netif esp_timer dura_recipe)
endif()

idf_component_register(
    SRCS ${dura_peers_src}
    INCLUDE_DIRS "include"
    REQUIRES ${dura_peers_requires}
)
