"use client";

import { useEffect, useState } from "react";
import { brokerUrl, type BrokerSettings } from "@/lib/broker";
import type { MqttLinkApi } from "@/hooks/useMqttLink";

interface SettingsDialogProps {
  open: boolean;
  settings: BrokerSettings;
  mqtt: MqttLinkApi;
  onClose: () => void;
  onSave: (next: BrokerSettings) => void;
}

const STATUS_STYLE: Record<string, string> = {
  offline: "text-neutral-400",
  connecting: "text-amber-300",
  connected: "text-green-400",
  error: "text-red-400",
};

/** Modal for the doorlock server (MQTT broker) connection settings. */
export default function SettingsDialog({ open, settings, mqtt, onClose, onSave }: SettingsDialogProps) {
  const [draft, setDraft] = useState<BrokerSettings>(settings);

  useEffect(() => {
    if (open) setDraft(settings);
  }, [open, settings]);

  if (!open) return null;

  const field = (
    label: string,
    value: string | number,
    onChange: (v: string) => void,
    type: "text" | "number" = "text",
    placeholder = ""
  ) => (
    <label className="flex flex-col gap-1">
      <span className="text-[10px] uppercase tracking-wider text-neutral-400">{label}</span>
      <input
        type={type}
        value={value}
        placeholder={placeholder}
        spellCheck={false}
        onChange={(e) => onChange(e.target.value)}
        className="rounded border border-neutral-700 bg-black px-3 py-2 font-mono text-[12px] text-green-300 outline-none focus:border-green-700"
      />
    </label>
  );

  const save = () => {
    onSave(draft);
    onClose();
  };

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/70 p-4"
      onClick={onClose}
    >
      <div
        className="w-full max-w-md rounded-xl border border-neutral-700 bg-neutral-900 p-5 shadow-2xl"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="mb-1 flex items-center justify-between">
          <h2 className="text-sm font-bold tracking-tight text-neutral-100">Doorlock Server Settings</h2>
          <button
            type="button"
            onClick={onClose}
            className="text-neutral-500 hover:text-neutral-200"
            aria-label="Close"
          >
            ✕
          </button>
        </div>
        <p className="mb-4 text-[11px] leading-snug text-neutral-500">
          The simulated lock connects to the OZKEYSERV broker over MQTT-over-WebSocket. The gateway
          uses the same broker on TCP :1883.
        </p>

        <div className="grid grid-cols-2 gap-3">
          {field("Broker Host / IP", draft.host, (v) => setDraft({ ...draft, host: v }), "text", "10.1.1.21")}
          {field("WebSocket Port", draft.wsPort, (v) => setDraft({ ...draft, wsPort: Number(v) || 0 }), "number")}
          {field("WS Path", draft.path, (v) => setDraft({ ...draft, path: v }), "text", "/mqtt")}
          {field("Device MAC", draft.mac, (v) => setDraft({ ...draft, mac: v }), "text", "AA:BB:CC:11:22:33")}
        </div>

        <div className="mt-3 rounded border border-neutral-800 bg-black/50 px-3 py-2 font-mono text-[11px]">
          <span className="text-neutral-500">URL </span>
          <span className="text-sky-300">{brokerUrl(draft)}</span>
          <div className="mt-1">
            <span className="text-neutral-500">LINK </span>
            <span className={STATUS_STYLE[mqtt.status]}>{mqtt.status.toUpperCase()}</span>
            {mqtt.error && <span className="ml-2 text-red-400">— {mqtt.error}</span>}
          </div>
        </div>

        <div className="mt-4 flex items-center justify-between gap-2">
          <div className="flex gap-2">
            <button
              type="button"
              onClick={() => void mqtt.connect(draft)}
              className="rounded border border-sky-700 bg-sky-900/50 px-3 py-1.5 text-[11px] font-semibold text-sky-200 hover:bg-sky-800/50"
            >
              {mqtt.connected ? "Reconnect" : "Connect"}
            </button>
            {mqtt.connected && (
              <button
                type="button"
                onClick={() => mqtt.disconnect()}
                className="rounded border border-red-800 bg-red-950/50 px-3 py-1.5 text-[11px] font-semibold text-red-300 hover:bg-red-900/50"
              >
                Disconnect
              </button>
            )}
          </div>
          <button
            type="button"
            onClick={save}
            className="rounded border border-green-700 bg-green-900/40 px-4 py-1.5 text-[11px] font-bold uppercase tracking-wider text-green-200 hover:bg-green-800/40"
          >
            Save &amp; Connect
          </button>
        </div>
      </div>
    </div>
  );
}
