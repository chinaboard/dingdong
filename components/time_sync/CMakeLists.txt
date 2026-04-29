idf_component_register(
    SRCS "dd_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log lwip
    PRIV_REQUIRES esp_netif config nvs_flash
)
