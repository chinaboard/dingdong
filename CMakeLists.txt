cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Version string baked into esp_app_desc_t.version (max 31 chars). Surfaced
# in bootloader logs, OTA descriptors, /api/system/diag, and the Web UI
# header. Tag-driven: `git describe` is the source of truth.
#
#   Clean tag (e.g. v0.5.5)         → "0.5.5"
#   N commits past last tag         → "0.5.5-3-gabc1234"
#   Working tree dirty              → "<above>-dirty"
#   No git / no tags / git failure  → contents of ./VERSION file (fallback)
#
# So `git tag v0.5.5 && git push --tags` is the only step needed to release
# a clean version; off-tag dev builds get a self-describing string that
# pinpoints the source state without needing a separate timestamp.
execute_process(
    COMMAND git -c safe.directory=* describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RC
)
if(GIT_RC EQUAL 0 AND GIT_DESC)
    string(REGEX REPLACE "^v" "" PROJECT_VER "${GIT_DESC}")
else()
    file(STRINGS "VERSION" PROJECT_VER)
endif()

# Cap at 31 chars to fit esp_app_desc_t.version[32].
string(LENGTH "${PROJECT_VER}" _ver_len)
if(_ver_len GREATER 31)
    string(SUBSTRING "${PROJECT_VER}" 0 31 PROJECT_VER)
endif()

project(dingdong)
