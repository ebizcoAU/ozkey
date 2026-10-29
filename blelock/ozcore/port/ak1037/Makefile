# ozcore for AK1037 (ozkey-49 P3). Runs on the Windows build host:
#   make -f port/ak1037/Makefile
# from the ozcore directory. Produces build-ak1037/libozcore.a.
#   make -f port/ak1037/Makefile selftest
# also builds build-ak1037/libozselftest.a: every host suite plus the commands
# of the two bring-up images, self-test and lock app (ozkey-49 P4 item 4b).
# The link roots pick which image you get (link_app_mcu.ps1).

SDK    ?= C:/Users/sshuser/ak1037/AnyMicro1037_SDK_V1.02
PREFIX ?= C:/Users/sshuser/ak1037/SysGCC/risc-v/bin/riscv64-unknown-elf-
CC      = $(PREFIX)gcc
AR      = $(PREFIX)ar
SIZE    = $(PREFIX)size
OUT     = build-ak1037

# The SDK's own target flags (from app_mcu's eide config), C11 for _Alignas.
ARCH   = -march=rv32imac -mabi=ilp32 -mcmodel=medlow
BASE   = $(ARCH) -std=gnu11 -Os -g -fno-builtin -fdata-sections -ffunction-sections \
         -DSUPP_MSH=1 -DSUPP_RTOS=1 -DSUPPORT_BLE_MCU_PRJ=1 $(EXTRA)
STRICT = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
# EXTRA: e.g. EXTRA="-fstack-usage -fcallgraph-info=su" for the stack report.
EXTRA  ?=
INC    = -Iinclude -Ithird_party/monocypher -Iport/common

include port/ak1037/sdk_inc.mk

CORE   = src/sha256_sw.c src/aes128_sw.c src/oz_crypto.c \
         src/oz_cbor.c src/oz_edhoc.c src/oz_coap.c src/oz_oscore.c src/oz_invite.c src/oz_lock.c \
         src/oz_frame.c src/oz_ble.c src/oz_adv.c src/oz_limit.c src/oz_keypad.c
OBJS   = $(CORE:src/%.c=$(OUT)/%.o) $(OUT)/monocypher.o $(OUT)/oz_kv_lfs.o $(OUT)/oz_port_ak1037.o $(OUT)/oz_gatt_ak1037.o

$(OUT)/libozcore.a: $(OBJS)
	$(AR) rcs $@ $^
	$(SIZE) -t $^

# Bring-up images: the host suites, each main() renamed oz_t_<name> so their
# statics stay apart (tests/oz_selftest.h), the runner, and each image's MSH
# commands.
SUITES = crypto cbor edhoc oscore invite frame adv keypad lock
TOBJS  = $(SUITES:%=$(OUT)/t_%.o) $(OUT)/selftest_all.o $(OUT)/oz_selftest_ak1037.o \
         $(OUT)/oz_lockapp_ak1037.o

selftest: $(OUT)/libozcore.a $(OUT)/libozselftest.a

$(OUT)/libozselftest.a: $(TOBJS)
	$(AR) rcs $@ $^
	$(SIZE) -t $^

# oz_test.h's helpers are static and not every suite uses all of them.
$(OUT)/t_%.o: tests/test_%.c | $(OUT)
	$(CC) $(BASE) -Wall -Wno-unused-function -Werror $(INC) -Iport/host -Itests -Dmain=oz_t_$* -c $< -o $@

$(OUT)/selftest_all.o: tests/selftest_all.c | $(OUT)
	$(CC) $(BASE) $(STRICT) $(INC) -Itests -c $< -o $@

# The SDK's AK_MSH_CMD_EXPORT casts every command to long (*)(void).
$(OUT)/oz_selftest_ak1037.o $(OUT)/oz_lockapp_ak1037.o: $(OUT)/%.o: port/ak1037/%.c | $(OUT)
	$(CC) $(BASE) -Wall -Wextra -Wno-cast-function-type -Werror $(INC) -Iport/host -Itests $(SDKINC) -c $< -o $@

# Portable core: the same warnings as the host build.
$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(BASE) $(STRICT) $(INC) -c $< -o $@

# Third-party: as shipped.
$(OUT)/monocypher.o: third_party/monocypher/monocypher.c | $(OUT)
	$(CC) $(BASE) -Wall -Werror $(INC) -c $< -o $@

# kv on littlefs: our code, but it includes the SDK's lfs.h, which doesn't survive
# -Wconversion. lfs.c itself is linked from the SDK's app_mcu build, not built here.
$(OUT)/oz_kv_lfs.o: port/common/oz_kv_lfs.c | $(OUT)
	$(CC) $(BASE) -Wall -Wextra -Werror $(INC) $(SDKINC) -c $< -o $@

$(OUT)/oz_gatt_ak1037.o: port/ak1037/oz_gatt_ak1037.c | $(OUT)
	$(CC) $(BASE) -Wall -Wextra -Werror $(INC) $(SDKINC) -c $< -o $@

# Port: vendor headers don't survive -Wpedantic/-Wconversion.
$(OUT)/oz_port_ak1037.o: port/ak1037/oz_port_ak1037.c | $(OUT)
	$(CC) $(BASE) -Wall -Wextra -Werror $(INC) $(SDKINC) -c $< -o $@

$(OUT):
	if not exist $(OUT) mkdir $(OUT)
