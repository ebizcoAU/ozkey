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
export type Byte = number;
export type ByteArray = Byte[];

export const FRAME_HEADER: readonly [Byte, Byte] = [0x55, 0xaa];
export const PROTOCOL_VERSION: Byte = 0x00;
/** Minimum frame size: header(2) + version(1) + cmd(1) + len(2) + checksum(1). */
export const MIN_FRAME_LENGTH = 7;

export enum TuyaCommand {
  HEARTBEAT = 0x00,
  DP_REPORT = 0x06,
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

function annotateDp(dp: TuyaDataPoint): string {
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
export function annotateFrame(frame: TuyaFrame): string[] {
  if (frame.command === TuyaCommand.HEARTBEAT) {
    return ["Parsed Incoming Hex -> Action: Heartbeat Ping"];
  }
  if (frame.command !== TuyaCommand.DP_REPORT) {
    return [`Parsed Incoming Hex -> Command 0x${toHexByte(frame.command)} (unhandled by lock firmware)`];
  }
  if (frame.dataPoints.length === 0) {
    return ["Parsed Incoming Hex -> DP_REPORT with no decodable data points"];
  }
  return frame.dataPoints.map((dp) => `Parsed Incoming Hex -> ${annotateDp(dp)}`);
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
