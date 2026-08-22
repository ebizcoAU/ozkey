#!/usr/bin/env python3
"""
Generate `common/ozprofile_gen.h` and `profiles/models.json` from `profiles/`.

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

WHY THERE IS A SECOND OUTPUT (`profiles/models.json`)
-----------------------------------------------------
XF-122 §7: at pairing the app shows "Detected: <model> — is this correct?", so
it must turn a `tuya_pid` off the wire into a human model name, and must know
which models it supports at all. That mapping is the same fact the DP tables
encode — which PID is which product — so if the app hardcodes its own copy we
get a fourth independent idea of what a lock is, which is the exact failure the
profile layer exists to prevent (ozkey-27 §2.1, and see `our-dp-map-is-invented`).

So it is emitted here, from the same load+resolve pass as the C tables. The
manifest carries IDENTITY only — PID, names, pairability. It deliberately does
NOT carry DP numbers or verbs: the app learns those per-lock from the `verbs`
array at enrol (XF-121), from the lock itself, which is the one source that
cannot be stale.

USAGE
  python3 tools/gen_profile.py            # write both outputs
  python3 tools/gen_profile.py --check    # exit 1 if either output is stale
"""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILES = ROOT / "profiles"
OUT = ROOT / "blelock" / "common" / "ozprofile_gen.h"
OUT_MODELS = PROFILES / "models.json"

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


# The (verb, field) pairs a command may target. MUST stay identical to
# `kProbe[][2]` in ozdoorlock_core.h's enrol builder — XF-123 P1.4 accepts the
# import only if Nexus's stored `verbs` matches what the lock reports, and the
# two lists are compared directly.
VERB_PROBE = [
    ("lock.unlock", None),          ("cred.put", "pin"),
    ("cred.put", "rfid"),           ("cred.delete", "pin"),
    ("cred.delete", "rfid"),        ("cred.sync", None),
    ("lock.settings.set", "autolock"), ("lock.settings.set", "volume"),
]


def verb_rows(entries):
    """Every (verb, field, dir) -> DP this profile resolves, sorted by policy.

    Extracted so the C tables and models.json are built from ONE computation.
    Two independent derivations of the same verb map is precisely the failure
    this layer exists to prevent — it is how firmware and LockSim came to
    disagree about DP numbers in the first place (ozkey-27 §2.1).
    """
    rows = []
    for e in entries:
        if e.get("verb_down"):
            rows.append((e["verb_down"], e.get("field_down"), "OZ_DIR_DOWN", e))
        if e.get("verb"):
            rows.append((e["verb"], e.get("field"), "OZ_DIR_UP", e))
    # Sort by (verb, STATUS, field) — status BEFORE field, deliberately.
    # A bare `lock.unlock` must land on DP 76 (confirmed), not DP 10 (reserved,
    # payload layout never supplied — ozkey-42 P0). Sorting by field first
    # happened to give the right answer only because "ble" < "remote"
    # alphabetically; that is luck, not policy. The resolver returns the first
    # match, so the order here IS the policy.
    rows.sort(key=lambda r: (r[0], STATUS[r[3]["status"]], r[1] or ""))
    return rows


def resolve_verb(rows, verb, field):
    """Mirror of ozResolveVerb(..., OZ_DIR_DOWN) in ozprofile.h — first match wins.

    🔴 The field rule is asymmetric, and getting it wrong is silent. Quoting the
    C: "A caller that names no field takes the first (best-status) candidate. A
    caller that names one must match it exactly." So `field=None` means ANY
    field, NOT "a row whose field is also null".

    An earlier version of this function required an exact None-to-None match,
    which made a bare `lock.unlock` resolve to nothing on Luona — whose DP 76
    carries `field_down: "ble"` — and the manifest reported a lock with no
    unlock capability, three days after we watched DP 76 open a real door.
    """
    for v, f, d, e in rows:
        if d != "OZ_DIR_DOWN" or v != verb:
            continue
        if field and f != field:
            continue
        return e
    return None


def derive_verbs_and_caps(rows):
    """XF-123 §16/§17 — the manifest's advisory copy of what a model can do.

    DERIVED, never asserted, and by the same rule the firmware uses: a verb
    counts only if it resolves to a CONFIRMED DP (ozVerbUsable() == status
    confirmed). `reserved` means the supplier documented the DP's type and not
    its payload layout, so we cannot build a frame — promising the capability
    would be the XF-121 bug moved into the generator. In practice this is why
    NO profile we hold advertises `pin_sync`: every credential-write DP on a
    real supplier map is `reserved` (genericDPList §3.1).
    """
    verbs = []
    for verb, field in VERB_PROBE:
        e = resolve_verb(rows, verb, field)
        if not e or e["status"] != "confirmed":
            continue
        row = {"verb": verb}
        if field:
            row["field"] = field
        row["dp"] = e["dp"]
        verbs.append(row)

    can = lambda v, f: any(x["verb"] == v and x.get("field") == f for x in verbs)
    caps = []
    # 🔴 remote_unlock vs assisted_unlock is a TRANSPORT decision the firmware
    # makes per unit (`isThread() ? "remote_unlock" : "assisted_unlock"`), and
    # transport is not a property of the model. The manifest therefore reports
    # the profile-derivable form; a Wi-Fi lock will report `assisted_unlock`
    # for the same model. Flagged to Nexus in §18 so P1.4 does not read that
    # legitimate difference as an import mismatch.
    if can("lock.unlock", None):
        caps.append("remote_unlock")
    if can("cred.put", "pin"):
        caps.append("pin_sync")
    if can("cred.put", "rfid"):
        caps.append("rfid_sync")
    # Ours, not the MCU's: the txlog is written by firmware and depends on no DP.
    caps.append("audit")
    return verbs, caps


# XF-124 §4 — the registry keys every device type, not just locks. Everything
# `profiles/` describes is a DOORLOCK DP map by construction: a bridge or a
# devkit has no Tuya DP profile at all, so they register with Nexus by another
# route and never appear in models.json. Constant here rather than a per-product
# field precisely so a lock profile cannot claim to be something else.
#
# 🔴 Must match OZ_DEVICE_TYPE in the doorlock sketches. The bridge defines its
# own ("bridge") and does not read this file.
DEVICE_TYPE = "doorlock"


def firmware_id(prod, catalogue_rev):
    """XF-124 F2 — `fw-{device_type}-{profile-id}-r{rev}`, NO build date.

    SUPERSEDES XF-123's `fw-{profile-id}-r{rev}` (shipped 2026-08-21 06:22),
    which had no device-type segment because that spec covered doorlocks only.
    XF-124 makes Nexus a universal registry across doorlock/bridge/devkit/iot,
    so the key has to say what kind of thing it names.

    It names WHAT THE LOCK IS, not when it compiled. The spec's first draft was
    `fw-{build-date}-{profile-id}`, which changes on every rebuild of an
    unchanged profile: every rebuild would need a new Nexus row, and a lock
    enrolled last week would report an id no longer matching any row, so the
    `firmware_id` stored against it (XF-123 §6.4) decays into a dead key.
    Rejected in firmware review §13.1 and now a Key Decision.

    WHICH rev (XF-123 §15.2, resolved 2026-08-21). Not the catalogue's, blindly.
    The id must change exactly when THIS profile's DP map changes, and
    `catalogue_rev` alone fails that twice:

      1. a product changing its `selects` list changes its DP map while the
         catalogue does not move — the id would stay put and the app would keep
         a cached model that no longer describes the lock;
      2. a `standalone` profile does not use the catalogue AT ALL. Stamping
         `tuya-wifi-lock-pro` with the catalogue rev pointed at a revision with
         no bearing on its contents.

    So: a standalone profile is versioned by its OWN rev; a selecting profile by
    whichever of the two moved last, since either can change what it dispatches.

    The build instant is still reported, separately and accurately, as `fw`
    (`doorlock-2.15`) — P3.6.
    """
    prod_rev = int(prod.get("rev", 0))
    rev = prod_rev if prod.get("standalone") else max(catalogue_rev, prod_rev)
    return f"fw-{DEVICE_TYPE}-{prod['profile_id']}-r{rev}"


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
    w("  // XF-123 P3.4 — reported at enrol, the key Nexus stores a model under.")
    w("  // `fw-{profile-id}-r{catalogue_rev}`: it identifies WHAT THIS LOCK IS.")
    w("  // Deliberately NOT the build instant — that stays in `fw`, separately")
    w("  // (P3.6), because an id that changes on every rebuild strands the")
    w("  // firmware_id already stored against an enrolled lock (§13.1).")
    w("  const char       *firmware_id;")
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
                      (prod.get("supplier") or {}).get("pid"),
                      firmware_id(prod, int(catalogue.get("rev", 0)))))
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
        rows = verb_rows(entries)  # shared with models.json — see verb_rows()
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
    for pid, ident, dep, rev, tuya_pid, fwid in names:
        tp = f'"{tuya_pid}"' if tuya_pid else "nullptr"
        w(f'  {{ "{pid}", {rev}, OZ_DP_{ident}, '
          f"(uint16_t)(sizeof(OZ_DP_{ident}) / sizeof(OzDpEntry)), {str(dep).lower()}, {tp}, "
          f'"{fwid}", OZ_VERBS_{ident}, {verb_counts[ident]} }},')
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


def generate_models():
    """The app-facing identity manifest — XF-122 §9. See the module docstring."""
    catalogue = load(PROFILES / "tuya-lock-catalogue.json")
    models = []

    for path in sorted((PROFILES / "products").glob("*.json")):
        prod = load(path)
        sup = prod.get("supplier") or {}
        pid = sup.get("pid")

        # PAIRABILITY IS DERIVED, NEVER ASSERTED — the XF-121 rule that `caps`
        # must be learned rather than promised, applied to the model list. A
        # profile becomes pairable by having the two things pairing needs, not
        # by carrying a flag someone remembered to set.
        #
        #   1. a PID, or PID discovery has nothing to match and the profile can
        #      never be proposed (XF-122 §7 rejected manual override, so a
        #      profile that cannot be detected cannot be chosen at all);
        #   2. a complete DP map, or we would pair onto a map we know is partial.
        #
        # 🔴 DO NOT INVENT THE REASON. `complete: false` means different things
        # in different profiles — Ladin has no DP reference at all, while Wi-Fi
        # Lock Pro's DP LIST is complete and only the RAW byte widths are
        # missing. An earlier draft of this generator rendered both as "the
        # supplier has not supplied a DP reference", which is false for the
        # second. So state only what the flag asserts and carry the profile's
        # own `source.note` / `blocked_by` for the actual cause.
        blockers = []
        if not pid:
            blockers.append(
                "no tuya_pid — PID discovery has nothing to match, and XF-122 §7 "
                "allows no manual override, so this profile can never be proposed")
        if prod.get("complete") is False:
            blockers.append("DP map is marked incomplete — see `source_note`")
        if prod.get("blocked_by"):
            blockers.append(f"blocked by {prod['blocked_by']}")

        manufacturer = sup.get("manufacturer")
        product = sup.get("product")
        model_verbs, model_caps = derive_verbs_and_caps(
            verb_rows(resolve(prod, catalogue)))
        models.append({
            # XF-123 §4 — the Nexus PRIMARY KEY, so it leads the object.
            "firmware_id": firmware_id(prod, int(catalogue.get("rev", 0))),
            # XF-124 F3 — the registry column that lets Nexus serve
            # ?type=doorlock. Constant for everything in profiles/; see
            # DEVICE_TYPE's comment for why it is not a per-product field.
            "device_type": DEVICE_TYPE,
            "profile_id": prod["profile_id"],
            "tuya_pid": pid,
            "manufacturer": manufacturer,
            "product": product,
            # What XF-122 §7's "Detected: <model>" line should render. Joined
            # here rather than in the app so every consumer shows one string.
            "display_name": " — ".join(x for x in (manufacturer, product) if x),
            "category": sup.get("category"),
            "module": sup.get("module"),
            "rev": int(prod.get("rev", 0)),
            "dp_count": len(resolve(prod, catalogue)),
            "pairable": not blockers,
            "not_pairable_because": blockers or None,
            # XF-123 §16/§17 — ADVISORY. The lock's own `verbs` array at enrol
            # is authoritative (§13.3/§14.1); these exist so Nexus has something
            # to import and so a mismatch is VISIBLE rather than assumed.
            "verbs": model_verbs,
            "caps": model_caps,
            # The profile's own account of what its DP map is based on. Carried
            # verbatim so nobody has to infer the cause from a boolean.
            "source_note": (prod.get("source") or {}).get("note"),
        })

    pairable = [m for m in models if m["pairable"]]
    doc = {
        "$comment":
            "GENERATED FILE — DO NOT EDIT. Source: profiles/*.json.\n"
            "Regenerate: python3 blelock/tools/gen_profile.py\n"
            "Verify current: python3 blelock/tools/gen_profile.py --check\n\n"
            "WHAT THIS IS. The app-facing model manifest (XF-122 §9): how to turn "
            "a `tuya_pid` reported by a lock into a model name a human recognises, "
            "and which models can be paired at all. Generated from the same load "
            "pass as the firmware's PROGMEM DP tables, so the app's model list "
            "cannot drift from the DP maps the firmware actually runs.\n\n"
            "WHAT THIS IS NOT. Not a DP map and not a capability list. The app "
            "learns what a specific lock can do from the `verbs` array in its "
            "enrol payload (XF-121) — from the lock itself, which is the only "
            "source that cannot go stale. Do not infer capability from a model "
            "name here.\n\n"
            "`pairable` IS DERIVED from whether a profile has a PID and a "
            "complete DP map — it is not a flag anyone sets. A profile with "
            "`pairable: false` must not be offered to a user; per XF-122 §7 an "
            "unrecognised PID is refused outright rather than manually overridden.",
        "generated_from": "profiles/products/*.json",
        "catalogue_rev": int(catalogue.get("rev", 0)),
        # 🔴 An unpinned build is a CONFIGURATION ERROR (ozprofile_gen.h), not a
        # model. Surfaced so the app can recognise it: a lock enrolling with this
        # profile_id was flashed without PROFILE= and is running a GUESS. Treat
        # it as unconfigured firmware, never as a detected model.
        "unpinned_build_default_profile_id": "tuya-wifi-lock-pro",
        "pairable_count": len(pairable),
        "models": models,
    }
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def main():
    text = generate()
    models = generate_models()
    outputs = [(OUT, text), (OUT_MODELS, models)]

    if "--check" in sys.argv:
        for path, want in outputs:
            if not path.exists():
                print(f"STALE: {path} does not exist", file=sys.stderr)
                return 1
            if path.read_text() != want:
                print(f"STALE: {path} does not match profiles/ — run gen_profile.py",
                      file=sys.stderr)
                return 1
        print(f"ok: {OUT.name} and {OUT_MODELS.name} are current")
        return 0

    for path, want in outputs:
        path.write_text(want)
    print(f"wrote {OUT} ({len(text.splitlines())} lines)")
    print(f"wrote {OUT_MODELS} ({len(models.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
