/** Tiny WebAudio synth for tactile feedback — no audio assets required. */

let ctx: AudioContext | null = null;

function getContext(): AudioContext | null {
  if (typeof window === "undefined") return null;
  ctx ??= new AudioContext();
  if (ctx.state === "suspended") void ctx.resume();
  return ctx;
}

function tone(freq: number, durationSec: number, type: OscillatorType, gain = 0.08): void {
  const ac = getContext();
  if (!ac) return;
  try {
    const osc = ac.createOscillator();
    const g = ac.createGain();
    osc.type = type;
    osc.frequency.value = freq;
    g.gain.setValueAtTime(gain, ac.currentTime);
    g.gain.exponentialRampToValueAtTime(0.0005, ac.currentTime + durationSec);
    osc.connect(g).connect(ac.destination);
    osc.start();
    osc.stop(ac.currentTime + durationSec);
  } catch {
    // Audio is best-effort; never let it break the simulator.
  }
}

/** Sharp keypad click. */
export function keyClick(): void {
  tone(2400, 0.045, "square", 0.06);
}

/** Rising two-note chirp for granted access. */
export function accessGranted(): void {
  tone(880, 0.09, "sine", 0.1);
  setTimeout(() => tone(1320, 0.14, "sine", 0.1), 90);
}

/** Low buzz for denied / expired access. */
export function accessDenied(): void {
  tone(180, 0.25, "sawtooth", 0.09);
}

/** Clutch motor whirr during bolt actuation. */
export function motorWhirr(): void {
  tone(95, 0.5, "sawtooth", 0.05);
  tone(140, 0.5, "triangle", 0.04);
}
