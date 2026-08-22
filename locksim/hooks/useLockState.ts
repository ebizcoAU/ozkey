"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { ResolvedProfile } from "@/lib/profile";
import {
  AccessResult,
  DpId,
  DpType,
  TuyaCommand,
  OZSIM_PID,
  OZSIM_MCU_FW,
  buildDpPayload,
  buildFrame,
  buildTimeReply,
  buildMcuFactoryReset,
  fromHexString,
  parseSlotPayload,
  parseTempCredential,
  parseTimePush,
  parseTimeReply,
  toHexString,
  u32be,
  type Byte,
  type ByteArray,
  type TuyaFrame,
} from "@/lib/tuya";
import {
  applyTimeReply,
  describeMcuClock,
  initialMcuClock,
  mcuClockState,
  noteUnanswered,
  readMcuUnix,
  type McuClock,
} from "@/lib/mcuClock";
import {
  checkWindow,
  checkWindowMcu,
  deleteCredential,
  loadCredentials,
  makeToken,
  saveCredentials,
  upsertCredential,
  type CredentialKind,
  type StoredCredential,
} from "@/lib/credentials";
import { accessDenied, accessGranted, keyClick, motorWhirr } from "@/lib/audio";

export type PowerState = "SLEEPING" | "WAKING";
export type LockState = "LOCKED" | "UNLOCKED";

export const HEARTBEAT_SECONDS = 600; // fallback MQTT heartbeat loop (10 min)
const HEARTBEAT_MIN_SECONDS = 5; // floor for the configurable wake interval
export const MASTER_PIN = "123456";
export const MASTER_CARD_UID = "7B 3F 91 D2";
const WAKE_HOLD_MS = 1000; // return to sleep after 1s of inactivity
const HEARTBEAT_BURST_MS = 200;
/** ozkey-21 — how long the MCU waits for a 0x1C reply before calling it unanswered. */
const TIME_REPLY_TIMEOUT_MS = 2000;
// Scramble / anti-peeping entry (ozkey-08 §0.3): the real PIN may be embedded
// anywhere in a longer digit string — junk digits defeat shoulder-surfing AND
// mask the wake-sync latency. Lockout (real MCUs) counts per ENTRY, never per
// candidate substring.
const SCRAMBLE_MAX_DIGITS = 20;
const UNLOCK_HOLD_MS = 5000; // remote/credential unlock auto-relock delay
const MOTOR_TRAVEL_MS = 900;
const ALARM_FLASH_MS = 1600;
/**
 * Default lag between receiving a DP command and answering with the DP 8 status
 * report. 120 ms is "parsed the frame and started acting" — a GUESS, and one
 * nobody has yet checked against Li's hardware. The single trace that settles it
 * (send one DP 1, record what comes back and when) replaces this number.
 */
const MCU_ACK_DELAY_MS = 120;

interface UseLockStateOptions {
  /** Fire an outbound Tuya frame onto the TX line (from useTuyaProtocol). */
  transmit: (command: Byte, payload: ByteArray, ...notes: string[]) => ByteArray;
  /** Virtual Master Clock getter — all temporal checks go through this. */
  virtualNow: () => number;
  /** Fired when the deep-sleep heartbeat timer elapses (Mode A: publish MQTT). */
  onHeartbeat?: () => void;
  /** Timer-wake interval in seconds (System Settings; falls back to 10 min). */
  heartbeatSeconds?: number;
  /** Fired on every door access transaction (Mode A: push usage log to MQTT). */
  onAccess?: (evt: AccessEvent) => void;
  /**
   * Milliseconds the MCU takes to answer a module-issued DP command with its
   * DP 8 status report. Defaults to `MCU_ACK_DELAY_MS`. Raise it past the
   * module's deadline to simulate a slow or still-waking MCU.
   */
  mcuAckDelayMs?: number;
  /**
   * ozkey-21 — append a line to the RX console. Used for time-service
   * diagnostics, which have no frame of their own when the answer never comes:
   * "the module did not reply" is exactly the observation we need visible, and
   * a missing frame cannot log itself.
   */
  pushRxLog?: (text: string, notes: string[], error?: boolean) => void;
  /**
   * The Tuya PID this emulated MCU answers command `0x01` with — normally the
   * `supplier.pid` of the profile selected in the UI.
   *
   * Until 2026-08-18 LockSim answered `OZSIM_PID` unconditionally, so
   * selecting `tuya-ds013-t3` changed how the console INTERPRETED bytes while
   * the MCU carried on claiming to be a fictional product. The module would
   * then discover a PID that disagreed with the profile the bench was reading
   * under — the two ends of the wire holding different ideas of what the
   * device is, which is the exact failure `profiles/` exists to prevent.
   *
   * Undefined falls back to `OZSIM_PID`, which keeps the default
   * (`ozkie-legacy-v0`, no PID) behaving exactly as it did.
   */
  tuyaPid?: string;
  /**
   * XF-118 P1c — the resolved product profile this bench is running as.
   *
   * LockSim emits only the DPs this product actually selects. Omit it and every
   * emission goes out unconditionally, which is the pre-2026-08-20 behaviour
   * and what the unit tests rely on.
   */
  profile?: ResolvedProfile;
  /**
   * ozkey-21 — what the EMULATED MODULE knows, in ms, or null if no module is
   * emulating the time service.
   *
   * Mode A (pure software) passes the Virtual Master Clock: LockSim is playing
   * both the MCU and the missing Wi-Fi chip, and a real Tuya module would
   * answer. Mode B passes null, because the thing on the other end of the wire
   * is our ESP32 — and whether IT answers is the entire question (ozkey-21
   * §2.3). Silence in Mode B is a result, not a bug in the simulator.
   */
  moduleTimeSource?: () => number | null;
  /** Inject a module -> MCU frame through the real parser (Mode A loopback). */
  receiveFromModule?: (bytes: ByteArray) => void;
  /**
   * ozkey-21 — true when the physical wire is open and writable.
   *
   * Web Serial needs a user gesture on EVERY page load, so in Mode B the port
   * is still disconnected when the boot time request fires and the request
   * goes nowhere, every single time. A real Tuya MCU asks for time when the
   * module reports itself ready, not on a fixed timer after its own power-up —
   * so we do the same, and re-ask on the rising edge of this flag.
   */
  linkReady?: boolean;
}

export interface AccessEvent {
  result: "granted" | "denied" | "expired";
  detail: string;
}

type Timer = ReturnType<typeof setTimeout>;

/**
 * The lock motherboard state machine: deep-sleep power management, GPIO wake
 * interrupts, credential validation, clutch motor cycles and heartbeat loop.
 */
export function useLockState({
  transmit,
  virtualNow,
  onHeartbeat,
  heartbeatSeconds,
  onAccess,
  mcuAckDelayMs,
  pushRxLog,
  tuyaPid,
  profile,
  moduleTimeSource,
  receiveFromModule,
  linkReady,
}: UseLockStateOptions) {
  const hbSeconds = Math.max(HEARTBEAT_MIN_SECONDS, Math.round(heartbeatSeconds || HEARTBEAT_SECONDS));
  const [powerState, setPowerState] = useState<PowerState>("SLEEPING");
  const [lockState, setLockState] = useState<LockState>("LOCKED");
  const [pinBuffer, setPinBuffer] = useState("");
  const [countdown, setCountdown] = useState(hbSeconds);
  const [lowBattery, setLowBattery] = useState(false);
  const [mechanicalKey, setMechanicalKeyState] = useState(false);
  const [motorActive, setMotorActive] = useState(false);
  const [alarm, setAlarm] = useState(false);
  const [lastEvent, setLastEvent] = useState("COLD BOOT — ENTERED DEEP SLEEP");
  const [credentials, setCredentials] = useState<StoredCredential[]>([]);
  /**
   * ozkey-21 — the MCU's OWN clock, which starts UNSYNCED and only advances
   * when the module answers 0x0C/0x1C. Deliberately NOT `virtualNow`: that is
   * the browser's clock and modelling the MCU with it is what let this
   * simulator pass tests that real hardware would fail.
   */
  const [mcuClock, setMcuClock] = useState<McuClock>(initialMcuClock);
  const mcuClockRef = useRef<McuClock>(mcuClock);

  /** Source of truth for keypad entry; `pinBuffer` state mirrors it for the display. */
  const pinBufferRef = useRef("");
  const sleepTimer = useRef<Timer | null>(null);
  const relockTimer = useRef<Timer | null>(null);
  const motorTimer = useRef<Timer | null>(null);
  // A LIST, not a single timer. Each command earns its own answer: two DP 1
  // frames arriving inside the ack delay must produce two reports, the way a
  // real MCU would. A shared timer would have let the second command silently
  // cancel the first one's report — building the exact "a command got no reply"
  // defect this change exists to expose.
  const ackTimers = useRef<Timer[]>([]);
  const alarmTimer = useRef<Timer | null>(null);
  const fingerprintPass = useRef(true); // alternates for deterministic bench testing
  const mechanicalRef = useRef(false);
  const credentialsRef = useRef<StoredCredential[]>([]);
  const transmitRef = useRef(transmit);
  transmitRef.current = transmit;
  const onHeartbeatRef = useRef(onHeartbeat);
  onHeartbeatRef.current = onHeartbeat;
  const pushRxLogRef = useRef(pushRxLog);
  pushRxLogRef.current = pushRxLog;
  // XF-118 P1c — the active product, so emitDp() can refuse to send a DP this
  // product does not carry. A ref, like every other option here, so the
  // emission callbacks do not need to be rebuilt when the operator switches
  // profile mid-session.
  const profileRef = useRef(profile);
  profileRef.current = profile;
  const moduleTimeSourceRef = useRef(moduleTimeSource);
  moduleTimeSourceRef.current = moduleTimeSource;
  const receiveFromModuleRef = useRef(receiveFromModule);
  receiveFromModuleRef.current = receiveFromModule;
  const onAccessRef = useRef(onAccess);
  onAccessRef.current = onAccess;
  // unlockCycle() is declared ABOVE emitDp() and needs to report the bolt, so
  // it reaches it through a ref rather than being reordered — same pattern as
  // every other forward reference in this hook. Assigned just below emitDp().
  const emitDpRef = useRef<
    ((dpId: DpId, type: DpType, value: number[], note: string) => void) | null
  >(null);
  const nowRef = useRef(virtualNow);
  nowRef.current = virtualNow;
  // Read through a ref, not a closure: the delay is edited live in Settings, and
  // a pending ack must fire with the value in force when the command landed.
  const ackDelayRef = useRef(MCU_ACK_DELAY_MS);
  ackDelayRef.current =
    typeof mcuAckDelayMs === "number" && Number.isFinite(mcuAckDelayMs)
      ? mcuAckDelayMs
      : MCU_ACK_DELAY_MS;

  // Load the EEPROM (LocalStorage) slot table once on mount.
  useEffect(() => {
    const loaded = loadCredentials();
    credentialsRef.current = loaded;
    setCredentials(loaded);
  }, []);

  const persistCredentials = useCallback((next: StoredCredential[]) => {
    credentialsRef.current = next;
    setCredentials(next);
    saveCredentials(next);
  }, []);

  /** Simulated GPIO wake interrupt: WAKE_INT line high, MCU at 45mA. */
  const wake = useCallback((reason: string, holdMs: number = WAKE_HOLD_MS) => {
    setPowerState("WAKING");
    setLastEvent(reason);
    if (sleepTimer.current) clearTimeout(sleepTimer.current);
    sleepTimer.current = setTimeout(() => {
      setPowerState("SLEEPING");
      setLastEvent("INACTIVITY TIMEOUT — BACK TO DEEP SLEEP");
    }, holdMs);
  }, []);

  const fireMotor = useCallback(() => {
    motorWhirr();
    setMotorActive(true);
    if (motorTimer.current) clearTimeout(motorTimer.current);
    motorTimer.current = setTimeout(() => setMotorActive(false), MOTOR_TRAVEL_MS);
  }, []);

  const flashAlarm = useCallback(() => {
    setAlarm(true);
    if (alarmTimer.current) clearTimeout(alarmTimer.current);
    alarmTimer.current = setTimeout(() => setAlarm(false), ALARM_FLASH_MS);
  }, []);

  /** Full unlock cycle: clutch motor, 5s open window, auto-relock. */
  const unlockCycle = useCallback(
    (source: string) => {
      accessGranted();
      onAccessRef.current?.({ result: "granted", detail: source });
      wake(`ACCESS GRANTED — ${source}`, UNLOCK_HOLD_MS + 1500);
      setLockState("UNLOCKED");
      fireMotor();
      // 🔴 REPORT THE BOLT (2026-08-22). Without this the module has NO evidence
      // the door ever moved: DP 8 ACCESS_RESULT is a fiction DP the real product
      // does not carry, so the profile gate drops it and the only other report
      // is the optional access event. A bolt that moves silently is exactly the
      // silent-failure class this bench exists to catch — and firmware does not
      // wait for an unlock ack, so nothing upstream would ever notice.
      // Field is `locked`: 0 = UNLOCKED.
      emitDpRef.current?.(DpId.BOLT_STATE, DpType.BOOL, [0], `DP 47 bolt_state — UNLOCKED (${source})`);
      if (relockTimer.current) clearTimeout(relockTimer.current);
      relockTimer.current = setTimeout(() => {
        if (mechanicalRef.current) return; // physical key holds the bolt open
        setLockState("LOCKED");
        fireMotor();
        // The relock is a real state change and must be reported too, or the
        // module's picture of the door stays UNLOCKED forever after one unlock.
        emitDpRef.current?.(DpId.BOLT_STATE, DpType.BOOL, [1], "DP 47 bolt_state — LOCKED (auto-relock)");
        setLastEvent("AUTO-RELOCK (5s TIMEOUT)");
      }, UNLOCK_HOLD_MS);
    },
    [wake, fireMotor]
  );

  /**
   * The DP 8 status report — the MCU's half of the Tuya UART contract.
   *
   * Single transmit site. It used to be written out twice (grant/deny), which
   * is how the remote-unlock path came to have no report at all: the third
   * caller simply never got written, and nothing pointed at its absence.
   */
  /**
   * XF-118 P1c — EMIT ONLY WHAT THE ACTIVE PROFILE SELECTS.
   *
   * Every emission below goes through here. If the selected product does not
   * carry the DP, LockSim says so on the console and sends NOTHING.
   *
   * WHY: a simulator that emits DPs the product does not have teaches everyone
   * downstream the wrong thing. Under `tuya-generic-lock` the fiction DPs
   * (1 unlock, 2 card, 3 fingerprint, 5 battery, 8 access-result, 21-24
   * credential CRUD) are simply not in the product — and 21/23/24 are worse
   * than absent, they are REAL DPs meaning navigation_volume, auto_lock and
   * auto_lock_delay. Emitting our meaning on those numbers is not a
   * simplification, it is a lie the console would render as truth.
   *
   * `lib/tuya.ts` already REJECTS inbound DPs outside the profile
   * ("NOT IN PROFILE … rejected, never forwarded"). Until now the send side had
   * no equivalent, so LockSim could emit frames its own decoder would refuse —
   * see XF-118 §1. This closes that asymmetry.
   *
   * No profile passed (tests, older callers) = emit unconditionally, which is
   * exactly the previous behaviour.
   */
  const emitDp = useCallback(
    (dpId: DpId, type: DpType, value: number[], note: string) => {
      const p = profileRef.current;
      if (p && !p.byDp.has(dpId)) {
        pushRxLogRef.current?.(
          `DP ${dpId} NOT EMITTED — not selected by profile '${p.profile_id}'`,
          [
            `LockSim only emits DPs the active product actually carries.`,
            `Select a profile that includes DP ${dpId}, or use the DP this product does have.`,
          ]
        );
        return;
      }
      transmitRef.current(TuyaCommand.DP_REPORT, buildDpPayload(dpId, type, value), note);
    },
    []
  );
  emitDpRef.current = emitDp;

  const reportAccessResult = useCallback(
    (result: AccessResult, note: string) => {
      emitDp(DpId.ACCESS_RESULT, DpType.ENUM, [result], note);
    },
    [emitDp]
  );

  const deny = useCallback(
    (reason: string, result: AccessResult) => {
      accessDenied();
      onAccessRef.current?.({
        result: result === AccessResult.EXPIRED ? "expired" : "denied",
        detail: reason,
      });
      flashAlarm();
      setLastEvent(`ACCESS ${result === AccessResult.EXPIRED ? "EXPIRED" : "DENIED"} — ${reason}`);
      reportAccessResult(result, `Access result: ${AccessResult[result]} — ${reason}`);
    },
    [flashAlarm, reportAccessResult]
  );

  /**
   * The REAL T3 access-event report (L-1/L-3, operator directive 2026-08-17).
   *
   * DP 61/63/64/76 — `status: confirmed`, `type: value`, `verb: event.access`,
   * payload = cred_id. This is what a real DS013-T3 sends when a credential
   * opens the door, and it carries information our fiction never could: WHICH
   * stored credential it was. DP 2 carried a raw card UID, DP 3 a bare bool.
   *
   * Single transmit site, deliberately — the comment on reportAccessResult()
   * above records what happened last time this kind of report was written out
   * at each call site instead: one path simply never got it, in total silence.
   *
   * 🔴 This is a REPORT, not a credential write. See DpId's note: provisioning
   * is DP 13/14/15 and all three are supplier-blocked (ozkey-27 Q2).
   */
  const reportAccessEvent = useCallback(
    (dpId: DpId, credId: number, source: string) => {
      emitDp(
        dpId,
        DpType.VALUE,
        u32be(credId),
        `DP ${dpId} access event — cred_id=${credId} (${source}) [real T3 DP]`
      );
    },
    [emitDp]
  );

  /**
   * Local credential entry (keypad / card / fingerprint). Unchanged ordering.
   *
   * `event` is the REAL T3 access-event DP for this credential kind. It is
   * optional so that grant() paths with no real-DP equivalent (the master PIN,
   * which is not a stored credential and has no cred_id) simply omit it rather
   * than inventing a slot number.
   */
  const grant = useCallback(
    (source: string, event?: { dp: DpId; credId: number }) => {
      reportAccessResult(AccessResult.SUCCESS, `Access result: SUCCESS — ${source}`);
      if (event) reportAccessEvent(event.dp, event.credId, source);
      unlockCycle(source);
    },
    [unlockCycle, reportAccessResult, reportAccessEvent]
  );

  /**
   * Remote unlock arriving as DP 1 over the UART — the path that had NO status
   * report until 2026-08-04.
   *
   * Two things differ from `grant()` deliberately:
   *
   * 1. **Execute, then report.** The Tuya contract for a module-issued command
   *    is parse → execute → report, and the report is supposed to be evidence
   *    that the bolt actually moved. Reporting first (as the local path does,
   *    where nothing issued a command to answer) would make the report a
   *    restatement of intent, which is the exact thing the module is currently
   *    being criticised for doing one layer up.
   * 2. **It can be LATE.** See `mcuAckDelayMs` — a simulator that always answers
   *    in zero milliseconds cannot tell an ESP32 with timeout/retry logic apart
   *    from one with none, and right now the firmware has none.
   */
  const remoteUnlock = useCallback(
    /**
     * `event` is the REAL T3 access-event DP for HOW this unlock arrived, and
     * the distinction is a genuine one in the catalogue:
     *
     *   DP 72 `unlock_remote`  access_kind: remote  — over the network
     *   DP 76 `unlock_ble`     access_kind: ble     — over Bluetooth, offline
     *
     * A real lock reports which of the two happened; our fiction (DP 1) could
     * not express the difference at all. Optional so a caller with no real
     * equivalent omits it rather than inventing one.
     */
    (source: string, event?: { dp: DpId; credId: number }) => {
      unlockCycle(source);
      const delay = Math.max(0, Math.round(ackDelayRef.current));
      const t = setTimeout(() => {
        ackTimers.current = ackTimers.current.filter((x) => x !== t);
        reportAccessResult(
          AccessResult.SUCCESS,
          `Access result: SUCCESS — ${source} (+${delay}ms)`
        );
        if (event) reportAccessEvent(event.dp, event.credId, source);
      }, delay);
      ackTimers.current.push(t);
    },
    [unlockCycle, reportAccessResult, reportAccessEvent]
  );

  // ---------------------------------------------------------------------
  // Timer-wake heartbeat loop (interval set in System Settings)
  // ---------------------------------------------------------------------
  useEffect(() => {
    const iv = setInterval(() => setCountdown((c) => (c > 0 ? c - 1 : c)), 1000);
    return () => clearInterval(iv);
  }, []);

  // Re-arm the countdown when the configured interval changes.
  useEffect(() => {
    setCountdown(hbSeconds);
  }, [hbSeconds]);

  /**
   * ozkey-21 — the ONE place a time request is issued. Boot, heartbeat and
   * serial-connect all call this, so the three cannot drift apart; the
   * unanswered-timeout accounting in particular was easy to get subtly
   * different per copy.
   */
  const requestMcuTime = useCallback((why: string) => {
    transmitRef.current(
      TuyaCommand.GET_LOCAL_TIME,
      [],
      `Time request (0x1C) -> module — ${why}, MCU has no clock of its own`
    );

    // Mode A: the emulated module answers, as real bytes through the real
    // parser, so the wire format is exercised even without hardware.
    const moduleMs = moduleTimeSourceRef.current?.();
    if (moduleMs != null && receiveFromModuleRef.current) {
      const reply = buildFrame(
        TuyaCommand.GET_LOCAL_TIME,
        buildTimeReply(Math.floor(moduleMs / 1000), true)
      );
      window.setTimeout(() => receiveFromModuleRef.current?.(reply), 60);
      return;
    }

    // Mode B: nothing here answers. Whether the ESP32 does is the experiment.
    const before = mcuClockRef.current.syncCount;
    window.setTimeout(() => {
      if (mcuClockRef.current.syncCount !== before) return; // someone answered
      const next = noteUnanswered(mcuClockRef.current);
      mcuClockRef.current = next;
      setMcuClock(next);
      pushRxLogRef.current?.(
        "(no reply)",
        [
          `TIME REQUEST UNANSWERED (${why}) — module did not serve time ` +
            `[${next.unansweredRequests} total]`,
          describeMcuClock(next),
        ],
        true
      );
    }, TIME_REPLY_TIMEOUT_MS);
  }, []);

  /**
   * Ask at boot. In Mode A this is the only trigger that matters. In Mode B it
   * will usually MISS, because Web Serial needs a user gesture on every page
   * load and the port is not open yet — which is why the link-ready effect
   * below exists rather than this being the only attempt.
   */
  useEffect(() => {
    const t = window.setTimeout(() => requestMcuTime("BOOT sync"), 800);
    return () => window.clearTimeout(t);
  }, [requestMcuTime]);

  /**
   * Ask on the RISING EDGE of the wire becoming ready.
   *
   * This is the one that actually fires in Mode B. Web Serial requires the
   * operator to pick the port by hand after every refresh, so "connected" can
   * happen many seconds after mount — the boot request above has already been
   * spent by then. A real MCU asks when the module says it is ready; so do we.
   */
  const linkWasReady = useRef(false);
  useEffect(() => {
    const ready = Boolean(linkReady);
    if (ready && !linkWasReady.current) {
      // Small settle before the first byte, same reasoning as boot.
      const t = window.setTimeout(() => requestMcuTime("LINK CONNECTED"), 400);
      linkWasReady.current = true;
      return () => window.clearTimeout(t);
    }
    if (!ready) linkWasReady.current = false; // re-arm for the next connect
  }, [linkReady, requestMcuTime]);

  useEffect(() => {
    if (countdown !== 0) return;
    wake("MQTT HEARTBEAT BURST (TIMER WAKE)", HEARTBEAT_BURST_MS);
    transmitRef.current(
      TuyaCommand.HEARTBEAT,
      [],
      `MQTT heartbeat ping -> Tuya broker (${hbSeconds}s timer wake)`
    );

    // ozkey-21 — ride the heartbeat to re-ask, which is what a real Tuya MCU
    // does ("synchronizes time with the server during each data interaction or
    // every six hours"). Free: the MCU is already awake and transmitting.
    requestMcuTime("heartbeat");

    onHeartbeatRef.current?.();
    setCountdown(hbSeconds);
  }, [countdown, wake, hbSeconds, requestMcuTime]);

  // ---------------------------------------------------------------------
  // Credential validation against the Virtual Master Clock
  // ---------------------------------------------------------------------
  const submitPin = useCallback(
    (pin: string) => {
      // 🔴 CHANGED 2026-08-11 — LockSim no longer sends the entered digits.
      //
      // It used to report DP 1 with the PIN as a u32 VALUE. That was our
      // invention, not verified Tuya behaviour, and it modelled something a
      // lock should not do: handing a credential to the wireless module over
      // an unencrypted UART. The firmware does not recognise DP 1 inbound, so
      // the frame fell through its unknown-DP path and the PIN was published
      // to the SERVER as a door-log line (fixed the same day in
      // ozdoorlock_core.h — see "UNCLASSIFIED DP, NOT published").
      //
      // A real lock reports the OUTCOME, not the keystrokes: DP 8
      // ACCESS_RESULT, which grant()/deny() already send below. The module has
      // no business knowing which digits were pressed — it cannot evaluate
      // them anyway; the credential store is the DL MCU's.
      //
      // We still report that an attempt HAPPENED, with its length only, so the
      // console shows keypad activity without carrying the secret.
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.UNLOCK_CHANNEL, DpType.BOOL, [0x01]),
        `Keypad entry attempt: ${pin.length} digit(s) — value NOT sent ` +
          `(a lock reports the result, not the credential)`
      );
      // Scramble matching: any contiguous substring of the entry may be the
      // real PIN. A valid-window credential wins over an expired match.
      if (pin.includes(MASTER_PIN)) {
        grant("KEYPAD PIN (MASTER)");
        return;
      }
      const candidates = credentialsRef.current.filter(
        (c) => c.kind === "PIN" && pin.includes(c.value)
      );
      if (!candidates.length) {
        deny("NO STORED PIN IN ENTRY", AccessResult.DENIED);
        return;
      }
      // ozkey-21 — the MCU's own clock, NOT the browser's. If the module never
      // served us the time, we cannot evaluate any window and must refuse.
      const mcuNow = readMcuUnix(mcuClockRef.current);
      const valid = candidates.find((c) => checkWindowMcu(c, mcuNow) === "VALID");
      if (valid) {
        grant(`TEMP PIN — SLOT ${valid.slot}`, {
          dp: DpId.UNLOCK_PASSWORD,
          credId: valid.slot,
        });
        return;
      }
      const first = candidates[0];
      const verdict = checkWindowMcu(first, mcuNow);
      if (verdict === "TIME_UNKNOWN") {
        // FAIL CLOSED. A lock that cannot tell whether the window has closed
        // must not open. Treating "I don't know" as "not expired" is exactly
        // the real-hardware failure ozkey-21 §2.3 suspects.
        deny(
          `TEMP PIN SLOT ${first.slot} — TIME_UNKNOWN, MCU never served time by module (refusing)`,
          AccessResult.EXPIRED
        );
        return;
      }
      deny(
        `TEMP PIN SLOT ${first.slot} ${verdict === "EXPIRED" ? "EXPIRED" : "NOT YET ACTIVE"}`,
        AccessResult.EXPIRED
      );
    },
    [grant, deny]
  );

  // ---------------------------------------------------------------------
  // Physical inputs (every one is a GPIO wake interrupt)
  // ---------------------------------------------------------------------
  const pressKey = useCallback(
    (key: string) => {
      keyClick();
      wake(`KEYPAD INTERRUPT — KEY '${key}'`);
      // '*' clears the entry. It was briefly a COMMAND PREFIX (*NN#), so the
      // bench could exercise a keypad gesture the firmware understood — that
      // whole path was removed 2026-08-16 once real product catalogues showed
      // it could never work on a shipping lock: no Tuya DP carries keystrokes,
      // '#'-as-submit varies by model, and on some locks '#' IS the doorbell
      // button. Modelling a keypad our own firmware invented is exactly how a
      // simulator passes tests hardware would fail.
      if (key === "*") {
        pinBufferRef.current = "";
        setPinBuffer("");
        return;
      }
      if (key === "#") {
        // Submit OUTSIDE the state updater — StrictMode runs updaters twice,
        // which double-fired the TX frame + usage-log publish per attempt.
        const buffer = pinBufferRef.current;
        pinBufferRef.current = "";
        setPinBuffer("");
        // 4-digit PINs arrived with the hotel/BANOI flow (server generatePin);
        // scramble entry (§0.3) allows junk digits around the real PIN, so
        // the ceiling is the anti-peep cap, not the PIN length.
        if (buffer.length >= 4 && buffer.length <= SCRAMBLE_MAX_DIGITS) {
          submitPin(buffer);
        } else {
          setLastEvent(`PIN REJECTED — NEED 4-${SCRAMBLE_MAX_DIGITS} DIGITS (GOT ${buffer.length})`);
        }
        return;
      }
      const next =
        pinBufferRef.current.length < SCRAMBLE_MAX_DIGITS ? pinBufferRef.current + key : pinBufferRef.current;
      pinBufferRef.current = next;
      setPinBuffer(next);
    },
    [wake, submitPin]
  );

  /** Tap the master Mifare card, or a stored temporary card when given. */
  const tapRfid = useCallback(
    (cred?: StoredCredential) => {
      const uid = cred?.value ?? MASTER_CARD_UID;
      wake(`RFID FIELD INTERRUPT — MIFARE UID ${uid}`);
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.RFID_CARD, DpType.RAW, fromHexString(uid) ?? []),
        `Mifare card tap: UID ${uid}${cred ? ` (temp slot ${cred.slot})` : " (master card)"}`
      );
      if (!cred) {
        grant("RFID MASTER CARD");
        return;
      }
      // ozkey-21 — MCU clock, same fail-closed rule as the PIN path.
      const window = checkWindowMcu(cred, readMcuUnix(mcuClockRef.current));
      if (window === "VALID") {
        grant(`TEMP RFID — SLOT ${cred.slot}`, {
          dp: DpId.UNLOCK_CARD,
          credId: cred.slot,
        });
      } else if (window === "TIME_UNKNOWN") {
        deny(
          `TEMP RFID SLOT ${cred.slot} — TIME_UNKNOWN, MCU never served time by module (refusing)`,
          AccessResult.EXPIRED
        );
      } else {
        deny(
          `TEMP RFID SLOT ${cred.slot} ${window === "EXPIRED" ? "EXPIRED" : "NOT YET ACTIVE"}`,
          AccessResult.EXPIRED
        );
      }
    },
    [wake, grant, deny]
  );

  /**
   * ozkey-22 R1 — simulate the PHYSICAL factory-reset gesture on the lock body.
   *
   * That button is wired to the MCU, not to our ESP32, so on real hardware the
   * MCU is the one that notices and it must tell the module. Tuya's door-lock
   * protocol gives 0x34 sub-command 0x0A for exactly this.
   *
   * The firmware does NOT handle this yet (ozkey-22 R1 is unbuilt) — pressing
   * this today should produce a frame on the wire and NO reaction from the
   * lock, which is the point: it makes the gap visible and gives R1 something
   * to be tested against the moment it lands.
   */
  /**
   * PROPOSED DP 60 — the keypad pairing gesture, as the DL MCU would report it.
   *
   * On production the keypad is the DL MCU's, so this is the only way a member
   * at the door can ask the lock to advertise. Emitting it here lets the
   * firmware side be written and tested before the manufacturer allocates a
   * real DP — but nothing on real hardware sends this today.
   */
  /**
   * DP 53 doorbell — a real, confirmed Tuya DP, unlike the proposed DP 60
   * below. Firmware (doorlock-1.84+) opens the BLE pairing window on this,
   * with a 2-minute cooldown after the window closes so ringing repeatedly
   * cannot hold the radio on and flatten the battery.
   */
  const ringDoorbell = useCallback(() => {
    wake("DOORBELL PRESSED");
    emitDp(
      DpId.DOORBELL,
      DpType.BOOL,
      [0x01],
      "DP 53 doorbell -> OZKIE MCU — visitor at the door"
    );
    setLastEvent("DOORBELL RUNG (DP 53)");
  }, [wake, emitDp]);

  /**
   * ⚠️ NO LONGER TRANSMITS. XF-118 §5 / ozkey-39 §2.
   *
   * This used to emit DP 60 as a "pairing request", a number ozkey-22 §7 chose
   * as a PLACEHOLDER pending manufacturer allocation. The first real supplier
   * DP list answered that request and the answer is that **the number is
   * taken**: on Luona DS013-T3, DP 60 is the door-lock ALARM channel — an
   * 18-value enum carrying pry, wrong_password, wrong_finger, low_battery,
   * system_lock and more. It is one of the busiest reports on the bus.
   *
   * Firmware deleted its DP 60 handler on 2026-08-13 for exactly that reason
   * (ozdoorlock_core.h, "the number was already taken"). LockSim kept emitting
   * it, so until now the simulator was sending a fiction on a channel the
   * firmware had correctly stopped listening to — and on real hardware that
   * frame would read as an ALARM.
   *
   * A simulator that emits traffic no real lock emits is worse than one that
   * emits nothing: it lets consumers be written against a signal that will
   * never arrive. That is the whole XF-114 failure mode, pointed the other way.
   *
   * The gesture stays in the UI because the underlying PRODUCT question is
   * still open — on production hardware the keypad belongs to the DL MCU, so a
   * member at the door has no way to make the lock advertise (the dev boards'
   * own touch panel is not present in production). The doorbell (DP 53) is the
   * real, confirmed channel firmware opens the window on; use that. This button
   * now says so instead of putting a lie on the wire.
   */
  const keypadPairingGesture = useCallback(() => {
    wake("KEYPAD PAIRING GESTURE — not transmitted (DP 60 is the ALARM channel)");
    setLastEvent("NOT SENT — DP 60 is alarm on real locks; ring the doorbell (DP 53)");
  }, [wake]);

  const mcuFactoryReset = useCallback(() => {
    wake("USER FACTORY RESET GESTURE (lock body button -> DL MCU)");

    // ── ORDER IS THE POINT (operator, 2026-08-10) ─────────────────────────
    // The DL MCU owns the button AND owns the credential store, so wiping
    // that store is its OWN local responsibility — done here, first,
    // unconditionally, before anything is sent anywhere. It must NOT depend
    // on the wireless side acknowledging, being awake, or even existing.
    //
    // This is what ozkey-22 §2.1 row 1 says should happen on real hardware,
    // and it is the behaviour we are asking the lock manufacturer to confirm
    // (ozkey-22 §6 Q0). Modelling it here means that when the answer comes
    // back, LockSim already behaves the way a correct DL MCU would — and if
    // the real one does NOT, the difference is visible against this baseline.
    const wiped = credentialsRef.current.length;
    persistCredentials([]);

    // THEN tell the OZKIE MCU, so it can wipe its own half (bonds, keypair,
    // mesh). Notification only — it is not what cleared the credentials.
    transmitRef.current(
      TuyaCommand.TIME_NOTIFY,
      buildMcuFactoryReset(),
      `FACTORY RESET (0x34 0x0A) -> OZKIE MCU — DL MCU wiped ${wiped} credential(s) locally first`
    );
    setLastEvent(
      `DL MCU FACTORY RESET — ${wiped} credential(s) erased locally, module notified`
    );
  }, [wake, persistCredentials]);

  const scanFingerprint = useCallback(() => {
    const pass = fingerprintPass.current;
    fingerprintPass.current = !pass; // alternate pass/fail for the bench
    wake(`FINGERPRINT SENSOR INTERRUPT — ${pass ? "MATCH" : "NO MATCH"}`);
    transmitRef.current(
      TuyaCommand.DP_REPORT,
      buildDpPayload(DpId.FINGERPRINT, DpType.BOOL, [pass ? 0x01 : 0x00]),
      `Fingerprint verification: ${pass ? "SUCCESS" : "FAILED"}`
    );
    // cred_id 1: LockSim has no fingerprint ENROLMENT store (the sensor is a
    // simple alternating pass/fail), so there is no real slot to report. A
    // fixed id is honest for a simulator and keeps the DP 63 shape exercisable;
    // it is NOT a claim that finger #1 was matched.
    if (pass) grant("FINGERPRINT", { dp: DpId.UNLOCK_FINGERPRINT, credId: 1 });
    else deny("FINGERPRINT NO MATCH", AccessResult.DENIED);
  }, [wake, grant, deny]);

  const triggerLowBattery = useCallback(() => {
    setLowBattery((prev) => {
      const next = !prev;
      wake(next ? "LOW BATTERY EVENT — VBAT < 4.4V" : "BATTERY RESTORED — VBAT NOMINAL");
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.BATTERY_ALARM, DpType.BOOL, [next ? 0x01 : 0x00]),
        next ? "Battery alarm: LOW (cell < 20%)" : "Battery alarm: CLEARED"
      );
      return next;
    });
  }, [wake]);

  /** Physical mechanical key override — pure clutch mechanics, no radio auth. */
  const setMechanicalKey = useCallback(
    (engaged: boolean) => {
      mechanicalRef.current = engaged;
      setMechanicalKeyState(engaged);
      fireMotor();
      if (engaged) {
        setLockState("UNLOCKED");
        onAccessRef.current?.({ result: "granted", detail: "MECHANICAL KEY OVERRIDE" });
        wake("MECHANICAL KEY OVERRIDE — CLUTCH FORCED OPEN");
      } else {
        setLockState("LOCKED");
        wake("MECHANICAL KEY REMOVED — BOLT RE-THROWN");
      }
    },
    [wake, fireMotor]
  );

  /** Surface an external one-line status message on the event ticker. */
  const pushEvent = useCallback((message: string) => setLastEvent(message), []);

  /** Manually revoke a slot: compile + fire a DPID 22/24 delete frame, then wipe. */
  const revokeCredential = useCallback(
    (kind: CredentialKind, slot: number) => {
      const dpId = kind === "PIN" ? DpId.DELETE_PIN : DpId.DELETE_RFID;
      wake(`REGISTRY REVOKE — ${kind} SLOT ${slot}`);
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(dpId, DpType.RAW, [(slot >> 8) & 0xff, slot & 0xff]),
        `Revoke ${kind} slot ${slot} -> DPID ${dpId} (${kind === "PIN" ? "Delete PIN" : "Delete RFID"}) wipe`
      );
      persistCredentials(deleteCredential(credentialsRef.current, kind, slot));
      setLastEvent(`${kind} SLOT ${slot} REVOKED (DPID ${dpId})`);
    },
    [wake, persistCredentials]
  );

  // ---------------------------------------------------------------------
  // Incoming frame dispatch (valid frames only, from useTuyaProtocol)
  // ---------------------------------------------------------------------
  const handleFrame = useCallback(
    (frame: TuyaFrame) => {
      wake("UART RX INTERRUPT — FRAME DECODED");
      if (frame.command === TuyaCommand.HEARTBEAT) {
        transmitRef.current(TuyaCommand.HEARTBEAT, [0x01], "Heartbeat response (MCU alive)");
        return;
      }

      // 0x01 — "what are you?". Answering this is what lets the module pick
      // its DP profile from the product rather than a compiled-in default.
      //
      // The PID comes from the SELECTED profile so the MCU's self-description
      // and the console's interpretation cannot drift apart.
      //
      // 🔴 2026-08-20 — NO PID MEANS SAY NOTHING. This used to fall back to the
      // fictional OZSIM_PID, so a profile that legitimately has no
      // `supplier.pid` — `ozkie-legacy-v0` (our invented map, and the DEFAULT)
      // and `tuya-generic-lock` (the maker we have no PID for, by definition) —
      // announced itself as a DIFFERENT PRODUCT. Firmware believed it and
      // switched its whole DP map to `ozsim-fullfeature`.
      //
      // Observed on the bench: the operator issued a PIN, both ends reported
      // nothing wrong, and it silently vanished — because firmware was running
      // a profile that carries no credential DP at all, adopted from a PID the
      // simulator invented (ozkey-42 §2.4, XF-118).
      //
      // Firmware already handles silence correctly and deliberately: an
      // unanswered 0x01 leaves it on its current profile, with
      // "[PID] MCU never answered 0x01 — staying on the compiled-in profile."
      // That is the honest outcome for a device that has no product identity.
      // Impersonating one is strictly worse than admitting we have none.
      // ── 0x08 STATUS QUERY — report every DP this product has ─────────────
      //
      // Tuya: on 0x08 the MCU reports the status of all its datapoints, in one
      // frame or several. That is how the module learns a lock's CAPABILITIES,
      // as opposed to its identity (0x01).
      //
      // We answer from the SELECTED PROFILE, which is the whole value of doing
      // it here: a simulator standing in for a DS013-T3 must report exactly the
      // DPs a DS013-T3 has. Answering with everything we could encode would
      // make firmware's census agree with us and disagree with real hardware —
      // which is worse than not answering, because it would look verified.
      //
      // RESERVED DPs are reported too, deliberately. The payload layout being
      // unsupplied does not mean the DP is absent from the device; firmware
      // needs to know it EXISTS in order to say "known DP, unusable payload"
      // rather than "unknown verb" (ozkey-42).
      if (frame.command === TuyaCommand.QUERY_STATUS) {
        const p = profileRef.current;
        if (!p) {
          pushRxLogRef.current?.("0x08 status query — no profile selected", [
            "Nothing to report; select a DP profile first.",
          ]);
          return;
        }
        // A status report carries each DP's CURRENT VALUE. We have no real
        // hardware state for most of these, so we report the type's zero —
        // firmware's census reads the DP ID, not the value.
        const zeroFor = (t: string): { type: DpType; val: number[] } => {
          switch (t) {
            case "bool":   return { type: DpType.BOOL,   val: [0x00] };
            case "value":  return { type: DpType.VALUE,  val: [0, 0, 0, 0] };
            case "enum":   return { type: DpType.ENUM,   val: [0x00] };
            case "bitmap": return { type: DpType.BITMAP, val: [0x00] };
            case "string": return { type: DpType.STRING, val: [] };
            default:       return { type: DpType.RAW,    val: [] };
          }
        };
        let n = 0;
        for (const e of p.entries) {
          const { type, val } = zeroFor(e.type);
          transmitRef.current(
            TuyaCommand.DP_REPORT,
            buildDpPayload(e.dp, type, val),
            `0x08 census: DP ${e.dp} ${e.name} (${e.status})`
          );
          n++;
        }
        pushRxLogRef.current?.(
          `0x08 status query — reported ${n} DPs from '${p.profile_id}'`,
          [`This is what a ${p.profile_id} actually implements.`]
        );
        setLastEvent(`DP CENSUS SENT — ${n} DPs (${p.profile_id})`);
        return;
      }

      if (frame.command === TuyaCommand.PRODUCT_INFO) {
        const pid = tuyaPid;
        if (!pid) {
          pushRxLogRef.current?.(
            `0x01 product info — NOT ANSWERED (profile has no PID)`,
            [
              `'${profileRef.current?.profile_id ?? "this profile"}' has no supplier.pid, which is correct for it.`,
              `Staying silent so the module keeps its own profile rather than adopting a fiction.`,
            ]
          );
          setLastEvent("PRODUCT INFO — no PID to report (silent, by design)");
          return;
        }
        const fictional = pid === OZSIM_PID;
        const body = JSON.stringify({ p: pid, v: OZSIM_MCU_FW });
        transmitRef.current(
          TuyaCommand.PRODUCT_INFO,
          Array.from(body, (ch) => ch.charCodeAt(0)),
          `Product info: pid=${pid}${fictional ? " (FICTIONAL)" : ""}, mcu fw=${OZSIM_MCU_FW}`
        );
        setLastEvent(`PRODUCT INFO SENT — ${pid}`);
        return;
      }

      // ozkey-21 — the module answering our time question. THIS is the frame
      // whose absence on real hardware is the suspected bug: we ask, and if
      // nothing comes back the MCU stays UNSYNCED and cannot enforce any
      // credential window.
      if (
        frame.command === TuyaCommand.GET_GMT_TIME ||
        frame.command === TuyaCommand.GET_LOCAL_TIME
      ) {
        if (frame.payload.length === 0) return; // our own request echoed back
        const reply = parseTimeReply(frame.payload);
        const { clock, accepted, reason } = applyTimeReply(
          mcuClockRef.current,
          reply?.valid ? reply.unix : null
        );
        mcuClockRef.current = clock;
        setMcuClock(clock);
        pushRxLogRef.current?.(
          toHexString(frame.payload),
          [`MCU TIME ${accepted ? "ACCEPTED" : "NOT ACCEPTED"} — ${reason}`, describeMcuClock(clock)],
          !accepted
        );
        return;
      }
      if (frame.command === TuyaCommand.TIME_NOTIFY) {
        const push = parseTimePush(frame.payload);
        if (!push) return;
        const { clock, accepted, reason } = applyTimeReply(
          mcuClockRef.current,
          push.valid ? push.unix : null
        );
        mcuClockRef.current = clock;
        setMcuClock(clock);
        pushRxLogRef.current?.(
          toHexString(frame.payload),
          [`MCU TIME PUSH ${accepted ? "ACCEPTED" : "NOT ACCEPTED"} — ${reason}`, describeMcuClock(clock)],
          !accepted
        );
        return;
      }

      // INBOUND is module → MCU, i.e. DP_ISSUE (0x06). DP_REPORT (0x07) is our
      // OWN direction and must not be required here — gating this on DP_REPORT
      // after the 0x06/0x07 split would silently drop every command the module
      // sends us (remote unlock, credential writes).
      if (frame.command !== TuyaCommand.DP_ISSUE && frame.command !== TuyaCommand.DP_REPORT)
        return;

      for (const dp of frame.dataPoints) {
        switch (dp.dpId) {
          case DpId.UNLOCK_CHANNEL:
            // Was `unlockCycle(...)`, which opened the bolt and told the module
            // NOTHING — the module's whole picture of "did it work" was the fact
            // that it had written bytes to a serial port. Now it answers.
            // DP 1 is OUR FICTION (XF-110). The real catalogue equivalent of a
            // network-issued unlock is DP 72 `unlock_remote`, so report that
            // back — the module asked on an invented DP, the MCU answers on the
            // real one, which is exactly the migration this bench exists to
            // exercise. cred_id 0 = "no stored credential", correct here: a
            // remote unlock is authorised by the server/app, not by a slot.
            if (dp.type === DpType.BOOL && dp.value === 1)
              remoteUnlock("REMOTE UNLOCK COMMAND", { dp: DpId.UNLOCK_REMOTE, credId: 0 });
            break;
          /**
           * DP 76 `unlock_ble` — INBOUND, module → MCU. Corrected 2026-08-17.
           *
           * 🔴 THIS WAS BUILT BACKWARDS. LockSim had a button that EMITTED
           * DP 76, which is impossible on real hardware: **the DL MCU has no
           * BLE radio** (operator). BLE belongs to the wireless module — T3-U
           * is BLE 5.4, ours is the ESP32-C6 — so the MCU can never originate
           * a BLE unlock. The module completes the BLE ceremony and then tells
           * the MCU to open, carrying cred_id.
           *
           * This matters beyond tidiness: DP 76 is `status: confirmed` with a
           * fully specified 4-byte payload (range 0..99999) and is issuable
           * under 0x06 in the supplier's own table — unlike DP 10, whose
           * payload is "0x00-0xff" and therefore unimplementable. So this may
           * be the one REAL unlock command available to us today, and it is
           * exactly the offline-BLE path OZLOCK is built on. See ozkey-39 §3.5.
           */
          case DpId.UNLOCK_BLE:
            if (dp.type === DpType.VALUE)
              // 🔴 The `event` argument was MISSING here until 2026-08-22, while
              // the DP 1 branch above has always passed one. So a real, fully
              // specified DP 76 unlock reported no access event at all — the
              // one DP whose payload the supplier documents completely was the
              // one we stayed silent on. Echoing DP 76 back with the cred_id is
              // what a real T3 does: the module commands on 76, the MCU reports
              // the access on 76.
              remoteUnlock(`BLE UNLOCK (DP 76, cred_id=${dp.value})`, {
                dp: DpId.UNLOCK_BLE,
                credId: dp.value,
              });
            break;
          case DpId.ADD_TEMP_PIN:
          case DpId.ADD_TEMP_RFID: {
            const parsed = parseTempCredential(dp.dpId, dp.raw);
            const kind = dp.dpId === DpId.ADD_TEMP_PIN ? "PIN" : "RFID";
            if (!parsed) {
              // 🔴 This used to be a bare `break`. A credential the MCU could
              // not decode vanished with NO log, no event, nothing on screen —
              // and the module had already told the app UNLOCK_OK, because it
              // never waits for us. So a PIN could be "issued" successfully and
              // simply not exist, with no evidence anywhere in the system.
              //
              // That is exactly what happened: the module hex-decoded the PIN
              // digits (fixed in doorlock-1.60), sending 48 29 15 for "482915".
              // It cost a bench session to find something the MCU knew
              // instantly. A real MCU rejects loudly; so does this one now.
              setLastEvent(`TEMP ${kind} REJECTED — UNDECODABLE PAYLOAD`);
              pushEvent(
                `MCU: ${kind} grant DISCARDED — payload ${toHexString(dp.raw)} ` +
                  `is not [slot 2B][${kind === "PIN" ? "ASCII digits" : "UID bytes"}][from 4B][to 4B]`
              );
              break;
            }
            persistCredentials(
              upsertCredential(credentialsRef.current, {
                kind,
                slot: parsed.slot,
                value: parsed.credential,
                start: parsed.start,
                end: parsed.end,
                token: makeToken(),
              })
            );
            setLastEvent(`TEMP ${kind} STORED — SLOT ${parsed.slot}`);
            // 🔴 ACK the module (ozkey-28 §4 / doorlock-1.61). Until now the MCU
            // stored the credential and said NOTHING back, so the module
            // answered UNLOCK_OK purely on the strength of having written bytes
            // to a serial port — indistinguishable from writing to a crashed
            // MCU or a disconnected pin, both of which have now happened on
            // this bench. Echoing the DP is the confirmation the module waits
            // for; no echo means no success, which is the honest default.
            transmitRef.current(
              TuyaCommand.DP_REPORT,
              buildDpPayload(dp.dpId, DpType.RAW, [(parsed.slot >> 8) & 0xff, parsed.slot & 0xff]),
              `ACK -> module: ${kind} stored in slot ${parsed.slot}`
            );
            break;
          }
          case DpId.DELETE_PIN:
          case DpId.DELETE_RFID: {
            const slot = parseSlotPayload(dp.raw);
            if (slot === null) break;
            const kind = dp.dpId === DpId.DELETE_PIN ? "PIN" : "RFID";
            persistCredentials(deleteCredential(credentialsRef.current, kind, slot));
            setLastEvent(`${kind} SLOT ${slot} WIPED`);
            // Same ack contract as the add path — a revoke the module cannot
            // confirm is a credential that may still open the door.
            transmitRef.current(
              TuyaCommand.DP_REPORT,
              buildDpPayload(dp.dpId, DpType.RAW, [(slot >> 8) & 0xff, slot & 0xff]),
              `ACK -> module: ${kind} slot ${slot} wiped`
            );
            break;
          }
        }
      }
    },
    [wake, remoteUnlock, persistCredentials, tuyaPid]
  );

  useEffect(
    () => () => {
      for (const t of [sleepTimer, relockTimer, motorTimer, alarmTimer]) {
        if (t.current) clearTimeout(t.current);
      }
      for (const t of ackTimers.current) clearTimeout(t);
      ackTimers.current = [];
    },
    []
  );

  return {
    powerState,
    lockState,
    pinBuffer,
    countdown,
    lowBattery,
    mechanicalKey,
    motorActive,
    alarm,
    lastEvent,
    credentials,
    pressKey,
    ringDoorbell,
    tapRfid,
    scanFingerprint,
    triggerLowBattery,
    mcuFactoryReset,
    keypadPairingGesture,
    setMechanicalKey,
    revokeCredential,
    pushEvent,
    handleFrame,
    /** ozkey-21 — the MCU's own clock state, for the diagnostic panel. */
    mcuClock,
    mcuClockState: mcuClockState(mcuClock),
    mcuUnix: readMcuUnix(mcuClock),
    describeMcuClock: () => describeMcuClock(mcuClock),
  };
}

export type LockStateApi = ReturnType<typeof useLockState>;
