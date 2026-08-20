/**
 * The verb resolver exists in two languages, and they must never disagree.
 *
 * `resolveVerbDown()` (TypeScript, LockSim) and `ozResolveVerb()` (C, firmware)
 * read the same `profiles/` data and apply the same rule. If they drift, the
 * bench and the real lock mean different things by the same verb — which is
 * exactly the failure `profiles/` was built to end (ozkey-27 §2.1), and the
 * reason DP 1 survived long enough to reach real hardware.
 *
 * So this parses the GENERATED PROGMEM table (`ozprofile_gen.h`, firmware's
 * actual source of truth — not the JSON both sides happen to read) and asserts
 * the TypeScript resolver returns the same DP for the same query.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { getProfile, PROFILE_IDS } from "../lib/profileRegistry";
import { resolveVerbDown, verbUsable } from "../lib/profile";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const GEN = path.join(HERE, "..", "..", "blelock", "common", "ozprofile_gen.h");

/** Pull the DOWN rows out of the generated C, per profile, in table order. */
function progmemDownRows(profileId: string) {
  const src = fs.readFileSync(GEN, "utf8");
  const ident = profileId.replace(/[-.]/g, "_");
  const m = src.match(
    new RegExp(`static const OzVerbMap OZ_VERBS_${ident}\\[\\] = \\{([\\s\\S]*?)\\n\\};`)
  );
  assert.ok(m, `no generated verb table for ${profileId} — run gen_profile.py`);
  const rows: { verb: string; field: string | null; dp: number }[] = [];
  for (const line of m![1].split("\n")) {
    const r = line.match(
      /\{\s*"([^"]+)",\s*(nullptr|"[^"]*")\s*,\s*OZ_DIR_(UP|DOWN)\s*,\s*(\d+)/
    );
    if (!r || r[3] !== "DOWN") continue;
    rows.push({
      verb: r[1],
      field: r[2] === "nullptr" ? null : r[2].slice(1, -1),
      dp: Number(r[4]),
    });
  }
  return rows;
}

test("every generated DOWN row resolves identically in TypeScript", () => {
  for (const id of PROFILE_IDS) {
    const profile = getProfile(id);
    for (const row of progmemDownRows(id)) {
      const got = resolveVerbDown(profile, row.verb, row.field ?? undefined);
      assert.ok(
        got,
        `${id}: C resolves ${row.verb}/${row.field} to DP ${row.dp}, TS resolves nothing`
      );
      assert.equal(
        got!.dp,
        row.dp,
        `${id}: ${row.verb}/${row.field} — C says DP ${row.dp}, TS says DP ${got!.dp}`
      );
    }
  }
});

test("a bare verb takes the first generated row — the status ordering IS policy", () => {
  for (const id of PROFILE_IDS) {
    const profile = getProfile(id);
    const seen = new Set<string>();
    for (const row of progmemDownRows(id)) {
      if (seen.has(row.verb)) continue; // first row wins, both sides
      seen.add(row.verb);
      const got = resolveVerbDown(profile, row.verb);
      assert.equal(
        got?.dp,
        row.dp,
        `${id}: bare '${row.verb}' — C takes DP ${row.dp}, TS takes DP ${got?.dp}`
      );
    }
  }
});

test("unlock resolves to the real DP on a real profile, the fiction only on the fiction", () => {
  // The regression that cost 2026-08-20: DP 1 is ours and exists on no real
  // lock, so a real profile must never resolve unlock to it.
  const real = resolveVerbDown(getProfile("tuya-ds013-t3"), "lock.unlock");
  assert.equal(real?.dp, 76, "real profile must unlock via DP 76");
  assert.ok(verbUsable(real), "DP 76 must be usable — it is fully specified");

  const legacy = resolveVerbDown(getProfile("ozkie-legacy-v0"), "lock.unlock");
  assert.equal(legacy?.dp, 1, "the invented map still unlocks via DP 1");
});

test("DP 10 is reachable only when named, never as the default", () => {
  // DP 10 is the nominal remote-unlock DP and its payload layout has never
  // been supplied (ozkey-42 P0). It must never be what a bare verb selects.
  const p = getProfile("tuya-ds013-t3");
  assert.equal(resolveVerbDown(p, "lock.unlock")?.dp, 76);
  const named = resolveVerbDown(p, "lock.unlock", "remote");
  assert.equal(named?.dp, 10);
  assert.equal(verbUsable(named), false, "DP 10 must report as unusable");
});

test("credential writes refuse rather than misfire on a real profile", () => {
  // The bench defect: `grant_pin` wrote onto DP 21, which is `navigation_volume`
  // on a real lock, and both ends reported success (ozkey-42 §2.4.1).
  const p = getProfile("tuya-ds013-t3");
  const pin = resolveVerbDown(p, "cred.put", "pin");
  assert.equal(pin?.dp, 16, "a PIN goes to the real password DP, not DP 21");
  assert.equal(verbUsable(pin), false, "and is refused — layout not supplied");
  assert.notEqual(pin?.dp, 21, "DP 21 is navigation_volume; writing a PIN there is the bug");
});
