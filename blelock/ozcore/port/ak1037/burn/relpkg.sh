#!/bin/sh
# REL-PKG-1 (PM nexus-1b, 2026-11-20): build the AK1037 RELEASE burn package for
# one commit, or refuse. Required before any image leaves us (CN-LAB-1).
#
#   blelock/ozcore/port/ak1037/burn/relpkg.sh <commit> [variant, default fp0_v0]
#
# Refuses: a commit not on origin/main (ALLOW_UNMERGED=1 for a dry run, the
# package is then named ...-UNMERGED); an existing release directory (never
# overwritten); a build-host failure; an image relcheck.py rejects (not exactly
# "ozkie-ak1037-<semver>+<sha7>", "+nosha", or any debug hook).
# Clean by construction: git archive of the commit (no working-tree files),
# extracted into a FRESH rel-<sha7> directory on the build host (nothing stale
# from an earlier tree), stamped with port/ak1037/oz_build_sha.h.
# Output: $OZ_REL_DIR/<version>/{ozkie_app.bin, ozkv_blank.bin,
# config_ozkie.txt, MANIFEST.txt, build.log} and <version>.zip.
set -eu
[ $# -ge 1 ] || { sed -n 2,17p "$0"; exit 2; }
commit=$1; variant=${2:-fp0_v0}
host=${OZ_WIN_HOST:-win-arthur}
wroot=${OZ_WIN_ROOT:-C:/Users/Arthur/ak1037}
sdk="$wroot/AnyMicro1037_SDK_V1.02"
reldir=${OZ_REL_DIR:-$HOME/Documents/Dev/nexus-lab-backups/ozkey_release}
here=$(cd "$(dirname "$0")" && pwd)
# git archive from a subdirectory archives only that subdirectory's view
# (found 2026-11-20: a 10 KB archive): always from the top level.
cd "$(git -C "$here" rev-parse --show-toplevel)"
git fetch -q origin
full=$(git rev-parse --verify "$commit^{commit}")
sha7=$(git rev-parse --short=7 "$full")
tag=""
if ! git merge-base --is-ancestor "$full" origin/main; then
    [ -n "${ALLOW_UNMERGED:-}" ] || { echo "REFUSED: $sha7 is not on origin/main (only merged code is released)"; exit 1; }
    tag="-UNMERGED"
fi
semver=$(git show "$full:blelock/ozcore/port/ak1037/oz_lockapp_ak1037.c" | sed -n 's/^#define OZ_LOCKAPP_SEMVER "\(.*\)"$/\1/p')
[ -n "$semver" ] || { echo "REFUSED: no OZ_LOCKAPP_SEMVER at $sha7"; exit 1; }
ver="ozkie-ak1037-$semver+$sha7"
out="$reldir/$ver-$variant$tag"
[ ! -e "$out" ] || { echo "REFUSED: $out exists (a release is never overwritten)"; exit 1; }
mkdir -p "$out"
log="$out/build.log"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
git archive --format=tar -o "$tmp/src.tar" "$full:blelock/ozcore"
for need in src/oz_lock.c port/ak1037/oz_lockapp_ak1037.c port/ak1037/burn/relpkg.ps1 port/ak1037/Makefile; do
    tar -tf "$tmp/src.tar" | grep -qx "$need" || { echo "REFUSED: the archive of $sha7 lacks $need"; rm -rf "$out"; exit 1; }
done
mkdir -p "$tmp/h/port/ak1037"
printf '#define OZ_BUILD_SHA "%s"\n' "$sha7" > "$tmp/h/port/ak1037/oz_build_sha.h"
tar -rf "$tmp/src.tar" -C "$tmp/h" port/ak1037/oz_build_sha.h
echo "relpkg $ver variant $variant commit $full$tag on $host, $(date '+%Y-%m-%d %H:%M:%S %Z')" > "$log"
fail() { mv "$out" "$out.FAILED-$(date +%Y%m%d-%H%M%S)"; echo "REFUSED: $1"; exit 1; }
scp -q -o BatchMode=yes "$tmp/src.tar" "$host:$wroot/rel-$sha7.tar" 2>>"$log" || fail "scp of the source to $host failed"
wr=$(printf '%s' "$wroot" | tr / '\\')
set +e
ssh -o BatchMode=yes "$host" "cd /d $wr && (if exist rel-$sha7 rmdir /s /q rel-$sha7) & mkdir rel-$sha7 && cd rel-$sha7 && tar -xf ..\\rel-$sha7.tar && powershell -NoProfile -ExecutionPolicy Bypass -File port\\ak1037\\burn\\relpkg.ps1 -Variant $variant" 2>&1 | tr -d '\r' | grep -v '^\*\*' >> "$log"
rc=$?
set -e
grep -q "relpkg.ps1 OK" "$log" || fail "build host failed (rc=$rc), see the build.log"
for f in ozkie_app.bin ozkv_blank.bin config_ozkie.txt; do      # from this tree's out\, not the shared burn folder
    scp -q -o BatchMode=yes "$host:$wroot/rel-$sha7/out/$f" "$out/" 2>>"$log" || fail "fetching $f failed"
done
if ! python3 -I "$here/../../../tools/relcheck.py" "$out/ozkie_app.bin" "$sha7" >> "$log" 2>&1; then
    grep REFUSED "$log"; fail "relcheck rejected the image"
fi
{
    echo "version   $ver"
    echo "variant   $variant (SDK Makefile.nano_$variant)"
    echo "commit    $full$tag"
    echo "built     $(date '+%Y-%m-%d %H:%M:%S %Z') on $host, from a fresh rel-$sha7 tree"
    echo "image     RELEASE (no OZ_DEBUG_HOOKS); relcheck OK"
    echo "burn      Anyka_MicroSys_Uart_Burn_Tool: import config_ozkie.txt (UTF-16LE), Auto Burn, BOOT + reset"
    echo "sha256"
    (cd "$out" && shasum -a 256 ozkie_app.bin ozkv_blank.bin config_ozkie.txt)
} > "$out/MANIFEST.txt"
(cd "$reldir" && zip -q -r "$(basename "$out").zip" "$(basename "$out")")
cat "$out/MANIFEST.txt"
echo "package: $reldir/$(basename "$out").zip"
