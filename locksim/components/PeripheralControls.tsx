"use client";

import type { StoredCredential } from "@/lib/credentials";
import { checkWindow } from "@/lib/credentials";

interface PeripheralControlsProps {
  onTapRfid: (cred?: StoredCredential) => void;
  onScanFingerprint: () => void;
  onLowBattery: () => void;
  lowBattery: boolean;
  credentials: StoredCredential[];
  virtualNowMs: number;
}

const buttonBase =
  "w-full rounded-lg border px-3 py-2 text-left text-[11px] font-medium tracking-wide transition-all duration-75 select-none active:translate-y-[1px]";

/** Peripheral event triggers: RFID reader, fingerprint sensor, battery monitor. */
export default function PeripheralControls({
  onTapRfid,
  onScanFingerprint,
  onLowBattery,
  lowBattery,
  credentials,
  virtualNowMs,
}: PeripheralControlsProps) {
  const tempCards = credentials.filter((c) => c.kind === "RFID");

  return (
    <div className="mx-5 flex flex-col gap-2 pb-3">
      <button
        type="button"
        onPointerDown={() => onTapRfid()}
        className={`${buttonBase} border-sky-800/60 bg-sky-950/40 text-sky-300 active:bg-sky-900/40`}
      >
        ▣ Tap RFID Card (Mifare)
      </button>

      {tempCards.map((card) => {
        const valid = checkWindow(card, virtualNowMs) === "VALID";
        return (
          <button
            key={`rfid-${card.slot}`}
            type="button"
            onPointerDown={() => onTapRfid(card)}
            className={`${buttonBase} ${
              valid
                ? "border-teal-800/60 bg-teal-950/40 text-teal-300 active:bg-teal-900/40"
                : "border-red-900/60 bg-red-950/30 text-red-400/80 active:bg-red-900/30"
            }`}
          >
            ▣ Tap Temp Card — Slot {card.slot} ({card.value}) {valid ? "" : "⚠ OUT OF WINDOW"}
          </button>
        );
      })}

      <button
        type="button"
        onPointerDown={onScanFingerprint}
        className={`${buttonBase} border-violet-800/60 bg-violet-950/40 text-violet-300 active:bg-violet-900/40`}
      >
        ◉ Scan Fingerprint
      </button>

      <button
        type="button"
        onPointerDown={onLowBattery}
        className={`${buttonBase} ${
          lowBattery
            ? "border-red-700 bg-red-950/60 text-red-300 active:bg-red-900/50"
            : "border-amber-800/60 bg-amber-950/40 text-amber-300 active:bg-amber-900/40"
        }`}
      >
        ⚠ {lowBattery ? "Clear Low Battery Event" : "Low Battery Event Trigger"}
      </button>
    </div>
  );
}
