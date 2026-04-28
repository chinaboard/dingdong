# dingdong-fw — ESP32-C6 build/flash via Docker IDF + macOS espflash
#
# Build runs inside espressif/idf:v6.0.1 (Docker), since macOS Docker can't
# pass through USB. Flash/monitor run on the host with espflash, which talks
# to /dev/cu.usbmodem101 directly.

PORT       ?= /dev/cu.usbmodem101
BAUD       ?= 921600
IDF_IMAGE  ?= espressif/idf:v6.0.1
PROJECT    ?= dingdong

# Build-time timezone (POSIX TZ string). Override per region:
#   make build TZ=JST-9        # Japan
#   make build TZ=EST5EDT      # US East
TZ         ?= CST-8

# Build-time LED config. SuperMini ships with a WS2812 on GPIO8 (most revs).
#   LED_ENABLE=0    skip LED code entirely (boards without an addressable LED)
#   LED_GPIO=15     override pin if your board differs
#   LED_BRIGHTNESS  0..255 — keep it dim, the on-board pixel is harsh up close
LED_ENABLE     ?= 1
LED_GPIO       ?= 8
LED_BRIGHTNESS ?= 10

# IDF picks up EXTRA_CFLAGS from the env at build time. Inner values use
# escaped double quotes so the whole string can be wrapped in shell double
# quotes when passed to `docker run -e` below — single-quoting inside
# single-quoting doesn't compose.
EXTRA_CFLAGS := -DDD_TZ=\"$(TZ)\" \
                -DDD_LED_ENABLE=$(LED_ENABLE) \
                -DDD_LED_GPIO=$(LED_GPIO) \
                -DDD_LED_BRIGHTNESS=$(LED_BRIGHTNESS)

DOCKER_RUN = docker run --rm -v $(PWD):/project -w /project -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(IDF_IMAGE)
DOCKER_TTY = docker run --rm -it -v $(PWD):/project -w /project -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(IDF_IMAGE)

.PHONY: build flash monitor flash-monitor erase clean fullclean menuconfig size shell

build:
	$(DOCKER_RUN) idf.py build

flash:
	espflash flash \
		--port $(PORT) --baud $(BAUD) \
		--partition-table build/partition_table/partition-table.bin \
		--bootloader build/bootloader/bootloader.bin \
		build/$(PROJECT).elf

monitor:
	espflash monitor --port $(PORT)

flash-monitor: flash monitor

erase:
	espflash erase-flash --port $(PORT)

size:
	$(DOCKER_RUN) idf.py size

menuconfig:
	$(DOCKER_TTY) idf.py menuconfig

shell:
	$(DOCKER_TTY) bash

clean:
	$(DOCKER_RUN) idf.py fullclean

fullclean:
	rm -rf build sdkconfig managed_components dependencies.lock
