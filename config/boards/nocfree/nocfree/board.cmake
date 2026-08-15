# These devices ship with the Adafruit nRF52 UF2 bootloader and we never erase it.
# Normal flashing is drag-and-drop of the generated zmk.uf2 onto the bootloader drive.
# nrfjprog is only relevant if an SWD probe is ever attached.
board_runner_args(nrfjprog "--nrf-family=NRF52")
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
