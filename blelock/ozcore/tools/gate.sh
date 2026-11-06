#!/bin/sh
# Firmware commit gate (PM nexus-a8 ruling 2026-10-29, ozkey-49 §4 "Commit gate").
#
#   blelock/ozcore/tools/gate.sh <label>
#
# Builds and runs ctest three ways, each from a clean build directory:
#   default  no build type, so assert() and littlefs's LFS_ASSERT are live
#   release  -DCMAKE_BUILD_TYPE=Release (NDEBUG, the optimised code)
#   asan     ASan + UBSan, any report fails the test
# The ruling asks for the sanitizer pass when port/, common/ or src/ change;
# this runs it every time, which costs about a minute.
#
# The log goes to $OZ_GATE_LOG_DIR (default: the PM's ozkey_notes/ctest/) as
# <date>_<label>.log. Exit status 0 only if all three pass: commit on 0 only.
set -u

label=${1:?usage: gate.sh <label>}
oz=$(cd "$(dirname "$0")/.." && pwd)
logdir=${OZ_GATE_LOG_DIR:-$HOME/Documents/Dev/nexus-lab-backups/ozkey_notes/ctest}
log=$logdir/$(date +%Y-%m-%d)_$label.log
work=${TMPDIR:-/tmp}/ozcore-gate
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

mkdir -p "$logdir" "$work"
rc=0
{
    echo "gate $label: $(git -C "$oz" rev-parse --abbrev-ref HEAD) @ $(git -C "$oz" rev-parse --short HEAD)"
    git -C "$oz" status --short -- .
    date
    for kind in default release asan; do
        b=$work/$kind
        rm -rf "$b"
        case $kind in
        default) cmake -S "$oz" -B "$b" >/dev/null ;;
        release) cmake -S "$oz" -B "$b" -DCMAKE_BUILD_TYPE=Release >/dev/null ;;
        asan)    cmake -S "$oz" -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                       -DCMAKE_C_FLAGS="$san" -DCMAKE_EXE_LINKER_FLAGS="$san" >/dev/null ;;
        esac
        echo "--- $kind"
        if cmake --build "$b" -j8 >"$b.build.log" 2>&1 && ctest --test-dir "$b" --output-on-failure; then
            echo "$kind: rc=0"
        else
            tail -20 "$b.build.log"
            echo "$kind: FAIL"
            rc=1
        fi
    done
    echo "gate rc=$rc"
} >"$log" 2>&1
grep -E '^--- |tests passed|: rc=|: FAIL|^gate rc' "$log"
echo "log: $log"
exit $rc
