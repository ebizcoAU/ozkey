"use client";

import { useEffect, useRef, useState } from "react";
import type { SerialLogEntry } from "@/hooks/useTuyaProtocol";
import type { VirtualClockApi } from "@/hooks/useVirtualClock";
import {
  SAMPLE_ADD_TEMP_PIN_FRAME,
  SAMPLE_ADD_TEMP_RFID_FRAME,
  SAMPLE_REMOTE_UNLOCK_FRAME,
} from "@/lib/tuya";

interface SerialConsoleProps {
  rxLog: SerialLogEntry[];
  txLog: SerialLogEntry[];
  onInject: (hex: string) => void;
  onClear: () => void;
  clock: VirtualClockApi;
}

const HOUR_MS = 3_600_000;
const DAY_MS = 86_400_000;

/** One scrolling hex terminal (matrix green on black, auto-scroll to tail). */
function Terminal({ title, entries, tint }: { title: string; entries: SerialLogEntry[]; tint: string }) {
  const bodyRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const el = bodyRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [entries]);

  return (
    <div className="flex min-h-0 flex-1 flex-col rounded-lg border border-neutral-800 bg-black">
      <div className="flex items-center justify-between border-b border-neutral-800 px-3 py-1.5">
        <span className={`text-[10px] font-bold uppercase tracking-widest ${tint}`}>{title}</span>
        <span className="text-[9px] text-neutral-600">{entries.length} FRAMES</span>
      </div>
      <div ref={bodyRef} className="h-64 overflow-y-auto p-2 font-mono text-[11px] leading-relaxed">
        {entries.length === 0 && <div className="text-neutral-700">-- bus idle --</div>}
        {entries.map((e) => (
          <div key={e.id} className="mb-1.5">
            <div className="flex gap-2">
              <span className="shrink-0 text-neutral-600">[{e.time}]</span>
              <span className={e.error ? "text-red-500" : "text-green-400"}>{e.hex}</span>
            </div>
            {e.notes.map((note, i) => (
              <div key={i} className={`pl-2 text-[10px] ${e.error ? "text-red-400/80" : "text-green-600"}`}>
                └─ {note}
              </div>
            ))}
          </div>
        ))}
      </div>
    </div>
  );
}

function toDatetimeLocal(ms: number): string {
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** Bench diagnostic console: virtual clock, dual hex terminals, hex injector. */
export default function SerialConsole({ rxLog, txLog, onInject, onClear, clock }: SerialConsoleProps) {
  const [injectValue, setInjectValue] = useState("");

  const execute = () => {
    if (!injectValue.trim()) return;
    onInject(injectValue);
    setInjectValue("");
  };

  const warpButton = (label: string, delta: number) => (
    <button
      key={label}
      type="button"
      onClick={() => clock.warpBy(delta)}
      className="rounded border border-neutral-700 bg-neutral-800 px-2 py-1 text-[10px] text-neutral-300 hover:bg-neutral-700"
    >
      {label}
    </button>
  );

  const sampleButton = (label: string, frame: string) => (
    <button
      key={label}
      type="button"
      onClick={() => setInjectValue(frame)}
      className="rounded border border-green-900/60 bg-green-950/30 px-2 py-1 text-[10px] text-green-500 hover:bg-green-900/30"
      title={frame}
    >
      {label}
    </button>
  );

  return (
    <div className="flex w-full max-w-3xl flex-col gap-3">
      {/* Virtual Master Clock */}
      <div className="rounded-lg border border-neutral-800 bg-neutral-900/70 p-3">
        <div className="mb-2 flex items-center justify-between">
          <span className="text-[10px] font-bold uppercase tracking-widest text-sky-400">
            ⏱ Virtual Master Clock
          </span>
          <span
            suppressHydrationWarning
            className={`font-mono text-sm ${clock.offsetMs !== 0 ? "text-amber-300" : "text-neutral-300"}`}
          >
            {new Date(clock.virtualNowMs).toLocaleString("en-GB")}
            {clock.offsetMs !== 0 && (
              <span className="ml-2 text-[10px] text-amber-500">
                (WARPED {clock.offsetMs > 0 ? "+" : ""}{(clock.offsetMs / HOUR_MS).toFixed(1)}h)
              </span>
            )}
          </span>
        </div>
        <div className="flex flex-wrap items-center gap-1.5">
          <input
            type="datetime-local"
            suppressHydrationWarning
            value={toDatetimeLocal(clock.virtualNowMs)}
            onChange={(e) => {
              const t = new Date(e.target.value).getTime();
              if (!Number.isNaN(t)) clock.warpTo(t);
            }}
            className="rounded border border-neutral-700 bg-black px-2 py-1 font-mono text-[11px] text-green-400 [color-scheme:dark]"
          />
          {warpButton("−1d", -DAY_MS)}
          {warpButton("−1h", -HOUR_MS)}
          {warpButton("+1h", HOUR_MS)}
          {warpButton("+1d", DAY_MS)}
          <button
            type="button"
            onClick={clock.reset}
            className="rounded border border-sky-800 bg-sky-950/40 px-2 py-1 text-[10px] text-sky-300 hover:bg-sky-900/40"
          >
            Sync Real Time
          </button>
        </div>
      </div>

      {/* Dual hex terminals */}
      <div className="grid gap-3 md:grid-cols-2">
        <Terminal title="A ▸ Incoming Hex Stream (RX)" entries={rxLog} tint="text-sky-400" />
        <Terminal title="B ▸ Outgoing Hex Stream (TX)" entries={txLog} tint="text-green-400" />
      </div>

      {/* Injector */}
      <div className="rounded-lg border border-neutral-800 bg-neutral-900/70 p-3">
        <label className="mb-1.5 block text-[10px] font-bold uppercase tracking-widest text-neutral-400">
          Inject Incoming Hex Command
        </label>
        <div className="flex gap-2">
          <input
            type="text"
            value={injectValue}
            onChange={(e) => setInjectValue(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && execute()}
            placeholder="55 AA 00 06 00 05 01 01 00 01 01 0E"
            spellCheck={false}
            className="flex-1 rounded border border-neutral-700 bg-black px-3 py-2 font-mono text-[12px] text-green-400 placeholder-neutral-700 outline-none focus:border-green-700"
          />
          <button
            type="button"
            onClick={execute}
            className="rounded border border-green-700 bg-green-900/40 px-4 py-2 text-[11px] font-bold uppercase tracking-wider text-green-300 hover:bg-green-800/40"
          >
            Execute
          </button>
          <button
            type="button"
            onClick={onClear}
            className="rounded border border-neutral-700 bg-neutral-800 px-4 py-2 text-[11px] font-bold uppercase tracking-wider text-neutral-400 hover:bg-neutral-700"
          >
            Clear Logs
          </button>
        </div>
        <div className="mt-2 flex flex-wrap items-center gap-1.5">
          <span className="text-[9px] uppercase tracking-wider text-neutral-600">Presets:</span>
          {sampleButton("Remote Unlock", SAMPLE_REMOTE_UNLOCK_FRAME)}
          {sampleButton("Add Temp PIN 482915 (Slot 14)", SAMPLE_ADD_TEMP_PIN_FRAME)}
          {sampleButton("Add Temp RFID (Slot 3)", SAMPLE_ADD_TEMP_RFID_FRAME)}
        </div>
      </div>
    </div>
  );
}
