cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Version string baked into esp_app_desc_t.version, surfaced in bootloader
# logs, OTA descriptors, /api/system/diag, and the Web UI header.
#
# Source of truth is the ./VERSION file (semver, manually bumped). The
# Makefile passes -DBUILD_TS=YYYYMMDD.HHMM on each build and we append it as
# a semver-style build identifier — yields e.g. "0.5.0+20260428.1506" so two
# builds of the same VERSION are still distinguishable in the field.
# esp_app_desc_t.version[32] caps us at 31 chars; "X.Y.Z+YYYYMMDD.HHMM" is
# 19, plenty of headroom.
file(STRINGS "VERSION" BASE_VER)
if(DEFINED BUILD_TS AND NOT BUILD_TS STREQUAL "")
    set(PROJECT_VER "${BASE_VER}+${BUILD_TS}")
else()
    set(PROJECT_VER "${BASE_VER}")
endif()
project(dingdong)
