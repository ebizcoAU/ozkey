"use client";

import { useCallback, useRef, useState } from "react";
import {
  annotateFrame,
  buildFrame,
  fromHexString,
  parseFrame,
  toHexString,
  type Byte,
  type ByteArray,
  type TuyaFrame,
} from "@/lib/tuya";

export interface SerialLogEntry {
  id: number;
  /** Wall-clock timestamp, HH:MM:SS.mmm. */
  time: string;
  /** Spaced hex pairs, e.g. "55 AA 00 06 00 05 01 01 00 01 01 0E". */
  hex: string;
  /** Human-readable annotation lines rendered under the hex block. */
  notes: string[];
  error?: boolean;
}

interface UseTuyaProtocolOptions {
  /** Invoked for every checksum-valid frame received on the simulated RX line. */
  onFrame: (frame: TuyaFrame) => void;
  /** Active hardware pipeline (see HardwarePipelineToggle). */
  mode: HardwareMode;
  /** Fire raw bytes out the physical USB-UART wire (Mode B). */
  sendToWire: (bytes: ByteArray) => Promise<boolean> | boolean;
  /** True when the Web Serial link is open and writable. */
  wireReady: boolean;
  /** Route a non-hex JSON payload (MQTT onboarding) to the provisioning parser. */
  onMqttPayload: (raw: string) => void;
}

/**
 * A = Pure Software Emulation: the app simulates both the lock motherboard and
 *     the missing Wi-Fi chip; outbound frames loop back to the internal parser.
 * B = Physical Wire Integration: outbound frames stream out a USB-UART port to a
 *     desk-side ESP32-C6, which owns the translation logic natively.
 */
export type HardwareMode = "SOFTWARE" | "HARDWARE";

const MAX_LOG_ENTRIES = 300;
let entrySeq = 0;

function stamp(): string {
  const d = new Date();
  return (
    [d.getHours(), d.getMinutes(), d.getSeconds()]
      .map((n) => String(n).padStart(2, "0"))
      .join(":") + `.${String(d.getMilliseconds()).padStart(3, "0")}`
  );
}

function makeEntry(hex: string, notes: string[], error = false): SerialLogEntry {
  return { id: ++entrySeq, time: stamp(), hex, notes, error };
}

function push(log: SerialLogEntry[], entry: SerialLogEntry): SerialLogEntry[] {
  const next = [...log, entry];
  return next.length > MAX_LOG_ENTRIES ? next.slice(-MAX_LOG_ENTRIES) : next;
}

/**
 * Simulated 4-wire UART bus (3.3V TTL) speaking the Tuya 0x55 0xAA MCU protocol.
 * Owns the RX/TX hex stream logs and isolates all byte-level work from the UI.
 */
export function useTuyaProtocol({
  onFrame,
  mode,
  sendToWire,
  wireReady,
  onMqttPayload,
}: UseTuyaProtocolOptions) {
  const [rxLog, setRxLog] = useState<SerialLogEntry[]>([]);
  const [txLog, setTxLog] = useState<SerialLogEntry[]>([]);

  // Refs keep the latest values visible to callbacks captured in timers.
  const onFrameRef = useRef(onFrame);
  onFrameRef.current = onFrame;
  const modeRef = useRef(mode);
  modeRef.current = mode;
  const sendToWireRef = useRef(sendToWire);
  sendToWireRef.current = sendToWire;
  const wireReadyRef = useRef(wireReady);
  wireReadyRef.current = wireReady;
  const onMqttPayloadRef = useRef(onMqttPayload);
  onMqttPayloadRef.current = onMqttPayload;

  /** Append a line to the outgoing (TX) terminal — used by non-frame uplinks. */
  const pushTxLog = useCallback((text: string, notes: string[]) => {
    setTxLog((log) => push(log, makeEntry(text, notes)));
  }, []);

  /** Append a line to the incoming (RX) terminal — used by MQTT/broker traffic. */
  const pushRxLog = useCallback((text: string, notes: string[], error = false) => {
    setRxLog((log) => push(log, makeEntry(text, notes, error)));
  }, []);

  /**
   * Compile a frame from the lock and route it. Mode A keeps it internal (TX log
   * only); Mode B additionally flushes the binary Uint8Array out the USB port.
   */
  const transmit = useCallback((command: Byte, payload: ByteArray, ...notes: string[]) => {
    const bytes = buildFrame(command, payload);
    const hardware = modeRef.current === "HARDWARE";
    const routeNote = hardware
      ? wireReadyRef.current
        ? "↳ FLUSHED TO USB-UART → ESP32-C6"
        : "↳ WIRE OFFLINE — frame not delivered"
      : "↳ internal software bus (no ESP32)";
    setTxLog((log) => push(log, makeEntry(toHexString(bytes), [...notes, routeNote])));
    if (hardware && wireReadyRef.current) void sendToWireRef.current(bytes);
    return bytes;
  }, []);

  /** Clock raw bytes into the internal virtual parser: log, validate, dispatch. */
  const receiveBytes = useCallback((bytes: ByteArray) => {
    const result = parseFrame(bytes);
    if (result.ok) {
      setRxLog((log) => push(log, makeEntry(toHexString(bytes), annotateFrame(result.frame))));
      onFrameRef.current(result.frame);
    } else {
      setRxLog((log) =>
        push(log, makeEntry(toHexString(bytes), [`FRAME REJECTED — ${result.error}`], true))
      );
    }
  }, []);

  /**
   * Route an inbound server/admin command by mode. Mode A packs it and hands it
   * to the internal virtual parser (updates LocalStorage immediately). Mode B
   * forwards it out the physical wire so the ESP32 handles translation natively.
   */
  const dispatchInbound = useCallback(
    (bytes: ByteArray, label: string) => {
      if (modeRef.current === "HARDWARE") {
        if (wireReadyRef.current) {
          setTxLog((log) =>
            push(log, makeEntry(toHexString(bytes), [`SERVER CMD FORWARDED TO ESP32 OVER UART — ${label}`]))
          );
          void sendToWireRef.current(bytes);
        } else {
          setRxLog((log) =>
            push(
              log,
              makeEntry(toHexString(bytes), [`SERVER CMD DROPPED — Mode B active, no serial link — ${label}`], true)
            )
          );
        }
      } else {
        receiveBytes(bytes);
      }
    },
    [receiveBytes]
  );

  /**
   * Manual developer injection from the console input bar. A payload beginning
   * with `{` is treated as an MQTT/OZKEYSERV JSON handshake and handed to the
   * provisioning parser; everything else is decoded as a raw Tuya hex frame.
   */
  const injectHex = useCallback(
    (input: string) => {
      if (input.trim().startsWith("{")) {
        onMqttPayloadRef.current(input);
        return;
      }
      const bytes = fromHexString(input);
      if (!bytes) {
        setRxLog((log) =>
          push(
            log,
            makeEntry(input.trim() || "(empty)", ["INJECT REJECTED — not a valid hex byte string"], true)
          )
        );
        return;
      }
      dispatchInbound(bytes, "MANUAL INJECTION");
    },
    [dispatchInbound]
  );

  /** Fire a named server/cloud admin command (e.g. Remote Unlock, Add Token). */
  const serverPush = useCallback(
    (input: string, label: string) => {
      const bytes = fromHexString(input);
      if (!bytes) return;
      dispatchInbound(bytes, label);
    },
    [dispatchInbound]
  );

  const clearLogs = useCallback(() => {
    setRxLog([]);
    setTxLog([]);
  }, []);

  return {
    rxLog,
    txLog,
    transmit,
    receiveBytes,
    injectHex,
    serverPush,
    pushTxLog,
    pushRxLog,
    clearLogs,
  };
}

export type TuyaProtocolApi = ReturnType<typeof useTuyaProtocol>;
