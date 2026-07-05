"use client";

import { useEffect, useRef, useState } from "react";
import type { PowerState } from "@/hooks/useLockState";

interface StatusLedsProps {
  powerState: PowerState;
  lowBattery: boolean;
  alarm: boolean;
  bleFlashing: boolean;
  provisioned: boolean;
  confirmPulse: number;
}

/** Power/Status LED, BLE provisioning indicator, and the Wi-Fi radio indicator. */
export default function StatusLeds({
  powerState,
  lowBattery,
  alarm,
  bleFlashing,
  provisioned,
  confirmPulse,
}: StatusLedsProps) {
  const [confirming, setConfirming] = useState(false);
  const prevPulse = useRef(confirmPulse);

  useEffect(() => {
    if (confirmPulse === prevPulse.current) return;
    prevPulse.current = confirmPulse;
    setConfirming(true);
    // 3 blinks × 0.34s ≈ 1.02s of green confirmation before returning to normal.
    const timer = setTimeout(() => setConfirming(false), 1050);
    return () => clearTimeout(timer);
  }, [confirmPulse]);

  const awake = powerState === "WAKING";
  const red = alarm || lowBattery;

  const ledClass = confirming
    ? "bg-green-400 shadow-[0_0_10px_2px_rgba(74,222,128,0.9)] animate-confirm"
    : red
      ? `bg-red-500 shadow-[0_0_10px_2px_rgba(239,68,68,0.8)] ${alarm ? "animate-alarm" : ""}`
      : awake
        ? "bg-green-400 shadow-[0_0_10px_2px_rgba(74,222,128,0.8)]"
        : "bg-neutral-700";

  const ledLabel = confirming
    ? "REGISTERED"
    : red
      ? lowBattery
        ? "BATT LOW"
        : "ALERT"
      : awake
        ? "ACTIVE"
        : "STANDBY";

  return (
    <div className="flex items-center justify-between px-6 py-2">
      <div className="flex items-center gap-2">
        <span
          key={confirmPulse}
          className={`h-2.5 w-2.5 rounded-full transition-colors ${ledClass}`}
        />
        <span className="text-[10px] uppercase tracking-widest text-neutral-500">{ledLabel}</span>
      </div>

      {/* BLE provisioning indicator */}
      <div className="flex items-center gap-1.5">
        <svg
          viewBox="0 0 24 24"
          className={`h-4 w-4 ${
            bleFlashing
              ? "animate-ble text-blue-400"
              : provisioned
                ? "text-blue-500/70"
                : "text-neutral-700"
          }`}
          fill="currentColor"
          aria-label="Bluetooth provisioning"
        >
          <path d="M12 2 17 7l-3.5 3.5L17 14l-5 5v-7.2L8.5 15l-1.4-1.4L11 10 7.1 6.1 8.5 4.7 12 8.2V2zm1.5 3.6L12 4.1v3l1.5-1.5zm0 8.3L12 15.4v3l1.5-1.5-1.5-1.5z" />
        </svg>
        <span className="text-[10px] uppercase tracking-widest text-neutral-500">
          {bleFlashing ? "BLE ADV" : provisioned ? "BLE PAIRED" : "BLE OFF"}
        </span>
      </div>

      {/* Wi-Fi radio indicator */}
      <div className="flex items-center gap-1.5">
        <svg
          viewBox="0 0 24 24"
          className={`h-4 w-4 ${awake ? "animate-flash text-sky-400" : "text-neutral-700"}`}
          fill="currentColor"
          aria-label="Wi-Fi radio"
        >
          <path d="M12 18.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM12 14c-2 0-3.8.78-5.15 2.05l1.77 1.77A4.98 4.98 0 0 1 12 16.5c1.3 0 2.5.5 3.38 1.32l1.77-1.77A7.47 7.47 0 0 0 12 14zm0-4.5c-3.24 0-6.18 1.28-8.35 3.36l1.77 1.77A9.44 9.44 0 0 1 12 12c2.55 0 4.87.99 6.58 2.63l1.77-1.77A11.94 11.94 0 0 0 12 9.5zM12 5C7.86 5 4.1 6.63 1.32 9.28l1.77 1.77A13.9 13.9 0 0 1 12 7.5c3.42 0 6.55 1.23 8.91 3.55l1.77-1.77A16.4 16.4 0 0 0 12 5z" />
        </svg>
        <span className="text-[10px] uppercase tracking-widest text-neutral-500">
          {awake ? "RADIO TX" : "RADIO OFF"}
        </span>
      </div>
    </div>
  );
}
