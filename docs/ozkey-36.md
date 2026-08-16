# ozkey-36 — Battery reality, and what it decides about the product line

> **Architecture record, 2026-08-16.** Consumers: **everyone** — operator, PM,
> architect, firmware, NEXUS, ftpos.
>
> This exists because the reasoning lived in a conversation and the conclusion
> contradicts a claim currently in `ozlock_v3.6.pdf`. It is arithmetic over
> stated assumptions, not measurement — **every figure here is an estimate**,
> and §7 says exactly which two numbers would turn it into fact.
>
> 🔴 **The headline: "2–7 year battery life" is only true at about 5 door-opens
> per day.** At 20/day the same lock lasts about a year. The usage assumption
> is doing all the work and it is stated nowhere.

---

## 1. What was actually asked

Whether the ESP32-C6 is the wrong chip for a battery lock, and whether the
Wi-Fi transport is viable. Short answers: **the chip is fine, and Wi-Fi is
viable only if you accept hours of latency.**

## 2. The model

4×AA alkaline, 2500 mAh. ESP32-C6 deep sleep **8.14 µA** (datasheet, and
consistent with independent measurements of 7–14 µA). Wi-Fi wake assumed 2 s at
~100 mA. DL MCU assumed 50 µA standby, 300 mA × 1 s motor per open, 40 mA × 8 s
active per entry.

**All of it is estimated.** The two figures that matter most — the real wake
duration and the DL MCU's real draw — are the two nobody has measured.

## 3. 🔴 Usage is the hidden variable in every claim in this market

6-hour wake timer, deep sleep, whole lock including the DL MCU:

| Opens/day | Life |
|---|---|
| 2 | 3.3 years |
| 5 | **2.5 years** |
| 10 | 1.8 years |
| **20** | **1.1 years** |
| 40 | 0.6 years |

Competitors quoting "2 years" are not lying — they are quoting **~5 opens/day**
and not saying so. A busy household or a rental at 20/day gets about a year.

**Any battery figure we publish must state the opens/day it assumes.** Without
it the number is unfalsifiable, and the first heavy-use customer makes us look
either careless or dishonest.

## 4. 🔴 The DL MCU is 70% of the budget and we do not control it

At 20 opens/day with the comms module already tuned to its best realistic
configuration (6-hour timer, 2 s wake, deep sleep):

| | mAh/day | Share |
|---|---|---|
| ESP32-C6 (ours) | 1.53 | 25% |
| DL MCU + motor (theirs) | 4.64 | **75%** |

Everything firmware can do lives inside that 25%. Past ~100 µA of MCU standby,
the MCU asleep costs more than our entire daily budget.

**This is the ceiling on the whole exercise**, and it is why the options in §5
cluster so tightly.

## 5. The options, all at 20 opens/day

| Option | Life | Notes |
|---|---|---|
| Wi-Fi, 15-min wake | 0.6 yr | responsive, halves the battery |
| Wi-Fi, 1-hour wake | 0.9 yr | |
| Wi-Fi, 6-hour wake | 1.1 yr | ~96% of the benefit of never self-waking |
| **Thread SED 5 s + bridge** | **1.2 yr** | *and* sub-second delivery |
| ESP32 drives the whole lock | 1.9 yr | the only option that touches the MCU's 75% |

The first four are within 25% of each other. That is not a coincidence — they
all optimise the same quarter of the budget.

### 5.1 The precise reason Wi-Fi loses, and it is not battery

Wi-Fi at 6 hours is within 10% of Thread. **Wi-Fi's problem is that
responsiveness and battery are the same dial.** Make it responsive (15 min) and
you spend half the battery for latency that is still poor. Thread does not have
that coupling: a poll is milliseconds of radio, so 5-second delivery costs less
than Wi-Fi's 6-hour delivery.

So Wi-Fi mode should stop being the default for a **battery** lock. It remains
correct for mains-powered locks, and for long-term rental where credentials are
programmed days ahead and hours of latency genuinely do not matter.

🟡 **The exception that still needs a decision: remote revoke.** Under Wi-Fi at
6 hours, a revoked credential stays live for up to 6 hours. Touch-to-wake does
**not** rescue this — the MCU validates a PIN locally in 1–3 s while a Wi-Fi
connect takes 2–8 s, so the door opens *before* the revoke is pulled. BLE at the
door is the only instant revoke, and it needs someone on site.

### 5.2 🔴 The awkward case: ozkey-04 market A

**Residential, no hub, Wi-Fi, battery** is the one configuration with no good
answer — 1.1 years at best, or half that if you want it responsive. It either
gains a bridge (becoming market B), accepts an annual battery change, or is
mains-adjacent. This should be an explicit product decision rather than a
surprise in the field.

## 6. What this decides

**Piggyback + Thread + bridge is the volume product.** The co-processor PCB
riding an existing Tuya DL MCU means *any* Tuya lock manufacturer's product can
become an OZLOCK. That reach is the commercial argument, and it is a stronger
one than any battery figure. In OZPMS — thousands of doors in powered buildings
— a bridge per floor costs nothing.

**Own-PCB is premium, and not justified on battery alone.** 1.9 vs 1.1 years is
real, but **8×AA gets the piggyback product to 2.2 years for a few dollars** and
no engineering. The genuine argument for own-PCB is sovereignty: today the
Sovereign Edge story stops at a Tuya MCU whose firmware, behaviour and power we
cannot see or verify. It also means owning motor drive, safety, keypad, reader
and certification — and the C6 has no capacitive-touch peripheral, so an
external controller is needed regardless.

**Wi-Fi is demoted, not deleted.** Mains-powered and latency-tolerant installs.

### 6.1 Firmware consequences

1. **Deep sleep instead of light sleep.** `ozdoorlock_core.h` calls
   `esp_light_sleep_start()`. At 6-hour intervals light sleep is **84% of our
   own budget** — the slow-wake design does not pay off without this. The two
   changes are one change.
2. **`heartbeat_s` needs an hours range.** Capped at 900 s today (C9 §3).
3. **Lean into the Tuya slave model.** `SRDY` is already a wake source
   (`gpio_wakeup_enable(SRDY_PIN, …)`), so the MCU can drive our wakes exactly
   as Tuya intends. Keep a slow housekeeping tick — liveness, battery level and
   **clock sync**, which temporary-credential expiry depends on — but stop
   self-waking every few minutes.

## 7. 🔴 The two measurements this all rests on

Neither has been taken. Both need the power monitor that does not exist on this
bench (`ozkey-35.md` §5 row 13, still marked **not measured**).

1. **Real wake duration, Wi-Fi.** Assumed 2 s. If it is 5 s, every figure above
   drops by roughly half. It is ~95% of the comms-side budget.
2. **The DL MCU's real consumption** — standby, motor per operation, active per
   entry. It is 75% of the total and it is a supplier question, the same gap as
   `ozkey-27` Q2.

A third, if own-PCB is ever considered seriously: whether a deep-sleep wake
(which loses RAM and the Wi-Fi association) stays under **~9 s**. Past that,
deep sleep is worse than the light sleep we do today.

## 8. What to correct elsewhere

- `ozlock_v3.6.pdf` §3.1 marks **C9 MEASURED** with "45 µA → 6.3 years". No
  power measurement has been taken, and that figure is comms-module-only — it
  omits the DL MCU, which is 75% of the load. `ozkey-35.md` §12 already flags
  the register; this adds the reason the number is optimistic even on its own
  terms.
- Any published battery claim needs its **opens/day** stated (§3).
- The Thread argument should be made on **latency**, not battery (§5.1).
  Claiming Thread as the battery fix overstates a 10% difference.
