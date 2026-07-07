"use client";

import { useEffect, useState } from "react";
import { brokerUrl, gatewayUrl, type BrokerSettings } from "@/lib/broker";
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
  const [health, setHealth] = useState<{ ok: boolean; text: string } | null>(null);
  const [checking, setChecking] = useState(false);

  useEffect(() => {
    if (open) {
      setDraft(settings);
      setHealth(null);
    }
  }, [open, settings]);

  if (!open) return null;

  const testGateway = async () => {
    setChecking(true);
    setHealth(null);
    try {
      const res = await fetch(`${gatewayUrl(draft)}/health`, { signal: AbortSignal.timeout(4000) });
      const body = await res.json().catch(() => ({}));
      setHealth({
        ok: res.ok,
        text: res.ok
          ? `HTTP ${res.status} · db=${body.db ?? "?"} mqtt=${body.mqtt ?? "?"} uptime=${body.uptime_s ?? "?"}s`
          : `HTTP ${res.status}`,
      });
    } catch (e) {
      setHealth({ ok: false, text: (e as Error)?.message ?? "unreachable" });
    } finally {
      setChecking(false);
    }
  };

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
          <h2 className="text-sm font-bold tracking-tight text-neutral-100">System Settings</h2>
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
          The lock&apos;s live traffic (announce, handshake, heartbeat, commands) rides the MQTT
          broker over WebSocket. The gateway REST API is a separate service on the same host — used
          here only for a health check, not the credential path.
        </p>

        <div className="mb-1 text-[10px] font-bold uppercase tracking-widest text-sky-400">
          MQTT Broker (data path)
        </div>
        <div className="grid grid-cols-2 gap-3">
          {field("Host / IP", draft.host, (v) => setDraft({ ...draft, host: v }), "text", "10.1.1.21")}
          {field("WS Port", draft.wsPort, (v) => setDraft({ ...draft, wsPort: Number(v) || 0 }), "number")}
          {field("WS Path", draft.path, (v) => setDraft({ ...draft, path: v }), "text", "/mqtt")}
          {field("Device MAC", draft.mac, (v) => setDraft({ ...draft, mac: v }), "text", "AA:BB:CC:11:22:33")}
        </div>

        <div className="mb-1 mt-4 text-[10px] font-bold uppercase tracking-widest text-amber-400">
          OZKEYSERV Gateway API (control-plane)
        </div>
        <div className="grid grid-cols-2 gap-3">
          {field("API Port", draft.gatewayPort, (v) => setDraft({ ...draft, gatewayPort: Number(v) || 0 }), "number", "3200")}
          {field("API Base Path", draft.gatewayBasePath, (v) => setDraft({ ...draft, gatewayBasePath: v }), "text", "/ozkeyserv/api")}
        </div>

        <div className="mb-1 mt-4 text-[10px] font-bold uppercase tracking-widest text-emerald-400">
          Lock System (firmware)
        </div>
        <div className="grid grid-cols-2 gap-3">
          {field("Timer Wake Interval (s)", draft.heartbeatSeconds, (v) => setDraft({ ...draft, heartbeatSeconds: Number(v) || 0 }), "number", "60")}
          <div className="flex flex-col justify-end pb-1 text-[10px] leading-snug text-neutral-500">
            Deep-sleep wake to pull queued MQTT tasks. Touch/keypad also wakes the
            lock. Minimum 5 s.
          </div>
        </div>

        <div className="mt-3 rounded border border-neutral-800 bg-black/50 px-3 py-2 font-mono text-[11px]">
          <div>
            <span className="text-neutral-500">MQTT </span>
            <span className="text-sky-300">{brokerUrl(draft)}</span>
            <span className="ml-2 text-neutral-500">LINK </span>
            <span className={STATUS_STYLE[mqtt.status]}>{mqtt.status.toUpperCase()}</span>
            {mqtt.error && <span className="ml-2 text-red-400">— {mqtt.error}</span>}
          </div>
          <div className="mt-1 flex items-center gap-2">
            <span className="text-neutral-500">API </span>
            <span className="text-amber-300">{gatewayUrl(draft)}</span>
            <button
              type="button"
              onClick={() => void testGateway()}
              disabled={checking}
              className="rounded border border-amber-800 bg-amber-950/40 px-2 py-0.5 text-[10px] text-amber-200 hover:bg-amber-900/40 disabled:opacity-40"
            >
              {checking ? "…" : "Test /health"}
            </button>
            {health && (
              <span className={health.ok ? "text-green-400" : "text-red-400"}>{health.text}</span>
            )}
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
