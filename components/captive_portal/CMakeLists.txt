idf_component_register(
    SRCS "dd_captive.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip log
)
