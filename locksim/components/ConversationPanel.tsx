"use client";

import { useEffect, useRef } from "react";
import type { ConversationMessage } from "@/lib/broker";
import type { MqttStatus } from "@/hooks/useMqttLink";

interface ConversationPanelProps {
  messages: ConversationMessage[];
  mqttStatus: MqttStatus;
  brokerUrl: string;
  registering: boolean;
  paired: boolean;
  /** Mode C: swap the announce button to OZLOCK direct enrollment. */
  ozlockMode: boolean;
  onRegister: () => void;
  onEnrollOzlock: () => void;
  onOpenSettings: () => void;
  onClear: () => void;
}

const STATUS_DOT: Record<MqttStatus, string> = {
  offline: "bg-neutral-500",
  connecting: "bg-amber-400 animate-pulse",
  connected: "bg-green-400 shadow-[0_0_8px_2px_rgba(74,222,128,0.7)]",
  error: "bg-red-500",
};

/**
 * OZKEYSERV/ onboarding conversation — the live lock ⇄ server transcript over
 * MQTT. Lock → server messages sit on the right, server → lock on the left, so
 * an operator can watch discovery, room assignment, heartbeats and command
 * frames as they happen.
 */
export default function ConversationPanel({
  messages,
  mqttStatus,
  brokerUrl,
  registering,
  paired,
  ozlockMode,
  onRegister,
  onEnrollOzlock,
  onOpenSettings,
  onClear,
}: ConversationPanelProps) {
  const bodyRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const el = bodyRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [messages]);

  const connected = mqttStatus === "connected";

  return (
    <div className="flex flex-col rounded-lg border border-blue-900/50 bg-neutral-900/70">
      <div className="flex items-center justify-between gap-2 border-b border-neutral-800 px-3 py-2">
        <div className="flex items-center gap-2">
          <span className={`h-2.5 w-2.5 rounded-full ${STATUS_DOT[mqttStatus]}`} />
          <span className="text-[10px] font-bold uppercase tracking-widest text-blue-400">
            OZKEYSERV/ Onboarding Handshake
          </span>
        </div>
        <button
          type="button"
          onClick={onOpenSettings}
          className="rounded border border-neutral-700 bg-neutral-800 px-2 py-1 text-[10px] text-neutral-300 hover:bg-neutral-700"
        >
          ⚙ System Settings
        </button>
      </div>

      <div className="flex flex-wrap items-center gap-2 border-b border-neutral-800 px-3 py-2">
        <button
          type="button"
          onClick={ozlockMode ? onEnrollOzlock : onRegister}
          disabled={!connected}
          className={`rounded border px-3 py-1.5 text-[11px] font-bold uppercase tracking-wider disabled:cursor-not-allowed disabled:opacity-40 ${
            ozlockMode
              ? "border-teal-700 bg-teal-900/40 text-teal-200 hover:bg-teal-800/40"
              : "border-blue-700 bg-blue-900/40 text-blue-200 hover:bg-blue-800/40"
          }`}
        >
          {ozlockMode ? "⇈ Enroll with OZLOCK" : "⇈ Register Doorlock"}
        </button>
        <span className="text-[10px] text-neutral-500">
          {!connected
            ? "link offline — open System Settings to connect"
            : registering
              ? ozlockMode
                ? "awaiting OZLOCK enrollment ack…"
                : "awaiting room assignment from cockpit…"
              : paired
                ? "paired — heartbeats flush queued commands"
                : ozlockMode
                  ? "register this device_id in the OZLOCK app, then enroll"
                  : "announce this lock to the broker"}
        </span>
        <button
          type="button"
          onClick={onClear}
          className="ml-auto rounded border border-neutral-700 bg-neutral-800 px-2 py-1 text-[10px] text-neutral-400 hover:bg-neutral-700"
        >
          Clear
        </button>
      </div>

      <div className="px-3 py-1 font-mono text-[9px] text-neutral-600">{brokerUrl || "ws://—"}</div>

      <div ref={bodyRef} className="h-[22rem] space-y-2 overflow-y-auto p-3">
        {messages.length === 0 && (
          <div className="pt-8 text-center text-[11px] text-neutral-600">
            -- no traffic yet · Register the doorlock to begin onboarding --
          </div>
        )}
        {messages.map((m) => {
          const up = m.dir === "up";
          return (
            <div key={m.id} className={`flex ${up ? "justify-end" : "justify-start"}`}>
              <div
                className={`max-w-[85%] rounded-lg border px-2.5 py-1.5 ${
                  m.error
                    ? "border-red-900/60 bg-red-950/30"
                    : up
                      ? "border-blue-900/60 bg-blue-950/40"
                      : "border-neutral-700 bg-neutral-800/60"
                }`}
              >
                <div className="flex items-center gap-2 text-[9px] text-neutral-500">
                  <span>{up ? "LOCK → SERVER" : "SERVER → LOCK"}</span>
                  <span className="font-mono">{m.topic}</span>
                  <span className="ml-auto">{m.time}</span>
                </div>
                <div
                  className={`mt-0.5 text-[11px] ${
                    m.error ? "text-red-300" : up ? "text-blue-200" : "text-neutral-200"
                  }`}
                >
                  {m.summary}
                </div>
                <div className="mt-0.5 break-all font-mono text-[9px] text-neutral-500">{m.raw}</div>
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
