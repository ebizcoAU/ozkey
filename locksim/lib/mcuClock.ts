/**
 * ozkey-21 — the lock MCU's clock, modelled honestly.
 *
 * WHY THIS EXISTS
 *
 * LockSim used to check credential windows against `useVirtualClock`, i.e. the
 * browser's own clock. That made every temporal test pass, and it is precisely
 * why LockSim could never have caught the bug in ozkey-21 §2.3.
 *
 * A real Tuya lock MCU does NOT own an authoritative clock. Tuya's own docs:
 *
 *   "the module comes with a software real-time clock (RTC) that provides time
 *    for the MCU, and therefore, even when the module is offline, the MCU can
 *    also get the time."
 *
 * The MODULE is the time source. The MCU asks it, over 0x0C / 0x1C, and holds
 * the answer. We replaced the Tuya module with our own ESP32 and never
 * implemented that service — so on real hardware the MCU may never have been
 * told the time at all, and its `from`/`to` windows may never have been
 * enforced.
 *
 * So this model starts UNSYNCED and stays that way until a module actually
 * answers. `useVirtualClock` keeps its job — it is the *module's* clock, and
 * the warp control is how a tester moves time — but the MCU may only ever see
 * time that arrived over the wire.
 */

export type McuClockState = "UNSYNCED" | "SYNCED" | "STALE";

/** How long an MCU trusts a sync before calling it stale. Tuya re-syncs ~6 h. */
export const MCU_SYNC_STALE_AFTER_MS = 6 * 60 * 60 * 1000;

export interface McuClock {
  /** Unix seconds as of `syncedAtMonotonicMs`, or null if never told. */
  baseUnix: number | null;
  /** performance.now()-style reading when `baseUnix` was accepted. */
  syncedAtMonotonicMs: number | null;
  /** Number of replies accepted — visible proof the service is being served. */
  syncCount: number;
  /** Requests sent with no reply. The symptom of the real-hardware bug. */
  unansweredRequests: number;
}

export function initialMcuClock(): McuClock {
  return { baseUnix: null, syncedAtMonotonicMs: null, syncCount: 0, unansweredRequests: 0 };
}

function monotonicNow(): number {
  return typeof performance !== "undefined" ? performance.now() : Date.now();
}

/**
 * Accept a time reply. Returns the new clock, and whether it was accepted.
 *
 * MONOTONIC-FORWARD ONLY, per CONTRACT.md:496 — "Firmware refuses any time
 * [going backward so nothing can] resurrect an expired token." A module that
 * offers an earlier time than we already hold is refused, because accepting it
 * would un-expire credentials. This is a security rule, not tidiness.
 */
export function applyTimeReply(
  clock: McuClock,
  unix: number | null
): { clock: McuClock; accepted: boolean; reason: string } {
  if (unix === null) {
    return {
      clock: { ...clock, unansweredRequests: clock.unansweredRequests + 1 },
      accepted: false,
      reason: "module reports no valid time — MCU stays UNSYNCED",
    };
  }
  const current = readMcuUnix(clock);
  if (current !== null && unix < current) {
    return {
      clock,
      accepted: false,
      reason: `REFUSED — ${unix} is earlier than current ${current}; clock is monotonic-forward only (CONTRACT.md:496)`,
    };
  }
  return {
    clock: {
      baseUnix: unix,
      syncedAtMonotonicMs: monotonicNow(),
      syncCount: clock.syncCount + 1,
      unansweredRequests: clock.unansweredRequests,
    },
    accepted: true,
    reason: current === null ? "first sync — MCU clock now usable" : "clock advanced",
  };
}

/** Record that we asked and nothing came back. */
export function noteUnanswered(clock: McuClock): McuClock {
  return { ...clock, unansweredRequests: clock.unansweredRequests + 1 };
}

/**
 * The MCU's current idea of unix time, or null if it has never been told.
 *
 * Between syncs it free-runs on its own oscillator — modelled here with the
 * monotonic clock. On the real board this is where the operator's external
 * 32.768 kHz crystal earns its place: ~±20 ppm (~1.7 s/day) instead of the
 * internal RC's percent-level drift, which is what makes a slow sync cadence
 * viable at all.
 */
export function readMcuUnix(clock: McuClock): number | null {
  if (clock.baseUnix === null || clock.syncedAtMonotonicMs === null) return null;
  const elapsedMs = monotonicNow() - clock.syncedAtMonotonicMs;
  return clock.baseUnix + Math.floor(elapsedMs / 1000);
}

export function mcuClockState(clock: McuClock): McuClockState {
  if (clock.baseUnix === null || clock.syncedAtMonotonicMs === null) return "UNSYNCED";
  return monotonicNow() - clock.syncedAtMonotonicMs > MCU_SYNC_STALE_AFTER_MS ? "STALE" : "SYNCED";
}

/** One-line status for the diagnostic console. */
export function describeMcuClock(clock: McuClock): string {
  const state = mcuClockState(clock);
  if (state === "UNSYNCED") {
    return (
      `MCU CLOCK: UNSYNCED — never told the time by the module` +
      (clock.unansweredRequests > 0
        ? ` (${clock.unansweredRequests} request(s) unanswered). ` +
          `Temporal credentials CANNOT be enforced.`
        : ". Temporal credentials CANNOT be enforced.")
    );
  }
  const unix = readMcuUnix(clock)!;
  return (
    `MCU CLOCK: ${state} — ${new Date(unix * 1000).toISOString().slice(0, 19)}Z ` +
    `(${clock.syncCount} sync(s))`
  );
}
