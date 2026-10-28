# ozcore for AK1037 (ozkey-49 P3). Runs on the Windows build host:
#   make -f port/ak1037/Makefile
# from the ozcore directory. Produces build-ak1037/libozcore.a.

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
INC    = -Iinclude -Ithird_party/monocypher

include port/ak1037/sdk_inc.mk

CORE   = src/sha256_sw.c src/aes128_sw.c src/oz_crypto.c \
         src/oz_cbor.c src/oz_edhoc.c src/oz_coap.c src/oz_oscore.c src/oz_invite.c src/oz_lock.c
OBJS   = $(CORE:src/%.c=$(OUT)/%.o) $(OUT)/monocypher.o $(OUT)/oz_port_ak1037.o

$(OUT)/libozcore.a: $(OBJS)
	$(AR) rcs $@ $^
	$(SIZE) -t $^

# Portable core: the same warnings as the host build.
$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(BASE) $(STRICT) $(INC) -c $< -o $@

# Third-party: as shipped.
$(OUT)/monocypher.o: third_party/monocypher/monocypher.c | $(OUT)
	$(CC) $(BASE) -Wall -Werror $(INC) -c $< -o $@

# Port: vendor headers don't survive -Wpedantic/-Wconversion.
$(OUT)/oz_port_ak1037.o: port/ak1037/oz_port_ak1037.c | $(OUT)
	$(CC) $(BASE) -Wall -Wextra -Werror $(INC) $(SDKINC) -c $< -o $@

$(OUT):
	if not exist $(OUT) mkdir $(OUT)
