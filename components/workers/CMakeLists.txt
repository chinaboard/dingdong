idf_component_register(
    SRCS "dd_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log
    PRIV_REQUIRES cjson
)
