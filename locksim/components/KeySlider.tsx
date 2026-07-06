"use client";

interface KeySliderProps {
  engaged: boolean;
  onChange: (engaged: boolean) => void;
}

/** Mechanical key override slider — physically forces the clutch open. */
export default function KeySlider({ engaged, onChange }: KeySliderProps) {
  return (
    <div className="mx-5 mb-2 flex items-center justify-between rounded-lg border border-neutral-800 bg-neutral-900/60 px-3 py-2">
      <div>
        <div className="text-[11px] font-medium text-neutral-300">Mechanical Key Override</div>
        <div className="text-[9px] text-neutral-500">
          {engaged ? "KEY TURNED — CLUTCH FORCED OPEN" : "KEYWAY EMPTY"}
        </div>
      </div>
      <button
        type="button"
        role="switch"
        aria-checked={engaged}
        onClick={() => onChange(!engaged)}
        className={`relative h-6 w-14 rounded-full border transition-colors ${
          engaged ? "border-emerald-600 bg-emerald-900/60" : "border-neutral-700 bg-neutral-800"
        }`}
      >
        <span
          className={`absolute top-0.5 flex h-[18px] w-[18px] items-center justify-center rounded-full text-[9px] transition-all duration-200 ${
            engaged ? "left-[34px] bg-emerald-400 text-black" : "left-0.5 bg-neutral-500 text-black"
          }`}
        >
          ⚿
        </span>
      </button>
    </div>
  );
}
