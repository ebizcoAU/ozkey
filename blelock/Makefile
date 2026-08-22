# blelock/Makefile — wraps arduino-cli so a board is a name, not a
# hand-typed FQBN. Every one of this session's flashing mistakes traced back
# to the raw FQBN string: CDCOnBoot=cdc forgotten (native USB never enumerates
# on the 1.9" board), or the wrong sketch pointed at the wrong port. These
# targets bake in the known-correct FQBN per board so that class of mistake
# can't happen. Never touches the Arduino IDE — arduino-cli only, which is
# all this project has ever actually used.
#
# BOARD names the physical HARDWARE, not the software role — bridge32.ino
# (the OZBRIDGE firmware) runs on GEEKC6 today; P4C6COMBO is the planned
# future hardware for the same role (a much more capable board — P4+C6
# combo, 32MB flash, 32MB PSRAM, 128GB SD, 768KB RAM — meant to also host a
# low-cost Thread+WiFi mini hotel PMS server, not just the bridge). Naming
# the Makefile slot after the board, not the role, is what lets one role's
# firmware move to new hardware without renaming every reference to it.
#
# Usage:
#   make list-ports                                  # find out what's plugged in
#   make compile BOARD=19                             # build only, no upload
#   make flash   BOARD=19       PORT=/dev/cu.usbmodemXXXX
#   make flash   BOARD=147      PORT=/dev/cu.usbmodemXXXX
#   make flash   BOARD=GEEKC6   PORT=/dev/cu.usbmodemXXXX
#   make flash   BOARD=dlock19  PORT=/dev/cu.usbmodemXXXX  # UI-only demo, no BLE/crypto
#   make monitor PORT=/dev/cu.usbmodemXXXX
#
# BOARD selects both the sketch folder and the FQBN — the two can never
# mismatch by hand. PORT is required for flash/monitor, ignored for compile.

ARDUINO_CLI := arduino-cli

# ── Per-board FQBN + sketch folder ──────────────────────────────────────────
# CDCOnBoot=cdc required on every board here — confirmed 2026-08-07 on all
# three (1.47", 1.9", and GEEKC6): all three are ESP32-C6 with the native
# USB-Serial/JTAG peripheral (esptool flash_id confirms this for GEEKC6:
# "USB mode: USB-Serial/JTAG"), and without this flag the app firmware never
# routes Serial to it — board is alive, just silent. flashwatch.sh's FQBN
# (no CDCOnBoot) is stale/wrong by this evidence, not a working alternative.
FQBN_147     := esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc
FQBN_19      := esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc
# GEEKC6 is the ONLY board here with 16 MB (N16, confirmed by esptool flash_id:
# Device 0x4018). It used default_8MB, which maps only the first 8 MB and left
# the rest unreachable. `custom` picks up bridge32/partitions.csv — see the
# header in that file for the layout, the upgrade-safety reasoning (nvs and
# app0 offsets are unchanged, so the Thread dataset survives and no lock is
# orphaned) and why the reported percentage is now measured against the whole
# chip rather than the app slot.
# 147/19/dlock19 stay on default_8MB: those are N8 parts, where an 8 MB table
# is exactly right and nothing is wasted.
FQBN_GEEKC6  := esp32:esp32:esp32c6:FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc
FQBN_dlock19 := esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc
# SIMLOCK — ozkit's N8 test-master (bare ESP32-C6-DevKitC-1-N8, 8MB flash,
# same part class as 147/19/dlock19). No LCD/touch, so no peripheral
# conflicts; CDCOnBoot=cdc still required for native-USB serial to work at
# all, same reason as every other board here.
FQBN_SIMLOCK := esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc
# P4C6COMBO — reserved slot, NOT configured yet. The ESP32-P4 uses a
# different Arduino core (esp32:esp32:esp32p4, not esp32c6) and this board's
# actual flash/PSRAM/partition options aren't known without real hardware in
# hand — deliberately left unset rather than guessed. Fill in FQBN_P4C6COMBO
# and SKETCH_P4C6COMBO once the board arrives; `compile`/`flash` already
# error cleanly on an unset BOARD, no other change needed.

SKETCH_147     := doorlock
SKETCH_19      := doorlock19
SKETCH_GEEKC6  := bridge32
SKETCH_dlock19 := dlock19
SKETCH_SIMLOCK := simlock

FQBN   := $(FQBN_$(BOARD))
SKETCH := $(SKETCH_$(BOARD))

# ── PROFILE — which DP map this BUILD speaks (operator, 2026-08-20) ──────────
#
# A lock is manufactured against ONE DL-MCU, so which DP list it speaks is a
# fact about the hardware in the door and belongs in the build, not in a
# runtime negotiation with whatever is on the far end of the UART. Firmware
# used to adopt whatever the MCU's 0x01 reply claimed; on the bench that meant
# it silently switched maps mid-session and read every DP under a profile for a
# product that was not attached.
#
#   make flash BOARD=19 PORT=... PROFILE=tuya-luona-ds013-t3
#
# 🔴 OMITTING IT IS NOW A CONFIGURATION ERROR, not the historical default.
# The build falls back to OZ_PROFILE_DEFAULT_ID, which since 2026-08-20 is
# `tuya-wifi-lock-pro` — Tuya's published standard, chosen as the least-wrong
# default when the invented map was deleted. It is still a GUESS for any
# specific lock: DP 76 is `unlock_ble` on Luona and `fill_light` on that map,
# so an unpinned build can answer an unlock by turning on a lamp. It warns
# loudly at boot. Always pin.
#
# The id must match a profiles/products/*.json profile_id — a typo is caught
# at boot with a loud [PROFILE] line, not silently ignored.
#
# Valid ids: `ls profiles/products/` — do not keep a copy of the list here.
# The five ids this comment used to name were all deleted or renamed in
# a6db8c3 and every one of them survived here as a bench instruction that
# would have silently mispinned a board.
PROFILE ?=
ifneq ($(PROFILE),)
PROFILE_FLAGS := --build-property "compiler.cpp.extra_flags=-DOZ_PROFILE_BUILD=\"$(PROFILE)\" -DOZ_PROFILE_PINNED=1"
else
PROFILE_FLAGS :=
endif

.PHONY: help compile flash monitor list-ports

help:
	@echo "Boards: 147  19  GEEKC6  dlock19  SIMLOCK  (P4C6COMBO reserved, not configured yet)"
	@echo "  make compile BOARD=<board> [PROFILE=<dp-map>]"
	@echo "  make flash   BOARD=<board> PORT=/dev/cu.usbmodemXXXX"
	@echo "  make monitor PORT=/dev/cu.usbmodemXXXX"
	@echo "  make list-ports"

compile:
	@test -n "$(BOARD)" || { echo "BOARD is required — one of: 147, 19, GEEKC6, dlock19, SIMLOCK"; exit 1; }
	@test -n "$(FQBN)"  || { echo "unknown/unconfigured BOARD '$(BOARD)' — one of: 147, 19, GEEKC6, dlock19, SIMLOCK (P4C6COMBO reserved, not configured yet)"; exit 1; }
	$(ARDUINO_CLI) compile $(PROFILE_FLAGS) --fqbn "$(FQBN)" $(SKETCH)

flash:
	@test -n "$(BOARD)" || { echo "BOARD is required — one of: 147, 19, GEEKC6, dlock19, SIMLOCK"; exit 1; }
	@test -n "$(FQBN)"  || { echo "unknown/unconfigured BOARD '$(BOARD)' — one of: 147, 19, GEEKC6, dlock19, SIMLOCK (P4C6COMBO reserved, not configured yet)"; exit 1; }
	@test -n "$(PORT)"  || { echo "PORT is required — e.g. PORT=/dev/cu.usbmodem14101 (see: make list-ports)"; exit 1; }
	$(ARDUINO_CLI) compile $(PROFILE_FLAGS) --upload -p "$(PORT)" --fqbn "$(FQBN)" $(SKETCH)

monitor:
	@test -n "$(PORT)" || { echo "PORT is required — see: make list-ports"; exit 1; }
	$(ARDUINO_CLI) monitor -p "$(PORT)" -c baudrate=115200

list-ports:
	$(ARDUINO_CLI) board list
