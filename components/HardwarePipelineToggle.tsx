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
 * Rendered as a compact single-row segmented control to conserve screen estate.
 */
export default function HardwarePipelineToggle({
  mode,
  onModeChange,
  serial,
}: HardwarePipelineToggleProps) {
  const hardware = mode === "HARDWARE";

  const segment = (active: boolean, onClick: () => void, tag: string, title: string, activeCls: string) => (
    <button
      type="button"
      onClick={onClick}
      title={title}
      aria-pressed={active}
      className={`flex items-center gap-1.5 rounded-md px-2.5 py-1 text-[11px] font-semibold transition-all ${
        active ? activeCls : "text-neutral-500 hover:text-neutral-300"
      }`}
    >
      <span
        className={`h-2 w-2 rounded-full ${active ? "bg-current" : "border border-neutral-600"}`}
      />
      {tag}
    </button>
  );

  return (
    <div className="flex w-full flex-wrap items-center gap-x-3 gap-y-2 rounded-xl border border-neutral-700 bg-neutral-900/80 px-3 py-2">
      <h2 className="text-[10px] font-bold uppercase tracking-[0.2em] text-neutral-300">
        ⚙ Hardware Pipeline
      </h2>

      <div className="flex items-center gap-1 rounded-lg border border-neutral-800 bg-black/40 p-0.5">
        {segment(
          !hardware,
          () => onModeChange("SOFTWARE"),
          "Mode A · Software",
          "Pure Software Emulation (No ESP32): the app simulates the Zhongshan lock motherboard and the missing Wi-Fi chip; frames loop into the internal parser.",
          "bg-emerald-950/60 text-emerald-300"
        )}
        {segment(
          hardware,
          () => onModeChange("HARDWARE"),
          "Mode B · ESP32 Wire",
          "Physical Wire Integration (ESP32-C6): Web Serial binds a 3.3V USB-UART COM port; keypad/peripheral frames stream out as a Uint8Array to the desk-side core.",
          "bg-sky-950/60 text-sky-300"
        )}
      </div>

      {hardware && (
        <div className="flex flex-1 flex-wrap items-center gap-2">
          <span className={`h-2 w-2 rounded-full ${STATUS_DOT[serial.status]}`} />
          <span className="font-mono text-[10px] text-neutral-400">
            {STATUS_TEXT[serial.status]}
            {serial.portLabel && <span className="ml-1.5 text-green-400">{serial.portLabel}</span>}
          </span>
          {serial.error && <span className="font-mono text-[10px] text-red-400">— {serial.error}</span>}
          <div className="ml-auto">
            {serial.ready ? (
              <button
                type="button"
                onClick={() => void serial.disconnect()}
                className="rounded border border-red-800 bg-red-950/50 px-2.5 py-1 text-[10px] font-semibold text-red-300 hover:bg-red-900/50"
              >
                Disconnect
              </button>
            ) : (
              <button
                type="button"
                onClick={() => void serial.connect()}
                disabled={!serial.supported || serial.status === "connecting"}
                className="rounded border border-sky-700 bg-sky-900/50 px-2.5 py-1 text-[10px] font-semibold text-sky-200 hover:bg-sky-800/50 disabled:cursor-not-allowed disabled:opacity-40"
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
