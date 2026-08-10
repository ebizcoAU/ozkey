/**
 * ozkey-21 — MCU time service + fail-closed credential enforcement.
 *
 * HEADLESS. No browser, no `next dev`, no React. Everything under test lives in
 * lib/*.ts as pure TypeScript, which is deliberate: the rules being checked here
 * are security rules, and a security rule you can only exercise by clicking
 * around a web page does not get exercised.
 *
 *   npm test          (or: npx tsx test/mcuClock.test.ts)
 *
 * WHAT IT GUARDS
 *
 * 1. An MCU that was never told the time must refuse EVERY credential, valid
 *    ones included. "I don't know" is not "not expired".
 * 2. Once a module serves time, windows enforce normally.
 * 3. Time never moves backwards, so an expired credential can never be
 *    resurrected by replaying or spoofing an older time (CONTRACT.md:496).
 *
 * Confirmed against real hardware 2026-08-10: DoorA receives 0x1C and never
 * answers (`mcu=up tx=0 rx=10`), so on the real lock condition (1) is the live
 * state and every temp PIN is effectively permanent. See docs/ozkey-21.md §2.3.
 */
import {
  applyTimeReply,
  describeMcuClock,
  initialMcuClock,
  mcuClockState,
  readMcuUnix,
} from "@/lib/mcuClock";
import { checkWindowMcu, type StoredCredential } from "@/lib/credentials";
import {
  TuyaCommand,
  buildFrame,
  buildTimeReply,
  parseFrame,
  parseTimeReply,
  toHexString,
} from "@/lib/tuya";

const NOW = 1_754_800_000;
const cred = (slot: number, start: number, end: number): StoredCredential => ({
  kind: "PIN",
  slot,
  value: `${100000 + slot}`,
  start,
  end,
  token: "SRT-TEST-0000",
});
const expired = cred(1, NOW - 7200, NOW - 3600);
const live = cred(2, NOW - 60, NOW + 3600);

let pass = 0;
let fail = 0;
function ok(name: string, cond: boolean, extra = "") {
  if (cond) pass++;
  else fail++;
  console.log(`${cond ? "PASS" : "FAIL"}  ${name}${extra ? `  — ${extra}` : ""}`);
}

// 1 — the §2.3 condition: MCU never served time -------------------------------
let clock = initialMcuClock();
ok("MCU starts UNSYNCED", mcuClockState(clock) === "UNSYNCED");
ok("expired PIN -> TIME_UNKNOWN (fails closed)", checkWindowMcu(expired, readMcuUnix(clock)) === "TIME_UNKNOWN");
ok("VALID PIN also refused with no clock", checkWindowMcu(live, readMcuUnix(clock)) === "TIME_UNKNOWN");

// 2 — module answers "I was asked, and I don't know" ---------------------------
let r = applyTimeReply(clock, null);
ok("null reply not accepted", !r.accepted, r.reason);
ok("still UNSYNCED after null reply", mcuClockState(r.clock) === "UNSYNCED");

// 3 — round-trip through the REAL wire codec ----------------------------------
const frame = buildFrame(TuyaCommand.GET_LOCAL_TIME, buildTimeReply(NOW, true));
const parsed = parseFrame(frame);
ok("0x1C reply frame parses (checksum valid)", parsed.ok, toHexString(frame));
const decoded = parsed.ok ? parseTimeReply(parsed.frame.payload) : null;
ok("unix round-trips exactly", decoded?.unix === NOW, `got ${decoded?.unix}`);
ok("weekday present on local-time reply", decoded?.weekday != null);

// 4 — served time makes enforcement real --------------------------------------
r = applyTimeReply(clock, decoded!.unix!);
ok("first sync accepted", r.accepted, r.reason);
clock = r.clock;
ok("MCU now SYNCED", mcuClockState(clock) === "SYNCED", describeMcuClock(clock));
ok("expired PIN -> EXPIRED", checkWindowMcu(expired, readMcuUnix(clock)) === "EXPIRED");
ok("live PIN -> VALID", checkWindowMcu(live, readMcuUnix(clock)) === "VALID");

// 5 — monotonic-forward only (CONTRACT.md:496) --------------------------------
const back = applyTimeReply(clock, NOW - 86400);
ok("backwards time REFUSED", !back.accepted, back.reason);
ok(
  "expired PIN stays EXPIRED after rollback attempt",
  checkWindowMcu(expired, readMcuUnix(back.clock)) === "EXPIRED"
);
ok("forward time accepted", applyTimeReply(clock, NOW + 60).accepted);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
