"use client";

import type { StoredCredential } from "@/lib/credentials";
import { checkWindowMcu } from "@/lib/credentials";

interface PeripheralControlsProps {
  onTapRfid: (cred?: StoredCredential) => void;
  onScanFingerprint: () => void;
  onLowBattery: () => void;
  /** ozkey-22 R1 — physical factory-reset gesture on the lock body (MCU-wired). */
  onFactoryReset: () => void;
  /** PROPOSED DP 60 — keypad pairing gesture, DL MCU owns the keypad. */
  onPairingGesture: () => void;
  /** DP 53 doorbell — REAL, confirmed Tuya DP. The gesture that ships. */
  onDoorbell: () => void;
  lowBattery: boolean;
  credentials: StoredCredential[];
  /** ozkey-21 — the MCU's clock, null when the module never served time. */
  mcuUnix: number | null;
}

const buttonBase =
  "w-full rounded-lg border px-3 py-2 text-left text-[11px] font-medium tracking-wide transition-all duration-75 select-none active:translate-y-[1px]";

/** Peripheral event triggers: RFID reader, fingerprint sensor, battery monitor. */
export default function PeripheralControls({
  onTapRfid,
  onScanFingerprint,
  onLowBattery,
  onFactoryReset,
  onPairingGesture,
  onDoorbell,
  lowBattery,
  credentials,
  mcuUnix,
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
        const valid = checkWindowMcu(card, mcuUnix) === "VALID";
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

      {/*
        DP 53 doorbell — the ONLY at-the-door pairing gesture that works on
        shipping hardware. `status: confirmed` in the real Tuya catalogue, so
        unlike the two below this is not our invention. doorlock-1.84 opens a
        60 s BLE window on it (~15 s of which is advertising-to-discovery
        latency on a real phone), with a 2-minute cooldown after it closes so
        ringing repeatedly cannot hold the radio on (ozkey-36: on a battery
        lock the radio IS the power budget).
      */}
      <button
        type="button"
        onPointerDown={onDoorbell}
        title="DP 53 — a REAL Tuya DP (status: confirmed). Opens the BLE pairing window on doorlock-1.84+"
        className={`${buttonBase} border-emerald-800/60 bg-emerald-950/40 text-emerald-300 active:bg-emerald-900/40`}
      >
        🔔 Ring Doorbell <span className="opacity-60">(DP 53, real)</span>
      </button>

      {/*
        PROPOSED DP 60. On production the keypad belongs to the DL MCU, so this
        is the only way a member at the door can ask the lock to advertise —
        our board has no touch panel in the real product. Marked PROPOSED
        because the DP number is a placeholder pending manufacturer allocation;
        no shipping DL MCU sends this.
      */}
      <button
        type="button"
        onPointerDown={onPairingGesture}
        title="PROPOSED DP 60 — not a real Tuya DP yet, pending manufacturer allocation"
        className={`${buttonBase} border-violet-800/60 bg-violet-950/40 text-violet-300 active:bg-violet-900/40`}
      >
        ⌨ Keypad Pairing Gesture <span className="opacity-60">(proposed DP 60)</span>
      </button>

      {/*
        ozkey-22 R1. This button is the reset gesture on the LOCK BODY, which is
        physically wired to the MCU — not to the ESP32. So the MCU is what
        notices, and it must tell the module over 0x34 0x0A.

        Styled as the destructive action it is, and sat apart from the event
        triggers above: everything else here simulates an input, this one asks
        the module to erase itself.
      */}
      <button
        type="button"
        onPointerDown={onFactoryReset}
        title="Sends 0x34 0x0A — Tuya MCU-initiated module factory reset"
        className={`${buttonBase} mt-1 border-red-800 bg-red-950/50 text-red-300 active:bg-red-900/50`}
      >
        ⏻ Hardware Factory Reset (MCU → module)
      </button>
    </div>
  );
}
