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

export const HEARTBEAT_SECONDS = 600; // 10-minute MQTT heartbeat loop
export const MASTER_PIN = "123456";
export const MASTER_CARD_UID = "7B 3F 91 D2";
const WAKE_HOLD_MS = 1000; // return to sleep after 1s of inactivity
const HEARTBEAT_BURST_MS = 200;
const UNLOCK_HOLD_MS = 5000; // remote/credential unlock auto-relock delay
const MOTOR_TRAVEL_MS = 900;
const ALARM_FLASH_MS = 1600;

interface UseLockStateOptions {
  /** Fire an outbound Tuya frame onto the TX line (from useTuyaProtocol). */
  transmit: (command: Byte, payload: ByteArray, ...notes: string[]) => ByteArray;
  /** Virtual Master Clock getter — all temporal checks go through this. */
  virtualNow: () => number;
}

type Timer = ReturnType<typeof setTimeout>;

/**
 * The lock motherboard state machine: deep-sleep power management, GPIO wake
 * interrupts, credential validation, clutch motor cycles and heartbeat loop.
 */
export function useLockState({ transmit, virtualNow }: UseLockStateOptions) {
  const [powerState, setPowerState] = useState<PowerState>("SLEEPING");
  const [lockState, setLockState] = useState<LockState>("LOCKED");
  const [pinBuffer, setPinBuffer] = useState("");
  const [countdown, setCountdown] = useState(HEARTBEAT_SECONDS);
  const [lowBattery, setLowBattery] = useState(false);
  const [mechanicalKey, setMechanicalKeyState] = useState(false);
  const [motorActive, setMotorActive] = useState(false);
  const [alarm, setAlarm] = useState(false);
  const [lastEvent, setLastEvent] = useState("COLD BOOT — ENTERED DEEP SLEEP");
  const [credentials, setCredentials] = useState<StoredCredential[]>([]);

  const sleepTimer = useRef<Timer | null>(null);
  const relockTimer = useRef<Timer | null>(null);
  const motorTimer = useRef<Timer | null>(null);
  const alarmTimer = useRef<Timer | null>(null);
  const fingerprintPass = useRef(true); // alternates for deterministic bench testing
  const mechanicalRef = useRef(false);
  const credentialsRef = useRef<StoredCredential[]>([]);
  const transmitRef = useRef(transmit);
  transmitRef.current = transmit;
  const nowRef = useRef(virtualNow);
  nowRef.current = virtualNow;

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

  const deny = useCallback(
    (reason: string, result: AccessResult) => {
      accessDenied();
      flashAlarm();
      setLastEvent(`ACCESS ${result === AccessResult.EXPIRED ? "EXPIRED" : "DENIED"} — ${reason}`);
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.ACCESS_RESULT, DpType.ENUM, [result]),
        `Access result: ${AccessResult[result]} — ${reason}`
      );
    },
    [flashAlarm]
  );

  const grant = useCallback(
    (source: string) => {
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.ACCESS_RESULT, DpType.ENUM, [AccessResult.SUCCESS]),
        `Access result: SUCCESS — ${source}`
      );
      unlockCycle(source);
    },
    [unlockCycle]
  );

  // ---------------------------------------------------------------------
  // 10-minute heartbeat loop
  // ---------------------------------------------------------------------
  useEffect(() => {
    const iv = setInterval(() => setCountdown((c) => (c > 0 ? c - 1 : c)), 1000);
    return () => clearInterval(iv);
  }, []);

  useEffect(() => {
    if (countdown !== 0) return;
    wake("MQTT HEARTBEAT BURST (TIMER WAKE)", HEARTBEAT_BURST_MS);
    transmitRef.current(
      TuyaCommand.HEARTBEAT,
      [],
      "MQTT heartbeat ping -> Tuya broker (10-min timer wake)"
    );
    setCountdown(HEARTBEAT_SECONDS);
  }, [countdown, wake]);

  // ---------------------------------------------------------------------
  // Credential validation against the Virtual Master Clock
  // ---------------------------------------------------------------------
  const submitPin = useCallback(
    (pin: string) => {
      transmitRef.current(
        TuyaCommand.DP_REPORT,
        buildDpPayload(DpId.UNLOCK_CHANNEL, DpType.VALUE, u32be(parseInt(pin, 10))),
        `Keypad PIN entry report: ${pin}`
      );
      if (pin === MASTER_PIN) {
        grant("KEYPAD PIN (MASTER)");
        return;
      }
      const cred = credentialsRef.current.find((c) => c.kind === "PIN" && c.value === pin);
      if (!cred) {
        deny("UNKNOWN PIN", AccessResult.DENIED);
        return;
      }
      const window = checkWindow(cred, nowRef.current());
      if (window === "VALID") {
        grant(`TEMP PIN — SLOT ${cred.slot}`);
      } else {
        deny(
          `TEMP PIN SLOT ${cred.slot} ${window === "EXPIRED" ? "EXPIRED" : "NOT YET ACTIVE"}`,
          AccessResult.EXPIRED
        );
      }
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
        setPinBuffer("");
        return;
      }
      if (key === "#") {
        setPinBuffer((buffer) => {
          if (buffer.length === 6) {
            submitPin(buffer);
          } else {
            setLastEvent(`PIN REJECTED — NEED 6 DIGITS (GOT ${buffer.length})`);
          }
          return "";
        });
        return;
      }
      setPinBuffer((buffer) => (buffer.length < 6 ? buffer + key : buffer));
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
        wake("MECHANICAL KEY OVERRIDE — CLUTCH FORCED OPEN");
      } else {
        setLockState("LOCKED");
        wake("MECHANICAL KEY REMOVED — BOLT RE-THROWN");
      }
    },
    [wake, fireMotor]
  );

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
            if (dp.type === DpType.BOOL && dp.value === 1) unlockCycle("REMOTE UNLOCK COMMAND");
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
    [wake, unlockCycle, persistCredentials]
  );

  useEffect(
    () => () => {
      for (const t of [sleepTimer, relockTimer, motorTimer, alarmTimer]) {
        if (t.current) clearTimeout(t.current);
      }
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
    handleFrame,
  };
}

export type LockStateApi = ReturnType<typeof useLockState>;
