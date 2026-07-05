"use client";

import type { NetworkProvisioning } from "@/lib/provisioning";

interface BleProvisioningProps {
  bleMode: boolean;
  onToggle: (on: boolean) => void;
  onBroadcast: () => void;
  deviceMac: string;
  provisioning: NetworkProvisioning | null;
}

/**
 * Matter-style onboarding controls inside the lock face: the BLE Provisioning
 * Mode switch and the hardware-MAC broadcast that announces the device to the
 * OZKEYSERV/ broker over the Web Serial bridge.
 */
export default function BleProvisioning({
  bleMode,
  onToggle,
  onBroadcast,
  deviceMac,
  provisioning,
}: BleProvisioningProps) {
  return (
    <div className="mx-5 mb-3 rounded-lg border border-neutral-800 bg-neutral-900/60 p-3">
      <div className="flex items-center justify-between">
        <div>
          <div className="text-[11px] font-medium text-neutral-300">BLE Provisioning Mode</div>
          <div className="font-mono text-[9px] text-neutral-500">MAC {deviceMac}</div>
        </div>
        <button
          type="button"
          role="switch"
          aria-checked={bleMode}
          onClick={() => onToggle(!bleMode)}
          className={`relative h-6 w-14 rounded-full border transition-colors ${
            bleMode ? "border-blue-500 bg-blue-900/60" : "border-neutral-700 bg-neutral-800"
          }`}
        >
          <span
            className={`absolute top-0.5 flex h-[18px] w-[18px] items-center justify-center rounded-full text-[9px] transition-all duration-200 ${
              bleMode ? "left-[34px] bg-blue-400 text-black animate-ble" : "left-0.5 bg-neutral-500 text-black"
            }`}
          >
            ᛒ
          </span>
        </button>
      </div>

      <button
        type="button"
        onClick={onBroadcast}
        disabled={!bleMode}
        className="mt-3 w-full rounded-lg border border-blue-800/60 bg-blue-950/40 px-3 py-2 text-[11px] font-medium tracking-wide text-blue-300 transition-all duration-75 select-none hover:bg-blue-900/40 active:translate-y-[1px] disabled:cursor-not-allowed disabled:opacity-40"
      >
        ⇈ Broadcast Hardware MAC ID → OZKEYSERV/
      </button>

      <div className="mt-2 border-t border-neutral-800 pt-2 text-[9px] leading-relaxed">
        {provisioning ? (
          <div className="text-emerald-400/90">
            PAIRED · Room {provisioning.assigned_room_no} · {provisioning.server_ip}
            <span className="ml-1 text-neutral-500">· token {provisioning.mac_token}</span>
          </div>
        ) : bleMode ? (
          <div className="animate-ble text-blue-400">
            UNPROVISIONED · advertising on BLE · listening on hotel/rooms/+/lock/command
          </div>
        ) : (
          <div className="text-neutral-600">Not provisioned — toggle BLE mode to begin onboarding.</div>
        )}
      </div>
    </div>
  );
}
