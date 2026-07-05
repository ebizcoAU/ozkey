"use client";

import type { CredentialKind, StoredCredential } from "@/lib/credentials";
import { checkWindow, type TemporalCheck } from "@/lib/credentials";

interface DeviceRegistryProps {
  credentials: StoredCredential[];
  virtualNowMs: number;
  onRevoke: (kind: CredentialKind, slot: number) => void;
}

function fmtDateTime(sec: number): string {
  return new Date(sec * 1000).toLocaleString("en-GB", {
    dateStyle: "medium",
    timeStyle: "short",
  });
}

const STATUS_LABEL: Record<TemporalCheck, string> = {
  VALID: "ACTIVE",
  EXPIRED: "EXPIRED",
  NOT_YET_ACTIVE: "PENDING",
};

/**
 * Sovereign Device Registry DB — compliance viewport over the LocalStorage slot
 * table. Rows are colour-coded by live temporal status against the Master Clock:
 * active = low-saturation green border, inactive/expired = faded amber backdrop.
 */
export default function DeviceRegistry({ credentials, virtualNowMs, onRevoke }: DeviceRegistryProps) {
  const activeCount = credentials.filter((c) => checkWindow(c, virtualNowMs) === "VALID").length;

  return (
    <div className="w-full max-w-3xl rounded-lg border border-neutral-800 bg-neutral-900/70 p-3">
      <div className="mb-2 flex items-center justify-between">
        <span className="text-[10px] font-bold uppercase tracking-widest text-emerald-400">
          ▤ Sovereign Device Registry DB
        </span>
        <span className="text-[9px] text-neutral-500">
          {credentials.length} SLOT{credentials.length === 1 ? "" : "S"} · {activeCount} ACTIVE ·
          LIVE LOCALSTORAGE
        </span>
      </div>

      <div className="overflow-x-auto">
        <table className="w-full min-w-[720px] border-separate border-spacing-y-1 font-mono text-[11px]">
          <thead>
            <tr className="text-left text-[9px] uppercase tracking-wider text-neutral-500">
              <th className="px-2 py-1 font-normal">Slot ID</th>
              <th className="px-2 py-1 font-normal">Credential Type</th>
              <th className="px-2 py-1 font-normal">Raw String Value / Hash</th>
              <th className="px-2 py-1 font-normal">Valid From</th>
              <th className="px-2 py-1 font-normal">Valid To</th>
              <th className="px-2 py-1 font-normal">System Reg. Token</th>
              <th className="px-2 py-1 font-normal text-right">Action</th>
            </tr>
          </thead>
          <tbody>
            {credentials.length === 0 && (
              <tr>
                <td colSpan={7} className="px-2 py-4 text-center text-neutral-600">
                  -- registry empty · provision a slot via a DPID 21 / 23 frame --
                </td>
              </tr>
            )}
            {credentials.map((c) => {
              const status = checkWindow(c, virtualNowMs);
              const active = status === "VALID";
              // Active: low-saturation green border. Inactive/expired: faded amber backdrop.
              const rowStyle = active
                ? "border border-green-800/50 bg-green-950/10"
                : "border border-amber-900/40 bg-amber-950/25";
              return (
                <tr key={`${c.kind}-${c.slot}`} className={`${rowStyle} text-neutral-200`}>
                  <td className="rounded-l px-2 py-1.5">{c.slot}</td>
                  <td className="px-2 py-1.5">
                    <span
                      className={`rounded px-1.5 py-0.5 text-[9px] ${
                        c.kind === "PIN"
                          ? "bg-sky-950/60 text-sky-300"
                          : "bg-teal-950/60 text-teal-300"
                      }`}
                    >
                      {c.kind}
                    </span>
                  </td>
                  <td className="px-2 py-1.5 text-green-400">{c.value}</td>
                  <td className="px-2 py-1.5 text-neutral-400">{fmtDateTime(c.start)}</td>
                  <td className="px-2 py-1.5 text-neutral-400">{fmtDateTime(c.end)}</td>
                  <td className="px-2 py-1.5 text-neutral-500">{c.token}</td>
                  <td className="rounded-r px-2 py-1.5 text-right">
                    <span
                      className={`mr-2 text-[9px] ${
                        active
                          ? "text-green-500"
                          : status === "EXPIRED"
                            ? "text-amber-400"
                            : "text-amber-300"
                      }`}
                    >
                      {STATUS_LABEL[status]}
                    </span>
                    <button
                      type="button"
                      onClick={() => onRevoke(c.kind, c.slot)}
                      title={`Compile & fire DPID ${c.kind === "PIN" ? 22 : 24} delete frame`}
                      className="rounded border border-red-800/70 bg-red-950/40 px-2 py-0.5 text-[10px] font-semibold text-red-300 transition-colors hover:bg-red-900/50 active:translate-y-[1px]"
                    >
                      Revoke / Wipe Slot
                    </button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}
