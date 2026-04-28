idf_component_register(
    SRCS "dd_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_timer lwip log config
    PRIV_REQUIRES captive_portal
)
