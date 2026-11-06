#!/bin/sh
# Stage ozcore as an Arduino library for the ESP32-C6 build (ozkey-55 F5).
# Arduino compiles only a library's src/ tree, so the engine, Monocypher and
# this port are copied (flat) into <out>/ozcore/src. Nothing is copied into
# the repo; the staged tree is rebuilt on every compile.
#   port/esp32c6/stage.sh <out-dir>
set -eu
oz=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: stage.sh <out-dir>}
lib="$out/ozcore"
rm -rf "$lib"
mkdir -p "$lib/src"
cat > "$lib/library.properties" <<P
name=ozcore
version=0.0.0
author=ozkie
maintainer=ozkie
sentence=OZKIE v2 lock engine (staged by port/esp32c6/stage.sh)
paragraph=
category=Communication
url=
architectures=esp32
P
cp "$oz"/include/*.h "$oz"/src/*.c "$lib/src/"
cp "$oz"/third_party/monocypher/monocypher.c "$oz"/third_party/monocypher/monocypher.h "$lib/src/"
cp "$oz"/port/esp32c6/*.c "$oz"/port/esp32c6/*.cpp "$oz"/port/esp32c6/*.h "$lib/src/"
echo "staged $(ls "$lib/src" | wc -l | tr -d ' ') files into $lib"
