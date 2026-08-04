"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  AccessResult,
  DpId,
  DpType,
  TuyaCommand,
  buildDpPayload,
  fromHexString,
  parseSlotPayload,
  parseTempCredential,
  u32be,
  type Byte,
  type ByteArray,
  type TuyaFrame,
} from "@/lib/tuya";
import {
  checkWindow,
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
  const onAccessRef = useRef(onAccess);
  onAccessRef.current = onAccess;
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
      if (relockTimer.current) clearTimeout(relockTimer.current);
      relockTimer.current = setTimeout(() => {
        if (mechanicalRef.current) return; // physical key holds the bolt open
        setLockState("LOCKED");
        fireMotor();
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
  const reportAccessResult = useCallback((result: AccessResult, note: string) => {
    transmitRef.current(
      TuyaCommand.DP_REPORT,
      buildDpPayload(DpId.ACCESS_RESULT, DpType.ENUM, [result]),
      note
    );
  }, []);

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

  /** Local credential entry (keypad / card / fingerprint). Unchanged ordering. */
  const grant = useCallback(
    (source: string) => {
      reportAccessResult(AccessResult.SUCCESS, `Access result: SUCCESS — ${source}`);
      unlockCycle(source);
    },
    [unlockCycle, reportAccessResult]
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
    (source: string) => {
      unlockCycle(source);
      const delay = Math.max(0, Math.round(ackDelayRef.current));
      const t = setTimeout(() => {
        ackTimers.current = ackTimers.current.filter((x) => x !== t);
        reportAccessResult(
          AccessResult.SUCCESS,
          `Access result: SUCCESS — ${source} (+${delay}ms)`
        );
      }, delay);
      ackTimers.current.push(t);
    },
    [unlockCycle, reportAccessResult]
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

  useEffect(() => {
    if (countdown !== 0) return;
    wake("MQTT HEARTBEAT BURST (TIMER WAKE)", HEARTBEAT_BURST_MS);
    transmitRef.current(
      TuyaCommand.HEARTBEAT,
      [],
      `MQTT heartbeat ping -> Tuya broker (${hbSeconds}s timer wake)`
    );
    onHeartbeatRef.current?.();
    setCountdown(hbSeconds);
  }, [countdown, wake, hbSeconds]);

  // ---------------------------------------------------------------------
  // Credential validation against the Virtual Master Clock
  // ---------------------------------------------------------------------
  const submitPin = useCallback(
    (pin: string) => {
      // u32 report caps at 9 digits — longer scramble entries report as 0.
      const numeric = pin.length <= 9 ? parseInt(pin, 10) : 0;
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.UNLOCK_CHANNEL, DpType.VALUE, u32be(numeric)),
        `Keypad PIN entry report: ${pin.length} digit(s)${pin.length > 9 ? " (scramble entry, value masked)" : ""}`
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
      const now = nowRef.current();
      const valid = candidates.find((c) => checkWindow(c, now) === "VALID");
      if (valid) {
        grant(`TEMP PIN — SLOT ${valid.slot}`);
        return;
      }
      const first = candidates[0];
      deny(
        `TEMP PIN SLOT ${first.slot} ${checkWindow(first, now) === "EXPIRED" ? "EXPIRED" : "NOT YET ACTIVE"}`,
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
      const window = checkWindow(cred, nowRef.current());
      if (window === "VALID") {
        grant(`TEMP RFID — SLOT ${cred.slot}`);
      } else {
        deny(
          `TEMP RFID SLOT ${cred.slot} ${window === "EXPIRED" ? "EXPIRED" : "NOT YET ACTIVE"}`,
          AccessResult.EXPIRED
        );
      }
    },
    [wake, grant, deny]
  );

  const scanFingerprint = useCallback(() => {
    const pass = fingerprintPass.current;
    fingerprintPass.current = !pass; // alternate pass/fail for the bench
    wake(`FINGERPRINT SENSOR INTERRUPT — ${pass ? "MATCH" : "NO MATCH"}`);
    transmitRef.current(
      TuyaCommand.DP_REPORT,
      buildDpPayload(DpId.FINGERPRINT, DpType.BOOL, [pass ? 0x01 : 0x00]),
      `Fingerprint verification: ${pass ? "SUCCESS" : "FAILED"}`
    );
    if (pass) grant("FINGERPRINT");
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
      if (frame.command !== TuyaCommand.DP_REPORT) return;

      for (const dp of frame.dataPoints) {
        switch (dp.dpId) {
          case DpId.UNLOCK_CHANNEL:
            // Was `unlockCycle(...)`, which opened the bolt and told the module
            // NOTHING — the module's whole picture of "did it work" was the fact
            // that it had written bytes to a serial port. Now it answers.
            if (dp.type === DpType.BOOL && dp.value === 1) remoteUnlock("REMOTE UNLOCK COMMAND");
            break;
          case DpId.ADD_TEMP_PIN:
          case DpId.ADD_TEMP_RFID: {
            const parsed = parseTempCredential(dp.dpId, dp.raw);
            if (!parsed) break;
            const kind = dp.dpId === DpId.ADD_TEMP_PIN ? "PIN" : "RFID";
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
            break;
          }
          case DpId.DELETE_PIN:
          case DpId.DELETE_RFID: {
            const slot = parseSlotPayload(dp.raw);
            if (slot === null) break;
            const kind = dp.dpId === DpId.DELETE_PIN ? "PIN" : "RFID";
            persistCredentials(deleteCredential(credentialsRef.current, kind, slot));
            setLastEvent(`${kind} SLOT ${slot} WIPED`);
            break;
          }
        }
      }
    },
    [wake, remoteUnlock, persistCredentials]
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
    tapRfid,
    scanFingerprint,
    triggerLowBattery,
    setMechanicalKey,
    revokeCredential,
    pushEvent,
    handleFrame,
  };
}

export type LockStateApi = ReturnType<typeof useLockState>;
