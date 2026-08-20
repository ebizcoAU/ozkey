/**
 * Profile resolver tests.
 *
 * The point of these is narrower than "does the code work". The bench's job is
 * to be able to FAIL — and the specific failure this whole layer exists to
 * prevent is LockSim and the doorlock firmware quietly agreeing on a DP number
 * that no supplier uses. So the assertions that matter most are the ones that
 * pin the REAL catalogue meanings of 21/23/24/60 and prove the invented map is
 * flagged as fiction rather than silently loadable.
 */

import assert from "node:assert/strict";
import {
  resolveProfile,
  dispositionFor,
  enumName,
  enumValue,
  usableEntries,
  blockedEntries,
  ProfileError,
  type Catalogue,
  type ProductProfile,
} from "../lib/profile";
import { loadProfile, listProducts, loadCatalogue, loadProduct } from "../lib/profileLoad";
import { PROFILES, getProfile, DEFAULT_PROFILE_ID } from "../lib/profileRegistry";

let passed = 0;
let failed = 0;

function test(name: string, fn: () => void) {
  try {
    fn();
    passed++;
    console.log(`  ok   ${name}`);
  } catch (e) {
    failed++;
    console.error(`  FAIL ${name}\n       ${(e as Error).message}`);
  }
}

console.log("\nprofile resolver\n");

// ── resolution mechanics ────────────────────────────────────────────────────

const miniCat: Catalogue = {
  catalogue_id: "mini",
  rev: 1,
  sources: [],
  entries: [
    { dp: 1, name: "a", type: "bool", dir: "both", status: "confirmed", verb: "v.a" },
    { dp: 2, name: "b", type: "value", dir: "up", status: "confirmed", verb: "v.b", range: [0, 9] },
    { dp: 3, name: "c", type: "raw", dir: "down", status: "reserved", blocked_by: "no layout" },
  ],
};

test("selects a subset of the catalogue", () => {
  const p = resolveProfile({ profile_id: "t", rev: 1, catalogue: "mini", selects: [1, 3] }, miniCat);
  assert.deepEqual(
    p.entries.map((e) => e.dp),
    [1, 3]
  );
});

test("entries come back sorted by dp", () => {
  const p = resolveProfile(
    { profile_id: "t", rev: 1, catalogue: "mini", selects: [3, 1, 2] },
    miniCat
  );
  assert.deepEqual(
    p.entries.map((e) => e.dp),
    [1, 2, 3]
  );
});

test("override narrows a selected entry without forking it", () => {
  const p = resolveProfile(
    {
      profile_id: "t",
      rev: 1,
      catalogue: "mini",
      selects: [2],
      overrides: [{ dp: 2, name: "b", type: "value", dir: "up", status: "confirmed", range: [0, 4] }],
    },
    miniCat
  );
  assert.deepEqual(p.byDp.get(2)!.range, [0, 4]);
  assert.equal(p.byDp.get(2)!.verb, "v.b", "unspecified fields survive the merge");
});

test("selecting a DP absent from the catalogue throws", () => {
  assert.throws(
    () => resolveProfile({ profile_id: "t", rev: 1, catalogue: "mini", selects: [99] }, miniCat),
    ProfileError
  );
});

test("overriding an unselected DP throws — it is a typo, not a feature", () => {
  assert.throws(
    () =>
      resolveProfile(
        {
          profile_id: "t",
          rev: 1,
          catalogue: "mini",
          selects: [1],
          overrides: [{ dp: 2, name: "b", type: "value", dir: "up", status: "confirmed" }],
        },
        miniCat
      ),
    ProfileError
  );
});

test("extra duplicating a selected DP throws", () => {
  assert.throws(
    () =>
      resolveProfile(
        {
          profile_id: "t",
          rev: 1,
          catalogue: "mini",
          selects: [1],
          extra: [{ dp: 1, name: "dup", type: "bool", dir: "both", status: "confirmed" }],
        },
        miniCat
      ),
    ProfileError
  );
});

test("non-standalone profile without a catalogue throws", () => {
  assert.throws(
    () => resolveProfile({ profile_id: "t", rev: 1, catalogue: "mini", selects: [1] }),
    ProfileError
  );
});

test("byVerb groups the several DPs that serve one verb", () => {
  const cat: Catalogue = {
    catalogue_id: "m",
    rev: 1,
    sources: [],
    entries: [
      { dp: 61, name: "p", type: "value", dir: "up", status: "confirmed", verb: "event.access" },
      { dp: 63, name: "f", type: "value", dir: "up", status: "confirmed", verb: "event.access" },
    ],
  };
  const p = resolveProfile({ profile_id: "t", rev: 1, catalogue: "m", selects: [61, 63] }, cat);
  assert.equal(p.byVerb.get("event.access")!.length, 2);
});

// ── disposition — the gate that replaces ozDpForwardable() ──────────────────

const gate = resolveProfile(
  { profile_id: "g", rev: 1, catalogue: "mini", selects: [1, 2, 3] },
  miniCat
);

test("confirmed DP in the right direction is handled", () => {
  assert.equal(dispositionFor(gate, 1, "down").action, "handle");
});

test("direction is enforced — an up-only DP written downstream is rejected", () => {
  assert.equal(dispositionFor(gate, 2, "down").action, "reject");
  assert.equal(dispositionFor(gate, 2, "up").action, "handle");
});

test("reserved DP is UNSUPPORTED, never silently forwarded", () => {
  const d = dispositionFor(gate, 3, "down");
  assert.equal(d.action, "unsupported");
  assert.match((d as { reason: string }).reason, /no layout/);
});

test("DP absent from the profile is rejected, not forwarded", () => {
  assert.equal(dispositionFor(gate, 77, "up").action, "reject");
});

test("raw entry with no codec is UNSUPPORTED even if marked confirmed", () => {
  const cat: Catalogue = {
    catalogue_id: "m",
    rev: 1,
    sources: [],
    entries: [{ dp: 5, name: "r", type: "raw", dir: "both", status: "confirmed" }],
  };
  const p = resolveProfile({ profile_id: "t", rev: 1, catalogue: "m", selects: [5] }, cat);
  const d = dispositionFor(p, 5, "down");
  assert.equal(d.action, "unsupported");
  assert.match((d as { reason: string }).reason, /no codec/);
});

// ── the real files on disk ──────────────────────────────────────────────────

console.log("\nshipped profiles\n");

test("every product profile on disk resolves", () => {
  const ids = listProducts();
  assert.ok(ids.length >= 3, `expected >=3 products, found ${ids.length}`);
  for (const id of ids) loadProfile(id);
});

test("catalogue has no duplicate DP ids", () => {
  const cat = loadCatalogue("tuya-lock-catalogue");
  const seen = new Set<number>();
  for (const e of cat.entries) {
    assert.ok(!seen.has(e.dp), `duplicate DP ${e.dp} in catalogue`);
    seen.add(e.dp);
  }
});

test("every catalogue entry names its Tuya name and a status", () => {
  for (const e of loadCatalogue("tuya-lock-catalogue").entries) {
    assert.ok(e.name, `DP ${e.dp} has no name`);
    assert.ok(e.status, `DP ${e.dp} has no status`);
  }
});

test("every `reserved` entry says what it is blocked on", () => {
  for (const e of loadCatalogue("tuya-lock-catalogue").entries) {
    if (e.status === "reserved") {
      assert.ok(e.blocked_by, `DP ${e.dp} (${e.name}) is reserved with no blocked_by`);
    }
  }
});

// ── 🔴 the regression guards this layer exists for ──────────────────────────
//
// If any of these four ever fail, someone has re-introduced the invented map
// into the real catalogue. They are the whole reason the profile split exists.

console.log("\nDS013-T3 — the real catalogue meanings\n");

const ds013 = loadProfile("tuya-luona-ds013-t3");

test("DP 21 is navigation_volume, NOT add_temp_pin", () => {
  const e = ds013.byDp.get(21)!;
  assert.equal(e.name, "navigation_volume");
  assert.equal(e.type, "enum");
  assert.equal(enumName(e, 0), "mute");
  assert.equal(enumValue(e, "high"), 3);
});

test("DP 23 is auto_lock (bool), NOT add_temp_rfid", () => {
  const e = ds013.byDp.get(23)!;
  assert.equal(e.name, "auto_lock");
  assert.equal(e.type, "bool");
});

test("DP 24 is auto_lock_delay 5-1800s, NOT delete_rfid", () => {
  const e = ds013.byDp.get(24)!;
  assert.equal(e.name, "auto_lock_delay");
  assert.deepEqual(e.range, [5, 1800]);
});

test("DP 60 is the 18-value alarm enum, NOT a pairing gesture", () => {
  const e = ds013.byDp.get(60)!;
  assert.equal(e.name, "alarm");
  assert.equal(e.verb, "event.alarm");
  assert.equal(Object.keys(e.enum!).length, 18);
  assert.equal(enumName(e, 14), "pry");
  assert.equal(enumName(e, 10), "low_battery");
});

test("DP 22 is not allocated at all on DS013-T3", () => {
  assert.equal(ds013.byDp.get(22), undefined);
});

test("our fictional DPs 1/2/3/5/8 are absent from the real profile", () => {
  for (const dp of [1, 2, 3, 5, 8]) {
    assert.equal(ds013.byDp.get(dp), undefined, `DP ${dp} should not exist on DS013-T3`);
  }
});

test("every credential write is reserved — nothing can be sent yet", () => {
  for (const dp of [13, 14, 15, 16, 17, 18, 19, 86]) {
    const d = dispositionFor(ds013, dp, "down");
    assert.equal(d.action, "unsupported", `DP ${dp} should be unsupported until ozkey-27 Q2 lands`);
  }
});

test("the whole event surface IS usable — only credential writes are blocked", () => {
  for (const dp of [45, 47, 52, 53, 60, 61, 63, 64, 69, 72, 73, 76, 98]) {
    assert.equal(dispositionFor(ds013, dp, "up").action, "handle", `DP ${dp} should be handled`);
  }
});

test("all settings are usable", () => {
  for (const dp of [11, 21, 23, 24, 42, 156]) {
    assert.equal(dispositionFor(ds013, dp, "down").action, "handle", `DP ${dp} should be handled`);
  }
});

test("duress is its own verb, never folded into event.alarm", () => {
  assert.equal(ds013.byDp.get(98)!.verb, "event.duress");
});

test("access DPs each name their credential class", () => {
  const kinds = ds013
    .byVerb.get("event.access")!
    .filter((e) => e.access_kind)
    .map((e) => e.access_kind);
  for (const k of ["pin", "fingerprint", "rfid", "temp_pin", "remote", "ble"]) {
    assert.ok(kinds.includes(k), `missing access_kind ${k}`);
  }
});

test("command variant is honestly marked unresolved", () => {
  assert.equal((ds013.cmd as { variant: string }).variant, "unresolved");
});

test("MCU-side credential expiry is recorded as a capability", () => {
  assert.equal((ds013.features as { credential_expiry_on_mcu: boolean }).credential_expiry_on_mcu, true);
  assert.equal((ds013.features as { offline_record_buffer: number }).offline_record_buffer, 20);
});

console.log("\nozkie-legacy-v0 — DELETED 2026-08-20\n");

// The invented map is gone. It defined DP 1 as unlock and DP 21-24 as
// credential CRUD; on every real product those numbers mean something else
// (DP 21 is navigation_volume on Luona, liftup_double_lock on Tuya's standard).
// Keeping it as a "safe default" meant every unpinned lock spoke a language no
// manufacturer implements. The tests that pinned its shape went with it —
// asserting the exact contents of a fiction is not a regression guard, it is a
// commitment to the fiction.
//
// What replaced them: the collision tests in the tuya-wifi-lock-pro block,
// which assert that the SAME DP means different things per product. That is the
// fact worth protecting.

console.log("\npartial profiles\n");

test("the Ladin profile is flagged incomplete — we have no DP reference from them", () => {
  assert.equal(loadProduct("tuya-ladin-f7-t3").complete, false);
});

test("DP 42 and 76 agree between Luona and Ladin — both are T3-U modules, NOT evidence of a universal standard", () => {
  const video = loadProfile("tuya-ladin-f7-t3");
  assert.equal(video.byDp.get(42)!.name, ds013.byDp.get(42)!.name);
  assert.equal(video.byDp.get(76)!.name, ds013.byDp.get(76)!.name);
  assert.equal(video.byDp.get(76)!.name, "unlock_ble");
});

// ── firmware parity ─────────────────────────────────────────────────────────
//
// The firmware's dispatch gate (blelock/common/ozprofile.h) replaced a
// hardcoded `dp == 1 || (dp >= 21 && dp <= 24)` with a profile lookup. That is
// only a safe refactor if the legacy profile reproduces the old allow-list
// EXACTLY — and the doorlock asserts it on every boot (ozM4SelfTest). Assert
// the same thing here, so the two ends cannot drift apart silently.

test("DP 1 is not an unlock command on ANY surviving profile", () => {
  // Replaces "legacy profile reproduces the old allow-list": that test pinned
  // the shape of our invented map, which was deleted 2026-08-20. The fact worth
  // guarding now is the opposite one — the fiction must not come back.
  for (const prof of PROFILES) {
    const dp1 = prof.byDp.get(1);
    if (!dp1) continue;
    assert.notEqual(dp1.verb_down, "lock.unlock", `${prof.profile_id}: DP 1 must not be an unlock command`);
  }
});

test("under the REAL profile the old allow-list would have been inverted", () => {
  const real = loadProfile("tuya-luona-ds013-t3");
  const forwardable = (dp: number) => dispositionFor(real, dp, "down").action === "handle";
  // The settings DPs the old constant happened to permit...
  for (const dp of [21, 23, 24]) assert.ok(forwardable(dp), `DP ${dp} is a real setting`);
  // ...while every credential write it was written to permit is blocked.
  for (const dp of [13, 16, 17]) assert.ok(!forwardable(dp), `DP ${dp} must be blocked`);
  assert.equal(real.byDp.get(22), undefined, "DP 22 is not allocated at all");
});

// ── the generic pre-flashed Tuya DL-MCU ─────────────────────────────────────
//
// docs/DPSuppliers/genericDPList.md. The operator's read is that most makers
// buy a pre-flashed Tuya DL-MCU and tick standard boxes at PID creation, so
// the interesting question is not "what did THIS vendor invent" but "what does
// the standard set do to us". These pin the answer so it cannot drift.

console.log("\ntuya-wifi-lock-pro — Tuya's own standard DP map\n");

// 🔴 THESE TESTS REPLACED THE `tuya-generic-lock` BLOCK, 2026-08-20, and the
// reason matters more than the assertions.
//
// `tuya-generic-lock` claimed to be "the standard Tuya lock DP map". It was
// not: it was Luona's DS013-T3 map with the PID stripped out. Checking Tuya's
// own published references showed there IS no universal lock DP numbering —
// numbers are assigned PER PRODUCT CATEGORY at PID creation:
//
//   DP 76  fill_light (Wi-Fi Lock Pro)  vs  unlock_ble (Luona DS013-T3)
//   DP 16  duress_alarm                 vs  bulk_password_add
//   DP 21  liftup_double_lock           vs  navigation_volume
//
// So a "generic" profile is a category error, and using one as a fallback for
// an unidentified lock could unlock a door by switching on a lamp. This file
// now holds the REAL Tuya standard, standalone, and asserts the collisions so
// nobody re-derives the mistake.

const wifipro = loadProfile("tuya-wifi-lock-pro");

test("it is STANDALONE — it cannot share the vendor catalogue", () => {
  // The catalogue is Luona-derived. Selecting from it would mean one of the two
  // maps was silently wrong about every colliding DP.
  const cat = loadCatalogue("tuya-lock-catalogue").entries.map((e) => e.dp);
  const mine = wifipro.entries.map((e) => e.dp);
  assert.notDeepEqual(mine, cat, "a category map must not track a vendor catalogue");
  assert.ok(wifipro.entries.length > 30, "the standard set is substantial");
});

test("🔴 the DP collisions that make per-vendor profiles mandatory", () => {
  const luona = loadProfile("tuya-luona-ds013-t3");
  // The one that matters most: we ship remote unlock on DP 76.
  assert.equal(wifipro.byDp.get(76)!.name, "fill_light");
  assert.equal(luona.byDp.get(76)!.name, "unlock_ble");
  // And the credential DP we refuse on.
  assert.equal(wifipro.byDp.get(16)!.name, "duress_alarm");
  assert.equal(luona.byDp.get(16)!.name, "bulk_password_add");
});

test("🔴 it has NO PID — it is a CATEGORY, not a product", () => {
  // A real lock built to this standard has its own PID and its own profile.
  // Inventing one here would make discovery match a maker that does not exist.
  assert.equal(wifipro.tuya_pid, undefined);
  assert.equal(loadProfile("tuya-luona-ds013-t3").tuya_pid, "vr4iiuqtyh0q4nix");
});

test("it carries no fiction — DP 1 is ours, and is absent here", () => {
  // DP 1 on the standard map is `unlock_fingerprint`, a REPORT — never our
  // invented unlock command.
  const dp1 = wifipro.byDp.get(1);
  assert.equal(dp1!.name, "unlock_fingerprint");
  assert.equal(dp1!.dir, "up");
});
console.log("\ntwo loaders, one truth\n");

// The browser cannot read `profiles/` off disk, so profileRegistry.ts imports
// the same JSON statically. Two code paths to the same data is exactly how the
// fiction survived in the first place — so pin that they agree.
test("the browser registry resolves identically to the filesystem loader", () => {
  for (const fromBrowser of PROFILES) {
    const fromDisk = loadProfile(fromBrowser.profile_id);
    assert.deepEqual(
      fromBrowser.entries.map((e) => [e.dp, e.name, e.status, e.verb]),
      fromDisk.entries.map((e) => [e.dp, e.name, e.status, e.verb]),
      `${fromBrowser.profile_id} differs between the two loaders`
    );
    assert.deepEqual(fromBrowser.in_lock, fromDisk.in_lock);
  }
});

test("the registry strips $comment too — in_lock must hold only real DPIDs", () => {
  const keys = Object.keys(getProfile("tuya-luona-ds013-t3").in_lock).sort();
  assert.deepEqual(keys, ["101", "102", "103"]);
});

test("the default is Tuya's standard map — and an unpinned build is a guess", () => {
  assert.equal(DEFAULT_PROFILE_ID, "tuya-wifi-lock-pro");
});

// ── summary ─────────────────────────────────────────────────────────────────

const usable = usableEntries(ds013).length;
const blocked = blockedEntries(ds013).length;
console.log(
  `\nDS013-T3: ${usable} usable / ${blocked} blocked of ${ds013.entries.length} DPs\n`
);
console.log(`${passed} passed, ${failed} failed\n`);
process.exit(failed === 0 ? 0 : 1);
