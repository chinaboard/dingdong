idf_component_register(
    SRCS "dd_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls log
)
