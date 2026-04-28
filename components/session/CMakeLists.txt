idf_component_register(
    SRCS "dd_session.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log
    PRIV_REQUIRES esp_system
)
