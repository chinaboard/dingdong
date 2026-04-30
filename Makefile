# dingdong-fw — ESP32-C3/C6 build/flash via Docker IDF + macOS espflash
#
# Build runs inside espressif/idf:v6.0.1 (Docker), since macOS Docker can't
# pass through USB. Flash/monitor run on the host with espflash, which talks
# to /dev/cu.usbmodem101 directly.
#
# Pick a board with BOARD=<name>; the file boards/<name>.mk supplies all
# board-specific defaults (TARGET, LED_KIND, LED_GPIO…). Each board lives in
# its own per-target build directory (build-esp32cN/) so you can keep both
# binaries around and switch with no fullclean dance.
#
# Built-in boards:
#   BOARD=supermini-c6   ESP32-C6 SuperMini, WS2812 on GPIO8 (default)
#   BOARD=supermini-c3   ESP32-C3 SuperMini, plain blue LED on GPIO8
#   BOARD=generic        no LED, c6 target — override TARGET=... yourself
# Add a new board by dropping a new boards/<name>.mk; no Makefile edit needed.
#
# CLI overrides still work:  make build BOARD=supermini-c3 LED_BRIGHTNESS=30

PORT       ?= /dev/cu.usbmodem101
BAUD       ?= 921600
IDF_IMAGE  ?= espressif/idf:v6.0.1
PROJECT    ?= dingdong
BOARD      ?= supermini-c6

# Board overlay sets TARGET / LED_* defaults. Use ?= inside so explicit CLI
# values (which Make sets before this include runs) win.
include boards/$(BOARD).mk

IDF_TARGET := esp32$(TARGET)
BUILD_DIR  := build-$(IDF_TARGET)
# Per-target sdkconfig (the generated one with all expanded settings).
# Defaults overlay still comes from sdkconfig.defaults + sdkconfig.defaults.$(IDF_TARGET).
SDKCONFIG  := sdkconfig.$(IDF_TARGET)

# Build-time timezone (POSIX TZ string). Override per region:
#   make build TZ=JST-9        # Japan
#   make build TZ=EST5EDT      # US East
TZ         ?= CST-8

# IDF picks up EXTRA_CFLAGS from the env at build time. Inner values use
# escaped double quotes so the whole string can be wrapped in shell double
# quotes when passed to `docker run -e` below — single-quoting inside
# single-quoting doesn't compose.
EXTRA_CFLAGS := -DDD_TZ=\"$(TZ)\" \
                -DDD_LED_ENABLE=$(LED_ENABLE) \
                -DDD_LED_KIND=$(LED_KIND) \
                -DDD_LED_GPIO=$(LED_GPIO) \
                -DDD_LED_BRIGHTNESS=$(LED_BRIGHTNESS)

# Debug-only WiFi cred override: skip the SoftAP setup wizard and boot a
# blank board straight into STA mode using these creds. Never set in CI.
#   make build DEBUG_WIFI_SSID=IoToI DEBUG_WIFI_PASS=54383845
ifdef DEBUG_WIFI_SSID
EXTRA_CFLAGS += -DDEBUG_WIFI_SSID=\"$(DEBUG_WIFI_SSID)\"
endif
ifdef DEBUG_WIFI_PASS
EXTRA_CFLAGS += -DDEBUG_WIFI_PASS=\"$(DEBUG_WIFI_PASS)\"
endif

DOCKER_RUN = docker run --rm -v $(PWD):/project -w /project \
             -e IDF_TARGET=$(IDF_TARGET) -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(IDF_IMAGE)
DOCKER_TTY = docker run --rm -it -v $(PWD):/project -w /project \
             -e IDF_TARGET=$(IDF_TARGET) -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(IDF_IMAGE)

.PHONY: build flash monitor flash-monitor erase clean fullclean menuconfig size shell

build:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) reconfigure build

flash:
	espflash flash \
		--port $(PORT) --baud $(BAUD) \
		--partition-table $(BUILD_DIR)/partition_table/partition-table.bin \
		--bootloader $(BUILD_DIR)/bootloader/bootloader.bin \
		$(BUILD_DIR)/$(PROJECT).elf

monitor:
	espflash monitor --port $(PORT)

flash-monitor: flash monitor

erase:
	espflash erase-flash --port $(PORT)

size:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) size

menuconfig:
	$(DOCKER_TTY) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) menuconfig

shell:
	$(DOCKER_TTY) bash

clean:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) fullclean

# Wipe ALL per-target build dirs + generated sdkconfigs + managed components.
# Careful: only delete sdkconfig.esp32cN (generated), NOT sdkconfig.defaults*
# (checked into git as the source of truth).
fullclean:
	rm -rf build build-* sdkconfig sdkconfig.esp32* managed_components dependencies.lock
