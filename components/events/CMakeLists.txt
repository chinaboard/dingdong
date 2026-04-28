idf_component_register(
    SRCS "dd_event.c"
    INCLUDE_DIRS "include"
    REQUIRES log storage time_sync workers
    PRIV_REQUIRES cjson
)
