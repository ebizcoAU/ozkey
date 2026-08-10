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
/**
 * Silicon Labs CP210x USB-UART bridge. This is the adapter on the lock's MCU
 * wire (GPIO16/17, 9600 8N1) — see doorlock19.ino's TUYA_TX_PIN/TUYA_RX_PIN.
 * Used to tell that adapter apart from the ESP32 boards' own native-USB
 * consoles, which also appear in getPorts() and must never be auto-opened.
 */
const CP210X_VENDOR_ID = 0x10c4;

/**
 * The port the operator last chose BY HAND, remembered across reloads
 * (operator, 2026-08-10: "u need human to manually select or config to save it
 * after the selection").
 *
 * Web Serial gives no stable per-port identifier we are allowed to persist, so
 * we store what getInfo() does expose — USB vendor + product id — and match on
 * that next load. That is enough to tell the CP2102 MCU wire apart from the
 * ESP32 boards' native-USB consoles, which is the confusion that had LockSim
 * writing every frame into the bridge.
 *
 * LIMIT, stated rather than hidden: two IDENTICAL adapters (same VID:PID) are
 * indistinguishable this way. If that ever happens the match is ambiguous and
 * we fall back to the manual picker instead of guessing — see the autoconnect
 * effect.
 */
const PORT_MEMORY_KEY = "locksim.serial.port.v1";

interface RememberedPort {
  usbVendorId?: number;
  usbProductId?: number;
}

function loadRememberedPort(): RememberedPort | null {
  if (typeof window === "undefined") return null;
  try {
    const raw = window.localStorage.getItem(PORT_MEMORY_KEY);
    return raw ? (JSON.parse(raw) as RememberedPort) : null;
  } catch {
    return null;
  }
}

function rememberPort(info: RememberedPort): void {
  if (typeof window === "undefined") return;
  try {
    window.localStorage.setItem(PORT_MEMORY_KEY, JSON.stringify(info));
  } catch {
    /* storage disabled — autoconnect simply won't persist */
  }
}

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

  /**
   * Open a port we already hold. Split out of `connect` so the auto-reconnect
   * path (which must NOT call requestPort) shares the exact same open/label/
   * writer/read-loop sequence — two copies of this drift.
   */
  const attach = useCallback(
    async (port: SerialPortLike) => {
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
    },
    [baudRate, readLoop]
  );

  /**
   * AUTO-RECONNECT on load (operator, 2026-08-10: "auto uart connection").
   *
   * `requestPort()` needs a user gesture and no browser will let us skip that —
   * but `getPorts()` returns ports this origin has ALREADY been granted, with
   * no gesture at all. So the port picker is a once-per-browser-profile cost,
   * not once-per-refresh, which is what was actually painful: every reload
   * meant re-picking the CP2102 by hand, and the boot time request was spent
   * before the wire was up.
   *
   * If the device is unplugged or permission was revoked, getPorts() simply
   * returns nothing and we fall back to the manual button.
   */
  useEffect(() => {
    if (!supported) return;
    let cancelled = false;
    void (async () => {
      try {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const ports: SerialPortLike[] = await (navigator as any).serial.getPorts();
        if (cancelled || !ports.length || portRef.current) return;

        // 🔴 NEVER just take ports[0].
        //
        // getPorts() returns EVERY port this origin has ever been granted, in
        // no useful order. On this bench that list also contains the ESP32-C6
        // boards' own native-USB consoles — and autoconnect duly picked the
        // BRIDGE, so LockSim reported "UART Connected", logged "FLUSHED TO
        // USB-UART", and wrote every frame into a device that was never
        // listening. The lock's rx counter sat frozen while the UI insisted
        // the link was up, which is the worst kind of wrong: confidently.
        //
        // So autoconnect only ever reopens what a HUMAN already chose.
        const saved = loadRememberedPort();
        if (!saved) return; // never picked one — the button is the way in

        const matches = ports.filter((p) => {
          const i = p.getInfo?.() ?? {};
          return (
            i.usbVendorId === saved.usbVendorId && i.usbProductId === saved.usbProductId
          );
        });
        // Exactly one, or we do not guess. 0 = adapter unplugged. >1 = two
        // identical adapters, genuinely ambiguous by VID:PID alone.
        if (matches.length !== 1) return;

        setStatus("connecting");
        await attach(matches[0]);
      } catch {
        // Autoconnect is best-effort: a failure here must never block the
        // manual path, and the device simply may not be plugged in yet.
        if (!cancelled) setStatus("idle");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [supported, attach]);

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
      await attach(port);
      // Remember the HUMAN's choice so every later load reopens this exact
      // device and never has to guess (see PORT_MEMORY_KEY).
      const info = port.getInfo?.() ?? {};
      rememberPort({ usbVendorId: info.usbVendorId, usbProductId: info.usbProductId });
      // Not fatal, but say so: the MCU wire on this bench is a CP2102, and
      // picking an ESP32's own USB console instead is exactly the mistake that
      // produced a "connected" link carrying frames nobody received.
      if (info.usbVendorId !== undefined && info.usbVendorId !== CP210X_VENDOR_ID) {
        setError(
          `Selected USB ${info.usbVendorId.toString(16)}:${(info.usbProductId ?? 0).toString(16)} — ` +
            `not a CP210x. If the lock's MCU never receives anything, this is likely the wrong device.`
        );
      }
      return;
    } catch (e) {
      const err = e as { name?: string; message?: string };
      if (err?.name === "NotFoundError") {
        setStatus("idle"); // user dismissed the picker
      } else {
        setError(err?.message ?? "connection failed");
        setStatus("error");
      }
      return;
    }
  }, [supported, attach]);

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
