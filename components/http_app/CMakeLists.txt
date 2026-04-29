idf_component_register(
    SRCS "dd_http.c"
         "dd_http_system.c"
         "dd_http_attendance.c"
         "dd_http_devices.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer esp_wifi esp_system log
             config wifi_mgr session ble events storage time_sync workers led log_buf ota_pull
    PRIV_REQUIRES cjson esp_app_format app_update
)

# Web UI is gzipped at build time and embedded as a binary blob. Saves ~20 KB
# of flash vs the raw HTML; root_get sends it as-is with Content-Encoding:gzip.
set(WEB_SRC "${COMPONENT_DIR}/web/index.html")
set(WEB_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT  "${WEB_GZ}"
    COMMAND gzip -9 -c "${WEB_SRC}" > "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    VERBATIM
)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
