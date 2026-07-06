/**
 * Connection settings for the OZKEYSERV MQTT broker that LockSim reaches over
 * WebSockets. In Mode A the browser *is* the lock's ESP32 radio, so it speaks
 * MQTT-over-WS directly to the same broker OZKEYSERV uses (lab: Mosquitto on
 * 10.1.1.21, TCP :1883 for the gateway, WS :9001 for browsers).
 */

import { DEVICE_MAC } from "./provisioning";

export interface BrokerSettings {
  /** Broker host / doorlock-server IP. */
  host: string;
  /** WebSocket listener port (Mosquitto `listener 9001` / `protocol websockets`). */
  wsPort: number;
  /** WebSocket path (mqtt.js convention: `/mqtt`). */
  path: string;
  /** This simulated lock's hardware MAC. */
  mac: string;
}

export const DEFAULT_BROKER: BrokerSettings = {
  host: "10.1.1.21",
  wsPort: 9001,
  path: "/mqtt",
  mac: DEVICE_MAC,
};

const STORAGE_KEY = "locksim.broker.v1";

/** Build the `ws://host:port/path` URL mqtt.js connects to. */
export function brokerUrl(s: BrokerSettings): string {
  const path = s.path.startsWith("/") ? s.path : `/${s.path}`;
  return `ws://${s.host}:${s.wsPort}${path}`;
}

export function loadBrokerSettings(): BrokerSettings {
  if (typeof window === "undefined") return DEFAULT_BROKER;
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    return raw ? { ...DEFAULT_BROKER, ...(JSON.parse(raw) as Partial<BrokerSettings>) } : DEFAULT_BROKER;
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
