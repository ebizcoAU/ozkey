"use client";

import type { PowerState } from "@/hooks/useLockState";

interface StatusLedsProps {
  powerState: PowerState;
  lowBattery: boolean;
  alarm: boolean;
}

/** Power/Status LED (green/red/off) and the Wi-Fi radio indicator. */
export default function StatusLeds({ powerState, lowBattery, alarm }: StatusLedsProps) {
  const awake = powerState === "WAKING";
  const red = alarm || lowBattery;
  const ledClass = red
    ? `bg-red-500 shadow-[0_0_10px_2px_rgba(239,68,68,0.8)] ${alarm ? "animate-alarm" : ""}`
    : awake
      ? "bg-green-400 shadow-[0_0_10px_2px_rgba(74,222,128,0.8)]"
      : "bg-neutral-700";

  return (
    <div className="flex items-center justify-between px-6 py-2">
      <div className="flex items-center gap-2">
        <span className={`h-2.5 w-2.5 rounded-full transition-colors ${ledClass}`} />
        <span className="text-[10px] uppercase tracking-widest text-neutral-500">
          {red ? (lowBattery ? "BATT LOW" : "ALERT") : awake ? "ACTIVE" : "STANDBY"}
        </span>
      </div>

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
