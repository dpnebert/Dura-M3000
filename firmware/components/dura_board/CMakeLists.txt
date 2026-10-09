set(dura_board_requires
    dura_meter dura_coordinator dura_peers gama_blefob esp_driver_gpio esp_hw_support
    esp_timer espressif__button espressif__qrcode freertos nvs_flash
)

if(CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK)
    list(APPEND dura_board_requires esp_adc)
endif()

idf_component_register(
    SRCS "dura_board.c" "dura_battery.c" "dura_battery_math.c"
         "dura_ui_behavior.c" "dura_ui_config.c"
         "dura_st7567.c" "dura_lcd_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES ${dura_board_requires}
)
