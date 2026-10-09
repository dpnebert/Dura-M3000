idf_component_register(
    SRCS "dura_meter.c" "dura_control_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer esp_driver_gpio esp_system freertos
)
