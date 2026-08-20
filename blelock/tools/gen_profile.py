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
# Tuya wire types, from the catalogue's `type`. Needed by the verb resolver so
# firmware can build a frame without a second table of its own.
DPTYPE = {"raw": 0x00, "bool": 0x01, "value": 0x02, "string": 0x03,
          "enum": 0x04, "bitmap": 0x05}


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
    # ── XF-120 / PM directive 2026-08-20 — the OZKIE verb resolver ──────────
    #
    # WHY THIS TABLE EXISTS. Firmware used to pick the DP for a verb in C:
    # `if (isUnlock) { if (ozDpFind(76)) dp = 76; else dp = 1; }`. That is a DP
    # number compiled into logic, so a second supplier whose unlock DP is not 76
    # needs a firmware change — which makes "profile-driven" aspirational rather
    # than true, and it is how DP 1 (a number we invented) survived this long.
    #
    # The catalogue already carried `verb` for the UP direction. rev 2 adds
    # `verb_down`/`field_down` for the DOWN direction, because a DP can be both:
    # DP 76 reports `event.access` upward and accepts `lock.unlock` downward.
    #
    # Resolution is on (verb, field, dir) because verb alone is ambiguous —
    # `cred.put` serves DP 13/16/86 and `lock.settings.set` serves seven DPs.
    # Verified unique across all 20 commandable DPs at catalogue rev 2.
    w("struct OzVerbMap {")
    w("  const char *verb;   // OZKIE verb, e.g. \"lock.unlock\"")
    w("  const char *field;  // sub-type, e.g. \"pin\"/\"ble\"; nullptr if none")
    w("  OzDpDir     dir;    // OZ_DIR_DOWN = command, OZ_DIR_UP = report")
    w("  uint16_t    dp;")
    w("  uint8_t     type;   // Tuya wire type — 0x00 RAW .. 0x05 BITMAP")
    w("  OzDpStatus  status; // RESERVED here means: known DP, unusable payload")
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
    w("  // Verb resolver table for THIS product — see OzVerbMap.")
    w("  const OzVerbMap  *verbs;")
    w("  uint16_t          verb_count;")
    w("};")
    w("")

    names = []
    verb_counts = {}
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

        # ── the verb table for this product ─────────────────────────────────
        #
        # Built from the SAME resolved entry list as the DP table above, so a
        # verb can never resolve to a DP this product does not select. That is
        # the whole point: one resolution, one source.
        rows = []
        for e in entries:
            if e.get("verb_down"):
                rows.append((e["verb_down"], e.get("field_down"), "OZ_DIR_DOWN", e))
            if e.get("verb"):
                rows.append((e["verb"], e.get("field"), "OZ_DIR_UP", e))
        # Sort by (verb, STATUS, field) — status BEFORE field, deliberately.
        # A bare `lock.unlock` with no field must land on DP 76 (confirmed),
        # not DP 10 (reserved, payload layout never supplied — ozkey-42 P0).
        # Sorting by field first happened to give the right answer only because
        # "ble" < "remote" alphabetically; that is luck, not policy.
        # A bare `lock.unlock` with no field must land on DP 76 (confirmed), not
        # DP 10 (reserved, payload layout never supplied — ozkey-42 P0). The
        # resolver returns the first match, so the order here IS the policy.
        rows.sort(key=lambda r: (r[0], STATUS[r[3]["status"]], r[1] or ""))
        # 🔴 UNIQUENESS IS A DOWN-DIRECTION INVARIANT ONLY.
        #
        # DOWN is verb -> DP: firmware is handed `{"kind":"unlock"}` and must
        # pick exactly one DP, so an ambiguous (verb, field) is a real defect
        # and is refused here at build time rather than guessed at runtime.
        #
        # UP is DP -> verb: firmware is handed a DP number by the MCU and asks
        # what it means. That direction cannot be ambiguous — a DP has one verb
        # — and it is NOT a (verb, field) lookup. Seven DPs legitimately report
        # `event.access` (password, fingerprint, card, temp, remote, voice,
        # ble); requiring those to be distinguishable BY VERB was my modelling
        # error, not a catalogue defect.
        seen = {}
        for v, f, d, e in rows:
            if d != "OZ_DIR_DOWN":
                continue
            k = (v, f)
            if k in seen:
                raise SystemExit(
                    f"{prod['profile_id']}: command {v}/{f} resolves to both DP "
                    f"{seen[k]} and DP {e['dp']} — ambiguous, fix the catalogue")
            seen[k] = e["dp"]
        w(f"static const OzVerbMap OZ_VERBS_{ident}[] = {{")
        for v, f, d, e in rows:
            fs = f'"{f}"' if f else "nullptr"
            w(f'  {{ "{v}", {fs:<22}, {d:<12}, {e["dp"]:>3}, '
              f'0x{DPTYPE[e["type"]]:02X}, {"OZ_DP_" + e["status"].upper()} }},')
        w("};")
        w("")
        verb_counts[ident] = len(rows)

    w("static const OzProfile OZ_PROFILES[] = {")
    for pid, ident, dep, rev, tuya_pid in names:
        tp = f'"{tuya_pid}"' if tuya_pid else "nullptr"
        w(f'  {{ "{pid}", {rev}, OZ_DP_{ident}, '
          f"(uint16_t)(sizeof(OZ_DP_{ident}) / sizeof(OzDpEntry)), {str(dep).lower()}, {tp}, "
          f"OZ_VERBS_{ident}, {verb_counts[ident]} }},")
    w("};")
    w(f"static const uint8_t OZ_PROFILE_COUNT = {len(names)};")
    w("")
    w("// The profile the firmware boots with when the build did not pin one.")
    w("//")
    w("// 🔴 AN UNPINNED BUILD IS NOW A CONFIGURATION ERROR, not a normal case.")
    w("// It used to default to `ozkie-legacy-v0`, our invented map, which was")
    w("// deleted 2026-08-20 — every lock now runs a REAL per-product profile.")
    w("//")
    w("// Tuya's own Wi-Fi Lock Pro map is the least-wrong default: it is a real")
    w("// published standard rather than a fiction. It is still a GUESS for any")
    w("// specific lock — DP 76 is `fill_light` here and `unlock_ble` on Luona —")
    w("// so ozProfileBegin() says so loudly at boot. Pin with PROFILE=.")
    w('#define OZ_PROFILE_DEFAULT_ID "tuya-wifi-lock-pro"')
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
