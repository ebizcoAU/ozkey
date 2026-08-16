#!/usr/bin/env python3
"""
Generate `common/ozprofile_gen.h` from `profiles/`.

WHY GENERATE RATHER THAN PARSE
------------------------------
`profiles/` is the single source of truth and LockSim reads the JSON directly.
The firmware cannot: ArduinoJson is already linked, but parsing a profile at
boot would cost heap on a board where `bridge32-role-decisions` records that
RAM, not flash, is the ceiling — and the doorlock is tighter than the bridge.

So the JSON is compiled to flat PROGMEM tables. One source of truth, no runtime
parse, no heap. The cost is that the header can go stale, which is the exact
failure `bridge32-1.32` hit with a hand-maintained LCD version badge — so
`--check` exists and the test suite runs it.

USAGE
  python3 tools/gen_profile.py            # write common/ozprofile_gen.h
  python3 tools/gen_profile.py --check    # exit 1 if the header is stale
"""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILES = ROOT / "profiles"
OUT = ROOT / "blelock" / "common" / "ozprofile_gen.h"

# Only what dispatch actually needs. Anything else stays in the JSON for
# LockSim, the app and humans — the firmware does not need enum tables or
# blocked_by strings to decide whether to forward a frame.
DIR = {"up": 0, "down": 1, "both": 2}
STATUS = {"confirmed": 0, "reserved": 1, "unknown": 2, "fiction": 3}


def strip_comments(v):
    """`$`-prefixed keys are documentation; they must not reach the tables."""
    if isinstance(v, list):
        return [strip_comments(x) for x in v]
    if isinstance(v, dict):
        return {k: strip_comments(x) for k, x in v.items() if not k.startswith("$")}
    return v


def load(path):
    return strip_comments(json.loads(path.read_text()))


def resolve(product, catalogue):
    """Mirror of resolveProfile() in locksim/lib/profile.ts."""
    by_dp = {}
    if product.get("standalone"):
        for e in product.get("entries", []):
            by_dp[e["dp"]] = e
    else:
        cat = {e["dp"]: e for e in catalogue["entries"]}
        for dp in product.get("selects", []):
            if dp not in cat:
                raise SystemExit(f"{product['profile_id']}: selects DP {dp}, not in catalogue")
            by_dp[dp] = cat[dp]
        for o in product.get("overrides", []):
            if o["dp"] not in by_dp:
                raise SystemExit(f"{product['profile_id']}: overrides unselected DP {o['dp']}")
            by_dp[o["dp"]] = {**by_dp[o["dp"]], **o}
        for x in product.get("extra", []):
            if x["dp"] in by_dp:
                raise SystemExit(f"{product['profile_id']}: extra DP {x['dp']} duplicates a selection")
            by_dp[x["dp"]] = x
    return [by_dp[k] for k in sorted(by_dp)]


def c_ident(s):
    return s.replace("-", "_").replace(".", "_")


def generate():
    catalogue = load(PROFILES / "tuya-lock-catalogue.json")
    products = sorted((PROFILES / "products").glob("*.json"))

    out = []
    w = out.append
    w("// GENERATED FILE — DO NOT EDIT.")
    w("//")
    w("// Source: profiles/*.json. Regenerate with:")
    w("//     python3 blelock/tools/gen_profile.py")
    w("// Verify it is current with:")
    w("//     python3 blelock/tools/gen_profile.py --check")
    w("//")
    w("// Editing this file by hand reintroduces exactly the failure the profile")
    w("// layer exists to prevent: firmware and LockSim holding different ideas of")
    w("// what a DP number means (ozkey-27 §2.1).")
    w("#pragma once")
    w("#include <stdint.h>")
    w("")
    w("enum OzDpDir : uint8_t { OZ_DIR_UP = 0, OZ_DIR_DOWN = 1, OZ_DIR_BOTH = 2 };")
    w("enum OzDpStatus : uint8_t {")
    w("  OZ_DP_CONFIRMED = 0, // type AND payload semantics documented")
    w("  OZ_DP_RESERVED  = 1, // DP known, payload layout NOT supplied -> UNSUPPORTED")
    w("  OZ_DP_UNKNOWN   = 2, // seen but unestablished -> log id+len only")
    w("  OZ_DP_FICTION   = 3, // we invented it. ozkie-legacy-v0 only.")
    w("};")
    w("")
    w("struct OzDpEntry {")
    w("  uint16_t   dp;")
    w("  OzDpDir    dir;")
    w("  OzDpStatus status;")
    w("  const char *name;")
    w("};")
    w("")
    w("struct OzProfile {")
    w("  const char       *id;")
    w("  uint16_t          rev;   // ozkey-28 §3.6 — device.info reports this")
    w("  const OzDpEntry  *entries;")
    w("  uint16_t          count;")
    w("  bool              deprecated;")
    w("  // Tuya product ID, from profiles/products/*.json supplier.pid.")
    w("  // This is what the DL MCU reports to command 0x01, so it is how a")
    w("  // lock identifies ITSELF instead of being told what it is.")
    w("  // nullptr where we have no PID (our own invented map).")
    w("  const char       *tuya_pid;")
    w("};")
    w("")

    names = []
    for path in products:
        prod = load(path)
        entries = resolve(prod, catalogue)
        ident = c_ident(prod["profile_id"])
        names.append((prod["profile_id"], ident, prod.get("deprecated", False),
                      int(prod.get("rev", 0)),
                      (prod.get("supplier") or {}).get("pid")))
        w(f"// {prod['profile_id']} — {len(entries)} DPs"
          + (" — DEPRECATED (invented map)" if prod.get("deprecated") else ""))
        w(f"static const OzDpEntry OZ_DP_{ident}[] = {{")
        for e in entries:
            w(f'  {{ {e["dp"]:>3}, {list(DIR)[DIR[e["dir"]]].upper() and "OZ_DIR_" + e["dir"].upper():<11}, '
              f'{"OZ_DP_" + e["status"].upper():<16}, "{e["name"]}" }},')
        w("};")
        w("")

    w("static const OzProfile OZ_PROFILES[] = {")
    for pid, ident, dep, rev, tuya_pid in names:
        tp = f'"{tuya_pid}"' if tuya_pid else "nullptr"
        w(f'  {{ "{pid}", {rev}, OZ_DP_{ident}, '
          f"(uint16_t)(sizeof(OZ_DP_{ident}) / sizeof(OzDpEntry)), {str(dep).lower()}, {tp} }},")
    w("};")
    w(f"static const uint8_t OZ_PROFILE_COUNT = {len(names)};")
    w("")
    w("// The profile the firmware boots with.")
    w("//")
    w("// 🔴 DELIBERATELY the invented map: doorlock-1.58 ships it and the current")
    w("// BANOI build constructs DP 21 frames against it, so defaulting to the real")
    w("// catalogue would break the bench and the app on the next flash. Staged")
    w("// migration, no flag day (ozkey-28 §1.1).")
    w('#define OZ_PROFILE_DEFAULT_ID "ozkie-legacy-v0"')
    w("")
    return "\n".join(out) + "\n"


def main():
    text = generate()
    if "--check" in sys.argv:
        if not OUT.exists():
            print(f"STALE: {OUT} does not exist", file=sys.stderr)
            return 1
        if OUT.read_text() != text:
            print(f"STALE: {OUT} does not match profiles/ — run gen_profile.py", file=sys.stderr)
            return 1
        print(f"ok: {OUT.name} is current")
        return 0
    OUT.write_text(text)
    print(f"wrote {OUT} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
