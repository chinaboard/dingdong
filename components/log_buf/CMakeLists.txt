idf_component_register(
    SRCS "dd_log.c"
    INCLUDE_DIRS "include"
    REQUIRES log
    PRIV_REQUIRES freertos
)
