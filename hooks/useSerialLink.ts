"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { extractFrames, type ByteArray } from "@/lib/tuya";

export type SerialStatus =
  | "unsupported"
  | "idle"
  | "connecting"
  | "connected"
  | "error";

interface UseSerialLinkOptions {
  /** Invoked for each complete Tuya frame reassembled off the physical wire. */
  onFrameBytes: (bytes: ByteArray) => void;
  /** Tuya MCUs default to 9600 8N1 on the 3.3V TTL UART. */
  baudRate?: number;
}

// Minimal Web Serial typings — the DOM lib doesn't ship them everywhere.
interface SerialPortLike {
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  getInfo?: () => { usbVendorId?: number; usbProductId?: number };
}

/**
 * Mode B transport: the browser Web Serial API bound to a USB-to-UART bridge on
 * a desk-side ESP32-C6. Owns the physical read loop (reassembling frames) and a
 * binary Uint8Array writer. Isolated so the protocol engine stays transport-agnostic.
 */
export function useSerialLink({ onFrameBytes, baudRate = 9600 }: UseSerialLinkOptions) {
  const [status, setStatus] = useState<SerialStatus>("idle");
  const [portLabel, setPortLabel] = useState("");
  const [error, setError] = useState("");

  const supported = typeof navigator !== "undefined" && "serial" in navigator;

  const portRef = useRef<SerialPortLike | null>(null);
  const readerRef = useRef<ReadableStreamDefaultReader<Uint8Array> | null>(null);
  const writerRef = useRef<WritableStreamDefaultWriter<Uint8Array> | null>(null);
  const bufferRef = useRef<ByteArray>([]);
  const keepReading = useRef(false);
  const onFrameRef = useRef(onFrameBytes);
  onFrameRef.current = onFrameBytes;

  useEffect(() => {
    if (!supported) setStatus("unsupported");
  }, [supported]);

  const readLoop = useCallback(async () => {
    const port = portRef.current;
    if (!port?.readable) return;
    const reader = port.readable.getReader();
    readerRef.current = reader;
    try {
      while (keepReading.current) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) {
          for (const b of value) bufferRef.current.push(b);
          const { frames, rest } = extractFrames(bufferRef.current);
          bufferRef.current = rest;
          for (const f of frames) onFrameRef.current(f);
        }
      }
    } catch {
      // Reader cancelled on disconnect — expected.
    } finally {
      try {
        reader.releaseLock();
      } catch {
        /* already released */
      }
    }
  }, []);

  const connect = useCallback(async () => {
    if (!supported) {
      setStatus("unsupported");
      setError("Web Serial API unavailable — use Chrome/Edge over HTTPS or localhost");
      return;
    }
    try {
      setStatus("connecting");
      setError("");
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const port: SerialPortLike = await (navigator as any).serial.requestPort();
      await port.open({ baudRate });
      portRef.current = port;
      const info = port.getInfo?.() ?? {};
      setPortLabel(
        info.usbVendorId !== undefined
          ? `USB ${info.usbVendorId.toString(16).padStart(4, "0")}:${(info.usbProductId ?? 0)
              .toString(16)
              .padStart(4, "0")} @ ${baudRate} 8N1`
          : `Serial @ ${baudRate} 8N1`
      );
      writerRef.current = port.writable?.getWriter() ?? null;
      keepReading.current = true;
      setStatus("connected");
      void readLoop();
    } catch (e) {
      const err = e as { name?: string; message?: string };
      if (err?.name === "NotFoundError") {
        // User dismissed the port picker — no hardware selected.
        setStatus("idle");
      } else {
        setError(err?.message ?? "connection failed");
        setStatus("error");
      }
    }
  }, [supported, baudRate, readLoop]);

  /** Fire a raw byte frame out the physical USB port as a binary Uint8Array. */
  const write = useCallback(async (bytes: ByteArray): Promise<boolean> => {
    const writer = writerRef.current;
    if (!writer) return false;
    try {
      await writer.write(new Uint8Array(bytes));
      return true;
    } catch (e) {
      setError((e as Error)?.message ?? "wire write failed");
      return false;
    }
  }, []);

  const disconnect = useCallback(async () => {
    keepReading.current = false;
    try {
      await readerRef.current?.cancel();
    } catch {
      /* noop */
    }
    try {
      await writerRef.current?.close();
    } catch {
      /* noop */
    }
    try {
      await portRef.current?.close();
    } catch {
      /* noop */
    }
    portRef.current = null;
    writerRef.current = null;
    readerRef.current = null;
    bufferRef.current = [];
    setPortLabel("");
    setStatus(supported ? "idle" : "unsupported");
  }, [supported]);

  useEffect(
    () => () => {
      keepReading.current = false;
    },
    []
  );

  const ready = status === "connected";

  return { status, portLabel, error, supported, ready, connect, disconnect, write };
}

export type SerialLinkApi = ReturnType<typeof useSerialLink>;
