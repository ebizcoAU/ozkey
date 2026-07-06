"use client";

const KEYS = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"];

/** 3x4 capacitive keypad with tactile press animation. */
export default function Keypad({ onKey }: { onKey: (key: string) => void }) {
  return (
    <div className="mx-5 grid grid-cols-3 gap-2.5 py-4">
      {KEYS.map((key) => (
        <button
          key={key}
          type="button"
          onPointerDown={() => onKey(key)}
          className="rounded-xl border border-neutral-700/60 bg-gradient-to-b from-neutral-800 to-neutral-900 py-3.5 text-lg font-semibold text-neutral-200 shadow-[0_2px_0_rgba(0,0,0,0.6)] transition-all duration-75 select-none active:translate-y-[2px] active:from-neutral-900 active:to-neutral-950 active:text-emerald-300 active:shadow-none"
        >
          {key}
        </button>
      ))}
    </div>
  );
}
