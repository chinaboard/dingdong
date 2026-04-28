idf_component_register(
    SRCS "dd_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt log esp_event esp_timer
    PRIV_REQUIRES events config time_sync
)
