/**
 * Version formatting.
 *
 * Small, but the failure it guards against is not: `bridge32-1.32` shipped an
 * LCD badge that had drifted from the real firmware version and told the
 * operator the wrong thing at the bench. These assertions pin the format and,
 * more importantly, pin that it comes from `package.json`.
 */

import assert from "node:assert/strict";
import { formatVersion, LOCKSIM_VERSION } from "../lib/version";
import pkg from "../package.json";

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

console.log("\nversion\n");

test("formats as V<major>.<minor> zero-padded, like doorlock-1.58", () => {
  assert.equal(formatVersion("1.0.0"), "V1.00");
  assert.equal(formatVersion("1.5.0"), "V1.05");
  assert.equal(formatVersion("1.58.0"), "V1.58");
  assert.equal(formatVersion("2.13.7"), "V2.13");
});

test("patch is not shown — a display tweak needs no version the firmware must match", () => {
  assert.equal(formatVersion("1.2.0"), formatVersion("1.2.99"));
});

test("tolerates a short or empty version string", () => {
  assert.equal(formatVersion("1"), "V1.00");
  assert.equal(formatVersion(""), "V0.00");
});

test("the exported constant is DERIVED from package.json, not typed twice", () => {
  assert.equal(LOCKSIM_VERSION, formatVersion(pkg.version));
});

test("package.json is on a 1.x version, so the badge reads V1.xx", () => {
  assert.match(LOCKSIM_VERSION, /^V1\.\d{2}$/, `got ${LOCKSIM_VERSION}`);
});

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed === 0 ? 0 : 1);
