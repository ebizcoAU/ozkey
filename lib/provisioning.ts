/**
 * Matter-style BLE onboarding for the lock: the device advertises its hardware
 * MAC to the OZKEYSERV/ broker, then captures a JSON provisioning handshake off
 * the MQTT topic pipeline `hotel/rooms/+/lock/command` and persists the assigned
 * network identity. Pure logic — no React, no DOM — so it unit-tests standalone.
 */

/** Factory-burned hardware address advertised during onboarding. */
export const DEVICE_MAC = "AA:BB:CC:11:22:33";

/**
 * Legacy label for the announce payload; the canonical MQTT destination is
 * ANNOUNCE_TOPIC (see ozkey-02 §3.1). Kept only for backward compatibility.
 */
export const BROADCAST_TOPIC = "OZKEYSERV/provision/announce";

/** Canonical discovery topic the lock announces its MAC on (lock → server). */
export const ANNOUNCE_TOPIC = "hotel/locks/unpaired/heartbeat";

/** MQTT topic filter the room-assignment handshake / commands arrive on. */
export const ONBOARDING_TOPIC = "hotel/rooms/+/lock/command";

/** Steady-state heartbeat topic for a provisioned room (lock → server). */
export function heartbeatTopic(roomNo: string): string {
  return `hotel/rooms/${roomNo}/lock/heartbeat`;
}

const STORAGE_KEY = "locksim.provisioning.v1";

/** Network identity written to flash once the lock is paired to a room. */
export interface NetworkProvisioning {
  mac: string;
  assigned_room_no: string;
  server_ip: string;
  mac_token: string;
  /** Wall-clock ms when registration completed. */
  provisionedAt: number;
}

export type OnboardingResult =
  | {
      ok: true;
      topic: string;
      mac: string;
      roomNo: string;
      serverIp: string;
      macToken: string;
    }
  | { ok: false; error: string };

/** Match a concrete MQTT topic against a filter using `+` (single) / `#` (rest). */
export function topicMatches(filter: string, topic: string): boolean {
  const f = filter.split("/");
  const t = topic.split("/");
  for (let i = 0; i < f.length; i++) {
    if (f[i] === "#") return true;
    if (i >= t.length) return false;
    if (f[i] === "+") continue;
    if (f[i] !== t[i]) return false;
  }
  return f.length === t.length;
}

/** Issue a broker-side network token when the handshake omits one. */
export function makeMacToken(): string {
  const seg = () =>
    Math.floor(Math.random() * 0x10000)
      .toString(16)
      .toUpperCase()
      .padStart(4, "0");
  return `OZK-${seg()}-${seg()}-${seg()}`;
}

/** Compact JSON the device publishes up to OZKEYSERV/ to announce its presence. */
export function buildBroadcastPayload(mac: string): string {
  return JSON.stringify({
    topic: BROADCAST_TOPIC,
    mac,
    device: "tuya-lock-zs-mb",
    fw: "1.4.2",
    capabilities: ["ble", "matter", "tuya-mcu"],
    rssi: -47,
    ts: Date.now(),
  });
}

/** Ready-made valid onboarding handshake for the given MAC and room. */
export function buildSampleOnboarding(mac: string, room = "412"): string {
  return JSON.stringify(
    {
      topic: `hotel/rooms/${room}/lock/command`,
      op: "provision_assign",
      mac,
      room_no: room,
      server_ip: "10.20.0.5",
      mac_token: "OZK-7F3A-C210-9E4D",
      issued_by: "OZKEYSERV/",
    },
    null,
    2
  );
}

function coerceRoom(value: unknown): string {
  if (typeof value === "string") return value.trim();
  if (typeof value === "number" && Number.isFinite(value)) return String(value);
  return "";
}

/**
 * Validate a raw MQTT payload as a provisioning handshake for `expectedMac`.
 * Enforces the topic filter, a matching MAC, and a room assignment before the
 * caller commits any network variables to storage.
 */
export function parseOnboardingPayload(raw: string, expectedMac: string): OnboardingResult {
  let obj: Record<string, unknown>;
  try {
    const parsed = JSON.parse(raw);
    if (typeof parsed !== "object" || parsed === null) {
      return { ok: false, error: "payload root is not a JSON object" };
    }
    obj = parsed as Record<string, unknown>;
  } catch {
    return { ok: false, error: "payload is not valid JSON" };
  }

  const topic = obj.topic;
  if (typeof topic !== "string") {
    return { ok: false, error: "missing 'topic' field" };
  }
  if (!topicMatches(ONBOARDING_TOPIC, topic)) {
    return { ok: false, error: `topic '${topic}' does not match ${ONBOARDING_TOPIC}` };
  }

  const mac = typeof obj.mac === "string" ? obj.mac.trim() : "";
  if (!mac) {
    return { ok: false, error: "missing 'mac' field" };
  }
  if (mac.toUpperCase() !== expectedMac.toUpperCase()) {
    return { ok: false, error: `MAC ${mac} does not match this device (${expectedMac})` };
  }

  const roomNo = coerceRoom(obj.room_no) || topic.split("/")[2] || "";
  if (!roomNo) {
    return { ok: false, error: "handshake carries no 'room_no' assignment" };
  }

  const serverIp =
    typeof obj.server_ip === "string"
      ? obj.server_ip.trim()
      : typeof obj.serverIp === "string"
        ? obj.serverIp.trim()
        : "";
  if (!serverIp) {
    return { ok: false, error: "missing 'server_ip' for the broker session" };
  }

  const macToken =
    typeof obj.mac_token === "string" && obj.mac_token.trim() ? obj.mac_token.trim() : makeMacToken();

  return { ok: true, topic, mac: mac.toUpperCase(), roomNo, serverIp, macToken };
}

export function loadProvisioning(): NetworkProvisioning | null {
  if (typeof window === "undefined") return null;
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as NetworkProvisioning) : null;
  } catch {
    return null;
  }
}

export function saveProvisioning(record: NetworkProvisioning): void {
  if (typeof window === "undefined") return;
  window.localStorage.setItem(STORAGE_KEY, JSON.stringify(record));
}

export function clearProvisioning(): void {
  if (typeof window === "undefined") return;
  window.localStorage.removeItem(STORAGE_KEY);
}
