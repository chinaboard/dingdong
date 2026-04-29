idf_component_register(
    SRCS "dd_ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_https_ota app_update mbedtls
    PRIV_REQUIRES log esp_partition cjson
)
