idf_component_register(
    SRCS "dd_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition log
    PRIV_REQUIRES vfs
)
# Re-export esp_littlefs include path so consumers can call esp_littlefs_info().
idf_component_get_property(LFS_DIR joltwallet__littlefs COMPONENT_DIR)
if(LFS_DIR)
    target_include_directories(${COMPONENT_LIB} INTERFACE "${LFS_DIR}/include")
endif()
