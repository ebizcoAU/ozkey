"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  AccessResult,
  DpId,
  DpType,
  TuyaCommand,
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
  const moduleTimeSourceRef = useRef(moduleTimeSource);
  moduleTimeSourceRef.current = moduleTimeSource;
  const receiveFromModuleRef = useRef(receiveFromModule);
  receiveFromModuleRef.current = receiveFromModule;
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
        grant(`TEMP PIN — SLOT ${valid.slot}`);
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
        grant(`TEMP RFID — SLOT ${cred.slot}`);
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
  const keypadPairingGesture = useCallback(() => {
    wake("KEYPAD PAIRING GESTURE (DL MCU owns the keypad)");
    transmitRef.current(
      TuyaCommand.DP_REPORT,
      buildDpPayload(DpId.PAIRING_REQUEST_PROPOSED, DpType.BOOL, [0x01]),
      "PROPOSED DP 60 -> OZKIE MCU — user asked for the BLE pairing window"
    );
    setLastEvent("PAIRING GESTURE SENT (proposed DP 60 — firmware may ignore)");
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
