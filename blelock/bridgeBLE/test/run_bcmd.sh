#!/bin/sh
# BRIDGE-CMD-AUTH-1 host test: sh test/run_bcmd.sh (from blelock/bridgeBLE)
set -eu
oz=../ozcore; m=$oz/third_party/monocypher; o=${TMPDIR:-/tmp}/bcmd.$$
mkdir -p "$o"
cc -std=c99 -O1 -Wall -c -I$oz/include $oz/src/oz_cbor.c -o "$o/oz_cbor.o"
cc -std=c99 -O1 -c -I$m $m/monocypher.c -o "$o/mc.o"
cc -std=c99 -O1 -c -I$m $m/optional/monocypher-ed25519.c -o "$o/mced.o"
c++ -std=c++11 -Wall -Wextra -Werror -I. -I$oz/include -I$m -I$m/optional -I$oz/tests/vectors \
    test/test_bcmd.cpp "$o/oz_cbor.o" "$o/mc.o" "$o/mced.o" -o "$o/test_bcmd"
"$o/test_bcmd"; rc=$?; rm -rf "$o"; exit $rc
