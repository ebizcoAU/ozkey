/**
 * Tuya MCU Hex Serial Communication Protocol engine (0x55 0xAA framing).
 *
 * Simulates the unencrypted 4-wire UART bus (3.3V TTL) between a smart lock
 * motherboard MCU and its Wi-Fi module.
 *
 * Frame structure:
 *   [0x55 0xAA] [Version 0x00] [Command (1B)] [Length (2B, big-endian)]
 *   [Payload (N bytes)] [Checksum (1B = sum of all preceding bytes % 256)]
 */

/** JS equivalent of uint8_t — every element must stay within 0x00–0xFF. */
import { enumName, type ResolvedProfile } from "./profile";

export type Byte = number;
export type ByteArray = Byte[];

export const FRAME_HEADER: readonly [Byte, Byte] = [0x55, 0xaa];
export const PROTOCOL_VERSION: Byte = 0x00;
/** Minimum frame size: header(2) + version(1) + cmd(1) + len(2) + checksum(1). */
export const MIN_FRAME_LENGTH = 7;

export enum TuyaCommand {
  HEARTBEAT = 0x00,
  DP_REPORT = 0x06,
  /**
   * ozkey-21 §2.3 — TIME SERVICE. The direction here is the whole point:
   * in Tuya's architecture the MODULE owns the clock and the MCU is its
   * client. Tuya's docs: "the module comes with a software real-time clock
   * (RTC) that provides time for the MCU… even when the module is offline,
   * the MCU can also get the time."
   *
   * We replaced the Tuya module with our own ESP32 and never implemented
   * this service — so the MCU has been asking a chip that never answers.
   * LockSim previously hid that by giving itself a browser clock, which is
   * exactly why it could never have caught the bug.
   */
  /** MCU -> module: "what is the GMT time?". Empty payload. Reply is 7 bytes. */
  GET_GMT_TIME = 0x0c,
  /** MCU -> module: "what is the LOCAL time?". Empty payload. Reply is 8 bytes. */
  GET_LOCAL_TIME = 0x1c,
  /** Module -> MCU unsolicited time push / MCU -> module subscribe (0x34). */
  TIME_NOTIFY = 0x34,
}

/**
 * Sub-commands carried in byte 0 of a 0x34 payload.
 *
 * 0x34 is MULTIPLEXED — it is not "the time command". Time subscribe/push share
 * it with the MCU-initiated factory reset, so anything dispatching on 0x34 must
 * branch on this byte rather than assume time.
 */
export enum TimeNotifySub {
  SUBSCRIBE = 0x01,
  PUSH = 0x02,
  /**
   * ozkey-22 R1 — MCU -> module factory reset. Tuya's door-lock protocol:
   * "The MCU can locally reset the module to factory settings through this
   * command. The module will be reset with data erased and enter low power
   * mode." Module replies 0x34 0x0A with data[1] 0=success, 1=failure.
   *
   * This is the PHYSICAL factory-reset gesture on the lock body, which is wired
   * to the MCU and not to our chip — so without handling this, a user holding
   * the reset button clears the lock mechanism and leaves the ESP32 still
   * owned, still bonded, still on the mesh.
   */
  FACTORY_RESET = 0x0a,
}

/** MCU -> module: "the user performed a factory reset on the lock body". */
export function buildMcuFactoryReset(): ByteArray {
  return [TimeNotifySub.FACTORY_RESET];
}

/** Module -> MCU acknowledgement. `ok:false` sends Tuya's failure byte (1). */
export function buildMcuFactoryResetAck(ok: boolean): ByteArray {
  return [TimeNotifySub.FACTORY_RESET, ok ? 0x00 : 0x01];
}

/** Tuya Data Point payload types. */
export enum DpType {
  RAW = 0x00,
  BOOL = 0x01,
  VALUE = 0x02,
  STRING = 0x03,
  ENUM = 0x04,
  BITMAP = 0x05,
}

/** Data Point IDs used by this lock's board firmware. */
export enum DpId {
  /** Outgoing: 6-digit PIN entry (VALUE). Incoming: remote unlock request (BOOL 0x01). */
  UNLOCK_CHANNEL = 0x01,
  /** Outgoing: Mifare card UID (RAW, 4 bytes). */
  RFID_CARD = 0x02,
  /** Outgoing: fingerprint verification result (BOOL). */
  FINGERPRINT = 0x03,
  /** Outgoing: low battery alarm (BOOL). */
  BATTERY_ALARM = 0x05,
  /** Outgoing: access attempt result (ENUM: see AccessResult). */
  ACCESS_RESULT = 0x08,
  /** Incoming: add temporary PIN — [Slot 2B][PIN var][Start unix 4B][End unix 4B]. */
  ADD_TEMP_PIN = 21,
  /** Incoming: delete PIN — [Slot 2B]. */
  DELETE_PIN = 22,
  /** Incoming: add temporary RFID — [Slot 2B][UID var][Start unix 4B][End unix 4B]. */
  ADD_TEMP_RFID = 23,
  /** Incoming: delete RFID — [Slot 2B]. */
  DELETE_RFID = 24,

  /**
   * ⚠️ PROPOSED — NOT A REAL TUYA DP. NOT IMPLEMENTED BY ANY SHIPPING DL MCU.
   *
   * "The user performed the pairing/maintenance gesture on the keypad."
   *
   * WHY THIS IS NEEDED: on production hardware the keypad belongs to the DL
   * MCU, not to us. The OZKIE MCU opens its BLE maintenance window on a local
   * touch — but on a real lock there IS no touch panel on our board, so a
   * member standing at the door has no way to make the lock advertise. Today
   * that gap is invisible because our dev boards happen to have their own
   * touch screen wired to the ESP32 (CST8xx @0x15), which production does not.
   *
   * WHY THE NUMBER IS A GUESS: Tuya's lock DP space is the manufacturer's,
   * and their MCU already uses low IDs we cannot enumerate. 60 is chosen to
   * sit clear of everything we know about — it is a PLACEHOLDER pending
   * manufacturer allocation, not a decision. Do not treat traffic on this DP
   * as meaningful from real hardware; only LockSim emits it.
   *
   * See ozkey-22 §7 for the allocation request.
   */
  PAIRING_REQUEST_PROPOSED = 60,
}

/** Values carried by DP 8 (ACCESS_RESULT, ENUM). */
export enum AccessResult {
  SUCCESS = 0x00,
  DENIED = 0x01,
  EXPIRED = 0x02,
}

export interface TuyaDataPoint {
  dpId: Byte;
  type: DpType;
  /** Raw value bytes exactly as carried on the wire. */
  raw: ByteArray;
  /** Numeric interpretation for BOOL / VALUE / ENUM types (NaN for RAW/STRING). */
  value: number;
}

export interface TuyaFrame {
  version: Byte;
  command: Byte;
  payload: ByteArray;
  /** Parsed DP units when command === DP_REPORT, otherwise empty. */
  dataPoints: TuyaDataPoint[];
}

export type ParseResult =
  | { ok: true; frame: TuyaFrame; bytes: ByteArray }
  | { ok: false; error: string; bytes: ByteArray };

/** 8-bit additive checksum: sum of all bytes modulo 256. */
export function checksum8(bytes: ByteArray): Byte {
  return bytes.reduce((sum, b) => sum + (b & 0xff), 0) % 256;
}

/** Compile a full wire frame from a command ID and payload. */
export function buildFrame(command: Byte, payload: ByteArray): ByteArray {
  const body: ByteArray = [
    FRAME_HEADER[0],
    FRAME_HEADER[1],
    PROTOCOL_VERSION,
    command & 0xff,
    (payload.length >> 8) & 0xff,
    payload.length & 0xff,
    ...payload.map((b) => b & 0xff),
  ];
  return [...body, checksum8(body)];
}

/** Compile a DP unit: [dpid] [type] [len hi] [len lo] [value bytes]. */
export function buildDpPayload(dpId: Byte, type: DpType, value: ByteArray): ByteArray {
  return [dpId & 0xff, type & 0xff, (value.length >> 8) & 0xff, value.length & 0xff, ...value];
}

/** Encode a number as a 4-byte big-endian VALUE payload (uint32_t). */
export function u32be(n: number): ByteArray {
  return [(n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
}

function decodeDpValue(type: DpType, raw: ByteArray): number {
  switch (type) {
    case DpType.BOOL:
    case DpType.ENUM:
      return raw[0] ?? 0;
    case DpType.VALUE:
      return raw.reduce((acc, b) => acc * 256 + b, 0);
    default:
      return NaN;
  }
}

/** Parse the DP units packed inside a DP_REPORT payload. */
export function parseDataPoints(payload: ByteArray): TuyaDataPoint[] {
  const dps: TuyaDataPoint[] = [];
  let i = 0;
  while (i + 4 <= payload.length) {
    const dpId = payload[i];
    const type = payload[i + 1] as DpType;
    const len = (payload[i + 2] << 8) | payload[i + 3];
    const raw = payload.slice(i + 4, i + 4 + len);
    if (raw.length < len) break; // truncated DP unit — stop parsing
    dps.push({ dpId, type, raw, value: decodeDpValue(type, raw) });
    i += 4 + len;
  }
  return dps;
}

/** Validate and decode a raw byte stream into a Tuya frame. */
export function parseFrame(bytes: ByteArray): ParseResult {
  if (bytes.length < MIN_FRAME_LENGTH) {
    return { ok: false, error: `FRAME TOO SHORT (${bytes.length} < ${MIN_FRAME_LENGTH} bytes)`, bytes };
  }
  if (bytes[0] !== FRAME_HEADER[0] || bytes[1] !== FRAME_HEADER[1]) {
    return { ok: false, error: "BAD HEADER — expected 55 AA", bytes };
  }
  const declaredLen = (bytes[4] << 8) | bytes[5];
  const expectedTotal = MIN_FRAME_LENGTH + declaredLen;
  if (bytes.length !== expectedTotal) {
    return {
      ok: false,
      error: `LENGTH MISMATCH — declared payload ${declaredLen}B implies ${expectedTotal}B frame, got ${bytes.length}B`,
      bytes,
    };
  }
  const expectedSum = checksum8(bytes.slice(0, -1));
  const actualSum = bytes[bytes.length - 1];
  if (expectedSum !== actualSum) {
    return {
      ok: false,
      error: `CHECKSUM FAIL — computed 0x${toHexByte(expectedSum)}, frame carries 0x${toHexByte(actualSum)}`,
      bytes,
    };
  }
  const command = bytes[3];
  const payload = bytes.slice(6, 6 + declaredLen);
  return {
    ok: true,
    bytes,
    frame: {
      version: bytes[2],
      command,
      payload,
      dataPoints: command === TuyaCommand.DP_REPORT ? parseDataPoints(payload) : [],
    },
  };
}

/**
 * Reassemble complete Tuya frames from a rolling byte buffer (Web Serial reads
 * arrive in arbitrary chunks). Resyncs past leading junk, and returns any
 * trailing partial frame in `rest` so the caller can prepend the next chunk.
 */
export function extractFrames(buffer: ByteArray): { frames: ByteArray[]; rest: ByteArray } {
  const frames: ByteArray[] = [];
  let i = 0;
  while (i < buffer.length) {
    if (buffer[i] !== FRAME_HEADER[0]) {
      i++; // hunt for a 0x55 header start
      continue;
    }
    if (i + 1 >= buffer.length) break; // need the second header byte
    if (buffer[i + 1] !== FRAME_HEADER[1]) {
      i++;
      continue;
    }
    if (i + MIN_FRAME_LENGTH > buffer.length) break; // not enough to read length yet
    const len = (buffer[i + 4] << 8) | buffer[i + 5];
    const total = MIN_FRAME_LENGTH + len;
    if (i + total > buffer.length) break; // frame still arriving
    frames.push(buffer.slice(i, i + total));
    i += total;
  }
  return { frames, rest: buffer.slice(i) };
}

export function toHexByte(b: Byte): string {
  return b.toString(16).toUpperCase().padStart(2, "0");
}

/** Format a byte array as a spaced hex pair string, e.g. "55 AA 00 06 ...". */
export function toHexString(bytes: ByteArray): string {
  return bytes.map(toHexByte).join(" ");
}

/** Parse a loose hex string ("55AA0006...", "55 aa 00 06", "0x55,0xAA") into bytes. */
export function fromHexString(input: string): ByteArray | null {
  const cleaned = input.replace(/0x/gi, "").replace(/[^0-9a-fA-F]/g, "");
  if (cleaned.length === 0 || cleaned.length % 2 !== 0) return null;
  const bytes: ByteArray = [];
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16));
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Credential-sync payloads (DPID 21–24)
// ---------------------------------------------------------------------------

export interface TempCredentialPayload {
  slot: number;
  /** PIN digits ("482915") for DP 21, or UID hex ("04 A3 7F 1C") for DP 23. */
  credential: string;
  /** Unix timestamps, seconds. */
  start: number;
  end: number;
}

/** Decode [Slot 2B][Credential var][Start 4B][End 4B] from a DP 21/23 RAW value. */
export function parseTempCredential(dpId: Byte, raw: ByteArray): TempCredentialPayload | null {
  if (raw.length < 2 + 1 + 4 + 4) return null;
  const slot = (raw[0] << 8) | raw[1];
  const credBytes = raw.slice(2, raw.length - 8);
  const start = raw
    .slice(raw.length - 8, raw.length - 4)
    .reduce((acc, b) => acc * 256 + b, 0);
  const end = raw.slice(raw.length - 4).reduce((acc, b) => acc * 256 + b, 0);
  const credential =
    dpId === DpId.ADD_TEMP_PIN
      ? credBytes.map((b) => String.fromCharCode(b)).join("")
      : toHexString(credBytes);
  if (dpId === DpId.ADD_TEMP_PIN && !/^\d+$/.test(credential)) return null;
  return { slot, credential, start, end };
}

/** Encode [Slot 2B][Credential var][Start 4B][End 4B] for a DP 21/23 RAW value. */
export function buildTempCredential(
  dpId: Byte,
  { slot, credential, start, end }: TempCredentialPayload
): ByteArray {
  const credBytes =
    dpId === DpId.ADD_TEMP_PIN
      ? credential.split("").map((c) => c.charCodeAt(0))
      : fromHexString(credential) ?? [];
  return [(slot >> 8) & 0xff, slot & 0xff, ...credBytes, ...u32be(start), ...u32be(end)];
}

// ---------------------------------------------------------------------------
// ozkey-21 — Tuya time service (0x0C / 0x1C / 0x34)
// ---------------------------------------------------------------------------

/**
 * A time reply as it appears on the wire. `valid:false` is a REAL answer, not a
 * parse failure: the module says "I was asked, and I do not know the time." A
 * module that has never reached NTP answers exactly this, and the MCU must not
 * confuse it with silence.
 */
export interface TuyaTimeReply {
  valid: boolean;
  /** Unix seconds, or null when `valid` is false. */
  unix: number | null;
  /** 1 = Monday … 7 = Sunday. Only present on the 0x1C (local) reply. */
  weekday: number | null;
}

/** MCU -> module. Empty payload; the command byte carries the whole question. */
export function buildTimeRequest(local = false): { command: Byte; payload: ByteArray } {
  return { command: local ? TuyaCommand.GET_LOCAL_TIME : TuyaCommand.GET_GMT_TIME, payload: [] };
}

/**
 * Module -> MCU reply body.
 *   byte 0   success flag (0 = module does NOT know the time)
 *   byte 1   year - 2000
 *   byte 2   month 1-12
 *   byte 3   day 1-31
 *   byte 4   hour 0-23
 *   byte 5   minute 0-59
 *   byte 6   second 0-59
 *   byte 7   weekday 1-7   (0x1C only)
 */
export function buildTimeReply(unixSeconds: number | null, local = false): ByteArray {
  if (unixSeconds === null) {
    // Flag byte 0 + zeroed fields. Tuya sends the full-length body either way.
    return local ? [0, 0, 0, 0, 0, 0, 0, 0] : [0, 0, 0, 0, 0, 0, 0];
  }
  const d = new Date(unixSeconds * 1000);
  const body: ByteArray = [
    1,
    (d.getUTCFullYear() - 2000) & 0xff,
    (d.getUTCMonth() + 1) & 0xff,
    d.getUTCDate() & 0xff,
    d.getUTCHours() & 0xff,
    d.getUTCMinutes() & 0xff,
    d.getUTCSeconds() & 0xff,
  ];
  if (!local) return body;
  // JS getUTCDay(): 0 = Sunday. Tuya: 1 = Monday … 7 = Sunday.
  const js = d.getUTCDay();
  return [...body, js === 0 ? 7 : js];
}

/** Decode a 0x0C / 0x1C reply body. Returns null only if the frame is malformed. */
export function parseTimeReply(raw: ByteArray): TuyaTimeReply | null {
  if (raw.length < 7) return null;
  if (raw[0] !== 1) return { valid: false, unix: null, weekday: null };
  const [, yr, mo, day, hh, mm, ss] = raw;
  // Tuya carries no sub-second field, so this is second-accurate by design.
  const unix = Math.floor(Date.UTC(2000 + yr, mo - 1, day, hh, mm, ss) / 1000);
  if (!Number.isFinite(unix)) return null;
  return { valid: true, unix, weekday: raw.length >= 8 ? raw[7] : null };
}

/** Module -> MCU unsolicited push: [0x02][timeType][7 time bytes]. */
export function buildTimePush(unixSeconds: number | null, local = false): ByteArray {
  return [TimeNotifySub.PUSH, local ? 0x01 : 0x00, ...buildTimeReply(unixSeconds, false)];
}

/** Decode a 0x34 payload. Returns null when it is not a time PUSH. */
export function parseTimePush(raw: ByteArray): TuyaTimeReply | null {
  if (raw.length < 2 || raw[0] !== TimeNotifySub.PUSH) return null;
  return parseTimeReply(raw.slice(2));
}

/** Decode a [Slot 2B] payload from a DP 22/24 delete command. */
export function parseSlotPayload(raw: ByteArray): number | null {
  if (raw.length < 2) return null;
  return (raw[0] << 8) | raw[1];
}

// ---------------------------------------------------------------------------
// Human-readable frame annotation for the diagnostic console
// ---------------------------------------------------------------------------

function fmtUnix(ts: number): string {
  return new Date(ts * 1000).toISOString().replace("T", " ").slice(0, 19) + " UTC";
}

/**
 * Fallback names for the INVENTED map, kept only so a frame still reads when no
 * profile is supplied. The authoritative naming now comes from the active
 * device profile — see `annotateDp`'s `profile` argument and `profileRegistry`.
 * Every name in here is fiction (`ozkey-27 §2.1`); do not add to it.
 */
const DP_NAMES: Record<number, string> = {
  [DpId.UNLOCK_CHANNEL]: "Unlock Channel",
  [DpId.RFID_CARD]: "RFID Card",
  [DpId.FINGERPRINT]: "Fingerprint",
  [DpId.BATTERY_ALARM]: "Battery Alarm",
  [DpId.ACCESS_RESULT]: "Access Result",
  [DpId.ADD_TEMP_PIN]: "Add Temporary PIN",
  [DpId.DELETE_PIN]: "Delete PIN",
  [DpId.ADD_TEMP_RFID]: "Add Temporary RFID",
  [DpId.DELETE_RFID]: "Delete RFID",
};

/**
 * Describe a DP the way the ACTIVE PROFILE describes it.
 *
 * This is the point of the whole profile layer made visible: the same bytes
 * mean different things under different profiles, and until now the console
 * asserted one interpretation as if it were fact. Under `tuya-ds013-t3` a
 * DP 21 write is `navigation_volume`; under `ozkie-legacy-v0` it is our
 * invented "Add Temporary PIN". Both are shown for what they are.
 */
function profileAnnotation(dp: TuyaDataPoint, profile: ResolvedProfile): string | null {
  const entry = profile.byDp.get(dp.dpId);
  if (!entry) {
    return `DPID ${dp.dpId} — NOT IN PROFILE ${profile.profile_id} (rejected, never forwarded)`;
  }

  const verb = entry.verb ? ` -> ${entry.verb}` : "";
  const head = `DP ${entry.dp} ${entry.name}${verb}`;

  if (entry.status === "reserved" || entry.status === "unknown") {
    // The honest answer: we know the DP exists and we cannot encode it.
    return `${head} — ${entry.status.toUpperCase()}: ${entry.blocked_by ?? "payload layout not supplied"}`;
  }

  let value: string;
  if (entry.enum) {
    value = enumName(entry, dp.value) ?? `UNKNOWN ENUM ${dp.value}`;
  } else if (Number.isNaN(dp.value)) {
    value = toHexString(dp.raw);
  } else {
    value = String(dp.value);
    if (entry.range && (dp.value < entry.range[0] || dp.value > entry.range[1])) {
      value += ` (OUT OF RANGE ${entry.range[0]}..${entry.range[1]})`;
    }
    if (entry.unit) value += ` ${entry.unit}`;
  }

  const kind = entry.access_kind ? `, kind: ${entry.access_kind}` : "";
  const fiction = entry.status === "fiction" ? "  ⚠ FICTION" : "";
  return `${head} = ${value}${kind}${fiction}`;
}

function annotateDp(dp: TuyaDataPoint, profile?: ResolvedProfile): string {
  if (profile) {
    const fromProfile = profileAnnotation(dp, profile);
    if (fromProfile) return fromProfile;
  }
  const name = DP_NAMES[dp.dpId] ?? `DPID ${dp.dpId}`;
  switch (dp.dpId) {
    case DpId.ADD_TEMP_PIN:
    case DpId.ADD_TEMP_RFID: {
      const cred = parseTempCredential(dp.dpId, dp.raw);
      if (!cred) return `Action: ${name} — MALFORMED PAYLOAD (${toHexString(dp.raw)})`;
      return (
        `Action: ${name}, Slot: ${cred.slot}, Value: ${cred.credential}, ` +
        `Valid: ${fmtUnix(cred.start)} -> Expires: ${fmtUnix(cred.end)}`
      );
    }
    case DpId.DELETE_PIN:
    case DpId.DELETE_RFID: {
      const slot = parseSlotPayload(dp.raw);
      return slot === null
        ? `Action: ${name} — MALFORMED PAYLOAD`
        : `Action: ${name}, Slot: ${slot}`;
    }
    case DpId.UNLOCK_CHANNEL:
      if (dp.type === DpType.BOOL)
        return `Action: Remote Unlock Request, Value: ${dp.value === 1 ? "UNLOCK" : "NO-OP"}`;
      return `Action: PIN Entry Report, Value: ${dp.value}`;
    case DpId.ACCESS_RESULT:
      return `Action: Access Result, Value: ${AccessResult[dp.value] ?? dp.value}`;
    default:
      return `Action: ${name}, Type: ${DpType[dp.type] ?? dp.type}, Value: ${
        Number.isNaN(dp.value) ? toHexString(dp.raw) : dp.value
      }`;
  }
}

/** Build "Parsed Incoming Hex -> ..." annotation lines for a decoded frame. */
export function annotateFrame(frame: TuyaFrame, profile?: ResolvedProfile): string[] {
  if (frame.command === TuyaCommand.HEARTBEAT) {
    return ["Parsed Incoming Hex -> Action: Heartbeat Ping"];
  }
  if (
    frame.command === TuyaCommand.GET_GMT_TIME ||
    frame.command === TuyaCommand.GET_LOCAL_TIME
  ) {
    const local = frame.command === TuyaCommand.GET_LOCAL_TIME;
    const kind = local ? "LOCAL" : "GMT";
    if (frame.payload.length === 0)
      return [`Parsed Incoming Hex -> Time Request (${kind}) — MCU asking the module`];
    const t = parseTimeReply(frame.payload);
    if (!t) return [`Parsed Incoming Hex -> Time Reply (${kind}) — MALFORMED`];
    if (!t.valid)
      return [
        `Parsed Incoming Hex -> Time Reply (${kind}) — module reports NO VALID TIME. ` +
          `MCU stays UNSYNCED; every temporal check fails closed.`,
      ];
    return [
      `Parsed Incoming Hex -> Time Reply (${kind}) — ${fmtUnix(t.unix!)}` +
        (t.weekday ? ` (weekday ${t.weekday})` : ""),
    ];
  }
  if (frame.command === TuyaCommand.TIME_NOTIFY) {
    // 0x34 is multiplexed — branch on the sub-command byte, never assume time.
    if (frame.payload[0] === TimeNotifySub.FACTORY_RESET) {
      if (frame.payload.length >= 2)
        return [
          `Parsed Incoming Hex -> FACTORY RESET ACK (0x34 0x0A) — module reports ` +
            `${frame.payload[1] === 0 ? "SUCCESS" : "FAILURE"}`,
        ];
      return [
        "Parsed Incoming Hex -> FACTORY RESET (0x34 0x0A) — MCU telling the module to wipe itself",
      ];
    }
    const t = parseTimePush(frame.payload);
    if (!t) return ["Parsed Incoming Hex -> Time Notify (0x34) subscribe/ack"];
    return t.valid
      ? [`Parsed Incoming Hex -> Time Push (0x34) — ${fmtUnix(t.unix!)}`]
      : ["Parsed Incoming Hex -> Time Push (0x34) — module reports NO VALID TIME"];
  }
  if (frame.command !== TuyaCommand.DP_REPORT) {
    return [`Parsed Incoming Hex -> Command 0x${toHexByte(frame.command)} (unhandled by lock firmware)`];
  }
  if (frame.dataPoints.length === 0) {
    return ["Parsed Incoming Hex -> DP_REPORT with no decodable data points"];
  }
  return frame.dataPoints.map((dp) => `Parsed Incoming Hex -> ${annotateDp(dp, profile)}`);
}

/** Reference frame: remote unlock request (DP 1, BOOL, value 0x01) with valid checksum. */
export const SAMPLE_REMOTE_UNLOCK_FRAME = toHexString(
  buildFrame(TuyaCommand.DP_REPORT, buildDpPayload(DpId.UNLOCK_CHANNEL, DpType.BOOL, [0x01]))
);

/** Reference frame: add temp PIN 482915 to slot 14, valid 2026-01-01 → 2026-12-31 UTC. */
export const SAMPLE_ADD_TEMP_PIN_FRAME = toHexString(
  buildFrame(
    TuyaCommand.DP_REPORT,
    buildDpPayload(
      DpId.ADD_TEMP_PIN,
      DpType.RAW,
      buildTempCredential(DpId.ADD_TEMP_PIN, {
        slot: 14,
        credential: "482915",
        start: 1767225600, // 2026-01-01 00:00:00 UTC
        end: 1798761599, // 2026-12-31 23:59:59 UTC
      })
    )
  )
);

/** Reference frame: add temp RFID card 04 A3 7F 1C to slot 3, same validity window. */
export const SAMPLE_ADD_TEMP_RFID_FRAME = toHexString(
  buildFrame(
    TuyaCommand.DP_REPORT,
    buildDpPayload(
      DpId.ADD_TEMP_RFID,
      DpType.RAW,
      buildTempCredential(DpId.ADD_TEMP_RFID, {
        slot: 3,
        credential: "04 A3 7F 1C",
        start: 1767225600,
        end: 1798761599,
      })
    )
  )
);
