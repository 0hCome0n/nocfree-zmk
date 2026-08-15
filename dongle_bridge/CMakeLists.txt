# NocFree dongle BLE->USB HID bridge (standalone Zephyr app).
# NOT the leftover ZMK nocfree_dongle split-central board.
#
# Build (inside zmk-build-arm container):
#   bash /workspace/tools/inner_build_dongle_bridge_trial.sh
#
# Flash is intentionally NOT automated. See docs/DONGLE_SAFETY.md.

cmake_minimum_required(VERSION 3.20.0)

# Board lives next to this app so we never pull ZMK dongle Kconfig defaults.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nocfree_dongle_bridge)

target_sources(app PRIVATE
  src/main.c
  src/recovery.c
  src/trial_autodfu.c
  src/usb_hid_bridge.c
  src/ble_hog_central.c
)

zephyr_library_include_directories(include)

# Hard refuse builds without USB recovery (dongle has no buttons / pinhole).
if(NOT CONFIG_NOCFREE_USB_RECOVERY)
  message(FATAL_ERROR
    "CONFIG_NOCFREE_USB_RECOVERY must be y. The dongle has no keys and no "
    "reset pinhole; refusing to build an unrecoverable image.")
endif()

# Trial = autodfu dead-man. Keeper = explicit opt-in, no autodfu.
# Bare prj.conf with neither is refused so nothing flashable is accidental.
if(CONFIG_NOCFREE_TRIAL_AUTODFU AND CONFIG_NOCFREE_BRIDGE_KEEPER)
  message(FATAL_ERROR
    "TRIAL autodfu and BRIDGE_KEEPER both set — refuse (contradictory).")
endif()
if(NOT CONFIG_NOCFREE_TRIAL_AUTODFU AND NOT CONFIG_NOCFREE_BRIDGE_KEEPER)
  message(FATAL_ERROR
    "Dongle bridge builds must be either TRIAL (prj_trial.conf) or KEEPER "
    "(prj_keeper.conf). Bare prj.conf alone is not flashable. "
    "See docs/DONGLE_SAFETY.md.")
endif()

if(CONFIG_LOG_MODE_IMMEDIATE AND CONFIG_LOG_BACKEND_UART)
  message(FATAL_ERROR
    "LOG_MODE_IMMEDIATE + LOG_BACKEND_UART is the 2026-08-09 brick combo. "
    "Refuse on dongle too.")
endif()
