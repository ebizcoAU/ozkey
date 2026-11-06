#!/bin/sh
# Stage ozcore for the bridge as an Arduino library (named "ozframe" for the
# Makefile's sake).
#   rev 6 (F1): oz_frame (GATT framing) + oz_cbor (the lock's `info`) only;
#               the bridge relayed OZKIE unopened and held no key.
#   rev 7 (F1-BRIDGE-BOND-1): the bridge also authenticates to each lock as
#               its own `bridge` bond (EDHOC initiator on `ozkie`), so it needs
#               the EDHOC/crypto engine, Monocypher, and the ESP32 port's
#               oz_port (random, X25519, NVS kv). The lock's GATT server
#               (oz_gatt_esp32c6.cpp) is NOT staged: the bridge is a central.
#               Unreferenced engine objects stay out of the link (archive).
# Relayed traffic is still never opened: the bridge's key is its own bond.
#   bridgeBLE/stage_ozframe.sh <out-dir>
set -eu
oz=$(cd "$(dirname "$0")/../ozcore" && pwd)
out=${1:?usage: stage_ozframe.sh <out-dir>}
lib="$out/ozframe"
rm -rf "$lib"
mkdir -p "$lib/src"
cat > "$lib/library.properties" <<P
name=ozframe
version=0.0.0
author=ozkie
maintainer=ozkie
sentence=OZKIE framing, CBOR and the bridge-bond EDHOC initiator (staged from ozcore)
paragraph=
category=Communication
url=
architectures=esp32
P
cp "$oz"/include/*.h "$oz"/src/*.c "$lib/src/"
cp "$oz"/third_party/monocypher/monocypher.c "$oz"/third_party/monocypher/monocypher.h "$lib/src/"
cp "$oz"/port/esp32c6/oz_port_esp32c6.c "$oz"/port/esp32c6/oz_port_esp32c6.h "$lib/src/"
echo "staged $(ls "$lib/src" | wc -l | tr -d ' ') files into $lib"
