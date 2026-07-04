"use client";

import type { LockState, PowerState } from "@/hooks/useLockState";

interface LockDisplayProps {
  powerState: PowerState;
  lockState: LockState;
  pinBuffer: string;
  countdown: number;
  motorActive: boolean;
  alarm: boolean;
  lastEvent: string;
  virtualNowMs: number;
}

function fmtCountdown(sec: number): string {
  return `${String(Math.floor(sec / 60)).padStart(2, "0")}:${String(sec % 60).padStart(2, "0")}`;
}

/** Main digital readout: lock/power status, deadbolt, clocks and event ticker. */
export default function LockDisplay({
  powerState,
  lockState,
  pinBuffer,
  countdown,
  motorActive,
  alarm,
  lastEvent,
  virtualNowMs,
}: LockDisplayProps) {
  const sleeping = powerState === "SLEEPING";
  const primary = sleeping ? "SLEEPING (7µA)" : lockState;
  const primaryColor = alarm
    ? "text-red-400 animate-alarm"
    : sleeping
      ? "text-neutral-500"
      : lockState === "UNLOCKED"
        ? "text-emerald-400"
        : "text-amber-300";

  return (
    <div className="mx-5 rounded-2xl border border-neutral-800 bg-black/80 px-4 py-3 font-mono">
      {/* System clock + heartbeat countdown */}
      <div className="flex items-center justify-between text-[10px] text-neutral-500">
        <span suppressHydrationWarning>
          SYS {new Date(virtualNowMs).toLocaleTimeString("en-GB")}
        </span>
        <span className={countdown <= 5 ? "text-sky-400" : ""}>
          HEARTBEAT T-{fmtCountdown(countdown)}
        </span>
      </div>

      {/* Primary status readout */}
      <div className="flex items-center justify-between py-2">
        <div>
          <div className={`text-2xl font-bold tracking-widest ${primaryColor}`}>{primary}</div>
          <div className="mt-1 text-[10px] tracking-wider text-neutral-500">
            {sleeping ? "DEEP SLEEP • RADIO OFF • GPIO WAKE_INT: LOW" : "WAKING (45mA) • GPIO WAKE_INT: HIGH"}
          </div>
        </div>
        {/* Clutch motor */}
        <svg
          viewBox="0 0 24 24"
          className={`h-8 w-8 ${motorActive ? "animate-motor text-emerald-400" : "text-neutral-700"}`}
          fill="currentColor"
          aria-label="Clutch motor"
        >
          <path d="M19.4 13a7.5 7.5 0 0 0 0-2l2-1.55-2-3.46-2.36.95a7.6 7.6 0 0 0-1.73-1L15 3.5h-4l-.3 2.44c-.63.25-1.2.6-1.74 1L6.6 6l-2 3.46L6.6 11a7.5 7.5 0 0 0 0 2l-2 1.55 2 3.46 2.36-.95c.53.4 1.1.74 1.73 1L11 20.5h4l.3-2.44a7.6 7.6 0 0 0 1.74-1l2.36.95 2-3.46zM13 15a3 3 0 1 1 0-6 3 3 0 0 1 0 6z" />
        </svg>
      </div>

      {/* Deadbolt animation */}
      <div className="relative h-3 overflow-hidden rounded bg-neutral-900 ring-1 ring-neutral-800">
        <div
          className={`absolute top-0 h-full w-2/3 rounded transition-transform duration-700 ease-in-out ${
            lockState === "LOCKED" ? "translate-x-0 bg-amber-500/80" : "-translate-x-[85%] bg-emerald-500/80"
          }`}
        />
      </div>

      {/* PIN entry dots */}
      <div className="flex items-center gap-2 pt-3">
        <span className="text-[10px] text-neutral-500">PIN</span>
        {Array.from({ length: 6 }).map((_, i) => (
          <span
            key={i}
            className={`h-2 w-2 rounded-full ${i < pinBuffer.length ? "bg-emerald-400" : "bg-neutral-800"}`}
          />
        ))}
        <span className="ml-auto text-[9px] text-neutral-600">#=SUBMIT *=CLR</span>
      </div>

      {/* Event ticker */}
      <div className="mt-2 truncate border-t border-neutral-800 pt-1.5 text-[10px] text-sky-500/80">
        &gt; {lastEvent}
      </div>
    </div>
  );
}
