"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { MqttClient } from "mqtt";
import { brokerUrl, type BrokerSettings } from "@/lib/broker";

export type MqttStatus = "offline" | "connecting" | "connected" | "error";

interface UseMqttLinkOptions {
  /** Topic filters to subscribe on every (re)connect. */
  subscriptions: string[];
  /** Delivered for each inbound broker message (server → lock). */
  onMessage: (topic: string, payload: string) => void;
}

/**
 * Mode A network transport: the simulated lock's ESP32 radio, implemented as an
 * MQTT-over-WebSocket client (mqtt.js) to the OZKEYSERV broker. The browser can
 * speak MQTT only over WS, so this is the real uplink — no HTTP `/sim/*` bridge.
 */
export function useMqttLink({ subscriptions, onMessage }: UseMqttLinkOptions) {
  const [status, setStatus] = useState<MqttStatus>("offline");
  const [error, setError] = useState("");
  const [url, setUrl] = useState("");

  const clientRef = useRef<MqttClient | null>(null);
  const subsRef = useRef(subscriptions);
  subsRef.current = subscriptions;
  const onMessageRef = useRef(onMessage);
  onMessageRef.current = onMessage;

  const disconnect = useCallback(() => {
    const client = clientRef.current;
    clientRef.current = null;
    if (client) client.end(true);
    setStatus("offline");
  }, []);

  const connect = useCallback(async (settings: BrokerSettings) => {
    // Tear down any prior client so settings changes take effect cleanly.
    if (clientRef.current) {
      clientRef.current.end(true);
      clientRef.current = null;
    }
    const target = brokerUrl(settings);
    setUrl(target);
    setError("");
    setStatus("connecting");
    try {
      const mqtt = (await import("mqtt")).default;
      const client = mqtt.connect(target, {
        clientId: `locksim-${settings.mac.replace(/[^0-9a-zA-Z]/g, "")}-${Date.now() % 100000}`,
        reconnectPeriod: 4000,
        connectTimeout: 6000,
        clean: true,
      });
      clientRef.current = client;

      client.on("connect", () => {
        setStatus("connected");
        setError("");
        for (const topic of subsRef.current) client.subscribe(topic, { qos: 1 });
      });
      client.on("reconnect", () => setStatus("connecting"));
      client.on("close", () => {
        if (clientRef.current === client) setStatus((s) => (s === "connected" ? "connecting" : s));
      });
      client.on("error", (e: Error) => {
        setError(e.message);
        setStatus("error");
      });
      client.on("message", (topic: string, payload: Uint8Array) => {
        onMessageRef.current(topic, new TextDecoder().decode(payload));
      });
    } catch (e) {
      setError((e as Error)?.message ?? "connect failed");
      setStatus("error");
    }
  }, []);

  /** Publish a payload; returns false if the link is down. */
  const publish = useCallback((topic: string, payload: string): boolean => {
    const client = clientRef.current;
    if (!client || !client.connected) return false;
    client.publish(topic, payload, { qos: 1 });
    return true;
  }, []);

  useEffect(
    () => () => {
      clientRef.current?.end(true);
    },
    []
  );

  return { status, error, url, connected: status === "connected", connect, disconnect, publish };
}

export type MqttLinkApi = ReturnType<typeof useMqttLink>;
