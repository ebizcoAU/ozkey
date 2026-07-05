"use client";

import type { HardwareMode } from "@/hooks/useTuyaProtocol";
import type { SerialLinkApi, SerialStatus } from "@/hooks/useSerialLink";

interface HardwarePipelineToggleProps {
  mode: HardwareMode;
  onModeChange: (mode: HardwareMode) => void;
  serial: SerialLinkApi;
}

const STATUS_TEXT: Record<SerialStatus, string> = {
  unsupported: "WEB SERIAL UNSUPPORTED (use Chrome/Edge)",
  idle: "NO PORT SELECTED",
  connecting: "REQUESTING PORT…",
  connected: "LINK ESTABLISHED",
  error: "LINK ERROR",
};

const STATUS_DOT: Record<SerialStatus, string> = {
  unsupported: "bg-neutral-600",
  idle: "bg-neutral-500",
  connecting: "bg-amber-400 animate-pulse",
  connected: "bg-green-400 shadow-[0_0_8px_2px_rgba(74,222,128,0.7)]",
  error: "bg-red-500",
};

/**
 * Master Hardware Pipeline Selection toggle — switches the whole simulator
 * backend between Mode A (pure software emulation) and Mode B (physical wire).
 */
export default function HardwarePipelineToggle({
  mode,
  onModeChange,
  serial,
}: HardwarePipelineToggleProps) {
  const hardware = mode === "HARDWARE";

  const card = (
    active: boolean,
    onClick: () => void,
    tag: string,
    title: string,
    body: string,
    accent: string
  ) => (
    <button
      type="button"
      onClick={onClick}
      className={`flex-1 rounded-lg border p-3 text-left transition-all ${
        active
          ? `${accent} shadow-[0_0_0_1px_rgba(255,255,255,0.05)]`
          : "border-neutral-800 bg-neutral-900/40 opacity-60 hover:opacity-90"
      }`}
    >
      <div className="flex items-center gap-2">
        <span
          className={`h-2.5 w-2.5 rounded-full ${
            active ? "bg-current" : "border border-neutral-600"
          }`}
        />
        <span className="text-[10px] font-bold uppercase tracking-widest">{tag}</span>
      </div>
      <div className="mt-1 text-sm font-semibold text-neutral-100">{title}</div>
      <div className="mt-0.5 text-[10px] leading-snug text-neutral-400">{body}</div>
    </button>
  );

  return (
    <div className="w-full rounded-xl border border-neutral-700 bg-neutral-900/80 p-4">
      <div className="mb-3 flex items-center justify-between">
        <h2 className="text-xs font-bold uppercase tracking-[0.2em] text-neutral-200">
          ⚙ Hardware Pipeline Selection
        </h2>
        <span className="text-[10px] text-neutral-500">
          Active backend:{" "}
          <span className={hardware ? "text-sky-400" : "text-emerald-400"}>
            {hardware ? "MODE B · PHYSICAL WIRE" : "MODE A · SOFTWARE EMULATION"}
          </span>
        </span>
      </div>

      <div className="flex flex-col gap-3 md:flex-row">
        {card(
          !hardware,
          () => onModeChange("SOFTWARE"),
          "Mode A",
          "Pure Software Emulation (No ESP32)",
          "App simulates the Zhongshan lock motherboard and the missing Wi-Fi chip. Admin/server commands are encoded and looped into the internal parser to update LocalStorage instantly.",
          "border-emerald-700 bg-emerald-950/40 text-emerald-300"
        )}
        {card(
          hardware,
          () => onModeChange("HARDWARE"),
          "Mode B",
          "Physical Wire Integration (ESP32-C6)",
          "Web Serial API binds a 3.3V USB-UART COM port. Keypad/peripheral frames stream out as Uint8Array; incoming server commands are forwarded over the wire for native translation.",
          "border-sky-700 bg-sky-950/40 text-sky-300"
        )}
      </div>

      {hardware && (
        <div className="mt-3 flex flex-wrap items-center gap-3 rounded-lg border border-sky-900/50 bg-black/50 p-2.5">
          <span className={`h-2.5 w-2.5 rounded-full ${STATUS_DOT[serial.status]}`} />
          <span className="font-mono text-[11px] text-neutral-300">
            {STATUS_TEXT[serial.status]}
            {serial.portLabel && <span className="ml-2 text-green-400">{serial.portLabel}</span>}
          </span>
          {serial.error && (
            <span className="font-mono text-[10px] text-red-400">— {serial.error}</span>
          )}
          <div className="ml-auto flex gap-2">
            {serial.ready ? (
              <button
                type="button"
                onClick={() => void serial.disconnect()}
                className="rounded border border-red-800 bg-red-950/50 px-3 py-1 text-[11px] font-semibold text-red-300 hover:bg-red-900/50"
              >
                Disconnect Port
              </button>
            ) : (
              <button
                type="button"
                onClick={() => void serial.connect()}
                disabled={!serial.supported || serial.status === "connecting"}
                className="rounded border border-sky-700 bg-sky-900/50 px-3 py-1 text-[11px] font-semibold text-sky-200 hover:bg-sky-800/50 disabled:cursor-not-allowed disabled:opacity-40"
              >
                {serial.status === "connecting" ? "Requesting…" : "Connect USB-UART Port"}
              </button>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
