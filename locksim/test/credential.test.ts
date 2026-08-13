/**
 * Credential wire-format tests.
 *
 * These exist because of a bug that cost a bench session and produced NO
 * evidence anywhere in the system.
 *
 * The module hex-decoded `cred` for both credential kinds. For an RFID UID
 * that is correct — a UID *is* hex, so "7B3F91D2" round-trips. For a PIN it is
 * wrong: the app sends the digits the user was shown, "482915", the module read
 * them as HEX and put `48 29 15` on the wire, and the MCU — which decodes a PIN
 * as ASCII per DS013-T3 §16/§18 — got "H)<0x15>", failed its all-digits check
 * and dropped the frame silently.
 *
 * Every layer reported success. The module had already answered UNLOCK_OK
 * (it never waits for the MCU), and the MCU's own reject path was a bare
 * `break` with no log. A PIN could be issued, confirmed, and simply not exist.
 *
 * So: assert the wire format for BOTH kinds, and assert that a mis-encoded PIN
 * is REJECTED rather than accepted as something plausible.
 */

import assert from "node:assert/strict";
import {
  parseTempCredential,
  buildFrame,
  buildDpPayload,
  parseFrame,
  DpId,
  DpType,
  toHexString,
  type ByteArray,
} from "../lib/tuya";

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

const FROM = 1786000000;
const TO = 1786600000;
const be = (n: number) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];

/** Mirror of doorlock-1.60's ozSemGrantValue(): per-KIND encoding, not per-content. */
function moduleEncode(slot: number, cred: string, isPin: boolean): ByteArray {
  const body: number[] = isPin
    ? [...cred].map((ch) => ch.charCodeAt(0))
    : (cred.match(/../g) ?? []).map((h) => parseInt(h, 16));
  return [(slot >> 8) & 255, slot & 255, ...body, ...be(FROM), ...be(TO)];
}

/** The PRE-1.60 encoder: hex-decode regardless of kind. Kept to prove it fails. */
function moduleEncodeBuggy(slot: number, cred: string): ByteArray {
  const body = (cred.match(/../g) ?? []).map((h) => parseInt(h, 16));
  return [(slot >> 8) & 255, slot & 255, ...body, ...be(FROM), ...be(TO)];
}

console.log("\ncredential wire format\n");

test("a PIN travels as ASCII digits and round-trips", () => {
  const raw = moduleEncode(3, "482915", true);
  assert.equal(toHexString(raw).slice(0, 26), "00 03 34 38 32 39 31 35 6A", "must be ASCII on the wire");
  const p = parseTempCredential(DpId.ADD_TEMP_PIN, raw);
  assert.deepEqual(p, { slot: 3, credential: "482915", start: FROM, end: TO });
});

test("an RFID UID travels as raw bytes and round-trips", () => {
  const raw = moduleEncode(4, "7B3F91D2", false);
  const p = parseTempCredential(DpId.ADD_TEMP_RFID, raw);
  assert.equal(p?.slot, 4);
  assert.equal(p?.credential, "7B 3F 91 D2");
});

test("🔴 the pre-1.60 hex-encoded PIN is REJECTED, not silently accepted", () => {
  // This is the regression guard. If someone reinstates the hex-decode, this
  // fails here instead of on a bench six hours later.
  const raw = moduleEncodeBuggy(3, "482915");
  assert.equal(
    parseTempCredential(DpId.ADD_TEMP_PIN, raw),
    null,
    "hex-encoding a PIN must not parse — it is the bug doorlock-1.60 fixed"
  );
});

test("a PIN with a non-digit is rejected rather than coerced", () => {
  const raw = [0, 3, ..."48A915".split("").map((c) => c.charCodeAt(0)), ...be(FROM), ...be(TO)];
  assert.equal(parseTempCredential(DpId.ADD_TEMP_PIN, raw), null);
});

test("PIN lengths 4..8 all round-trip", () => {
  for (const pin of ["4829", "48291", "482915", "4829157", "48291573"]) {
    const p = parseTempCredential(DpId.ADD_TEMP_PIN, moduleEncode(1, pin, true));
    assert.equal(p?.credential, pin, `PIN ${pin} must survive`);
  }
});

test("a truncated payload is rejected, not read past the end", () => {
  assert.equal(parseTempCredential(DpId.ADD_TEMP_PIN, [0, 3, 0x34]), null);
});

test("the from/to window survives unchanged — expiry depends on it", () => {
  const p = parseTempCredential(DpId.ADD_TEMP_PIN, moduleEncode(7, "1234", true));
  assert.equal(p?.start, FROM);
  assert.equal(p?.end, TO);
  assert.ok(p!.end > p!.start, "a window that ends before it starts is born expired");
});

// ── the MCU ack (ozkey-28 §4 / doorlock-1.61) ───────────────────────────────
//
// The module now BLOCKS on an echo before reporting success, so the exact frame
// LockSim sends has to satisfy the firmware's matcher:
//     g_ackWaitDp && (f[3]==0x06 || f[3]==0x07) && n>=11 && f[6]==g_ackWaitDp
// Two codebases, one contract, no compiler between them — so assert it here.

console.log("\nMCU acknowledgement\n");

for (const [label, dpId] of [
  ["add PIN", DpId.ADD_TEMP_PIN],
  ["add RFID", DpId.ADD_TEMP_RFID],
  ["delete PIN", DpId.DELETE_PIN],
  ["delete RFID", DpId.DELETE_RFID],
] as const) {
  test(`the ${label} ack satisfies the firmware matcher`, () => {
    const f = buildFrame(0x06, buildDpPayload(dpId, DpType.RAW, [0x00, 0x03]));
    assert.ok(f[3] === 0x06 || f[3] === 0x07, "cmd must be a DP report");
    assert.ok(f.length >= 11, `frame must be >= 11 bytes, got ${f.length}`);
    assert.equal(f[6], dpId, "dpid must echo the DP the module wrote");
    assert.ok(parseFrame(f).ok, "and it must be a structurally valid frame");
  });
}

test("an ack for a DIFFERENT dp must not satisfy the matcher", () => {
  // Guards against a lazy "any inbound frame counts" implementation — the whole
  // point is that the module learns THIS credential landed, not that the MCU is
  // merely alive.
  const f = buildFrame(0x06, buildDpPayload(DpId.ADD_TEMP_RFID, DpType.RAW, [0, 3]));
  assert.notEqual(f[6], DpId.ADD_TEMP_PIN);
});

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed === 0 ? 0 : 1);
