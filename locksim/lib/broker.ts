/**
 * Connection settings for the OZKEYSERV MQTT broker that LockSim reaches over
 * WebSockets. In Mode A the browser *is* the lock's ESP32 radio, so it speaks
 * MQTT-over-WS directly to the same broker OZKEYSERV uses (lab: Mosquitto on
 * 10.1.1.21, TCP :1883 for the gateway, WS :9001 for browsers).
 */

import { DEVICE_MAC } from "./provisioning";

export interface BrokerSettings {
  /**
   * Doorlock-server host. In the lab the MQTT broker (Mosquitto) and the
   * OZKEYSERV gateway (Express) run on the same box, so one host with two ports.
   */
  host: string;
  /** MQTT-over-WebSocket listener port (Mosquitto `listener 9001` / websockets). */
  wsPort: number;
  /** WebSocket path (mqtt.js convention: `/mqtt`). */
  path: string;
  /**
   * OZKEYSERV gateway HTTP API port. This is the REST endpoint host
   * (`http://host:3200/ozkeyserv/api/...`) — control-plane only. The lock's
   * credential/handshake traffic rides the broker (wsPort), not this port.
   */
  gatewayPort: number;
  /** Gateway REST base path. */
  gatewayBasePath: string;
  /** This simulated lock's hardware MAC. */
  mac: string;
  /** Deep-sleep timer-wake interval in seconds (lock "firmware" heartbeat). */
  heartbeatSeconds: number;
}

export const DEFAULT_BROKER: BrokerSettings = {
  host: "10.1.1.21",
  wsPort: 9001,
  path: "/mqtt",
  gatewayPort: 3200,
  gatewayBasePath: "/ozkeyserv/api",
  mac: DEVICE_MAC,
  heartbeatSeconds: 60,
};

const STORAGE_KEY = "locksim.broker.v1";

/**
 * Mint an ESP32-style factory MAC: Espressif OUI (A4:CF:12) + 3 random bytes.
 * Minted once per browser profile, then persisted — the software analogue of
 * the eFuse-burned base MAC real hardware surrenders at provisioning. Lets
 * many LockSim copies (one per Chrome profile) coexist on one broker/gateway.
 */
export function generateDeviceMac(): string {
  const bytes = new Uint8Array(3);
  crypto.getRandomValues(bytes);
  const tail = Array.from(bytes, (b) => b.toString(16).toUpperCase().padStart(2, "0"));
  return `A4:CF:12:${tail.join(":")}`;
}

/** Build the `ws://host:port/path` URL mqtt.js connects to (broker data path). */
export function brokerUrl(s: BrokerSettings): string {
  const path = s.path.startsWith("/") ? s.path : `/${s.path}`;
  return `ws://${s.host}:${s.wsPort}${path}`;
}

/** Build the OZKEYSERV gateway REST base URL (`http://host:3200/ozkeyserv/api`). */
export function gatewayUrl(s: BrokerSettings): string {
  const base = s.gatewayBasePath.startsWith("/") ? s.gatewayBasePath : `/${s.gatewayBasePath}`;
  return `http://${s.host}:${s.gatewayPort}${base}`;
}

export function loadBrokerSettings(): BrokerSettings {
  if (typeof window === "undefined") return DEFAULT_BROKER;
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    const stored = raw ? (JSON.parse(raw) as Partial<BrokerSettings>) : {};
    const settings = { ...DEFAULT_BROKER, ...stored };
    // Fresh profile (or pre-MAC settings blob): burn this profile's factory
    // MAC now so it survives reloads, like eFuse on a real ESP32.
    if (!stored.mac) {
      settings.mac = generateDeviceMac();
      saveBrokerSettings(settings);
    }
    return settings;
  } catch {
    return DEFAULT_BROKER;
  }
}

export function saveBrokerSettings(settings: BrokerSettings): void {
  if (typeof window === "undefined") return;
  window.localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
}

/** One line in the lock ⇄ server onboarding conversation transcript. */
export interface ConversationMessage {
  id: number;
  /** HH:MM:SS.mmm wall clock. */
  time: string;
  /** `up` = lock → server, `down` = server → lock. */
  dir: "up" | "down";
  topic: string;
  /** Human-readable one-liner describing the message. */
  summary: string;
  /** The raw payload (JSON or hex). */
  raw: string;
  error?: boolean;
}
