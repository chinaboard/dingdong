idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES config session wifi_mgr http_app ble time_sync storage workers led
    PRIV_REQUIRES app_update esp_timer esp_driver_gpio esp_app_format
)
