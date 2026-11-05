#!/bin/sh
# Stage the two ozcore pieces the bridge needs as an Arduino library
# (ozkey-48 rev 6 §2.4): oz_frame (GATT framing, the same code the lock runs)
# and oz_cbor (reading the lock's `info`). The bridge never needs the engine
# or any key: it relays OZKIE messages unopened.
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
sentence=OZKIE framing + CBOR reader for the bridge (staged from ozcore)
paragraph=
category=Communication
url=
architectures=esp32
P
cp "$oz"/include/oz_frame.h "$oz"/include/oz_cbor.h "$oz"/include/oz_port.h "$lib/src/"
cp "$oz"/src/oz_frame.c "$oz"/src/oz_cbor.c "$lib/src/"
echo "staged $(ls "$lib/src" | wc -l | tr -d ' ') files into $lib"
