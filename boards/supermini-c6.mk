# ESP32-C6 SuperMini — onboard WS2812 RGB pixel on GPIO8.
# Plain KEY=VAL so this file is sourceable from both Make (via include) and
# bash (via .). Make's command-line variables (e.g. TARGET=c3) still override
# these — that priority rule beats Makefile assignments without needing ?=.
TARGET=c6
LED_ENABLE=1
LED_KIND=1
LED_GPIO=8
LED_BRIGHTNESS=10
