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
}

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
export function useTuyaProtocol({ onFrame }: UseTuyaProtocolOptions) {
  const [rxLog, setRxLog] = useState<SerialLogEntry[]>([]);
  const [txLog, setTxLog] = useState<SerialLogEntry[]>([]);

  // Ref keeps the latest handler visible to callbacks captured in timers.
  const onFrameRef = useRef(onFrame);
  onFrameRef.current = onFrame;

  /** Compile and fire an outbound frame onto the TX line. */
  const transmit = useCallback((command: Byte, payload: ByteArray, ...notes: string[]) => {
    const bytes = buildFrame(command, payload);
    setTxLog((log) => push(log, makeEntry(toHexString(bytes), notes)));
    return bytes;
  }, []);

  /** Clock raw bytes into the RX line: log, validate, parse, dispatch. */
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

  /** Manual developer injection from the console input bar. */
  const injectHex = useCallback(
    (input: string) => {
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
      receiveBytes(bytes);
    },
    [receiveBytes]
  );

  const clearLogs = useCallback(() => {
    setRxLog([]);
    setTxLog([]);
  }, []);

  return { rxLog, txLog, transmit, receiveBytes, injectHex, clearLogs };
}

export type TuyaProtocolApi = ReturnType<typeof useTuyaProtocol>;
