# ozkey-37 — `tuya_pid` behaviour, confirmed on hardware

> **F-20 deliverable, 2026-08-17.** Consumers: **ftpos**, **NEXUS**, PM.
>
> The directive asked firmware to *"confirm `tuya_pid` is reliably exposed on
> `info` and handles late arrival — already in `doorlock-1.86`"*, and stated
> **F-21: no new firmware work required.**
>
> 🔴 **Confirming it found two things that needed fixing.** Late arrival was
> handled; **survival across a reboot was not**, and the discovery path had
> never actually run. Both are closed in `doorlock-1.89`, and the whole chain
> is now verified on LockA rather than reasoned about.

---

## 1. What was verified, and how

**Observed on LockA, `doorlock-1.89`, via the broker heartbeat:**

```
fw           : doorlock-1.89
tuya_pid     : ozsimfullfeature      ← the MCU answered 0x01
profile      : ozsim-fullfeature     ← the PID selected the DP map
has_doorbell : True                  ← that profile selects DP 53
bonds        : 1                     ← the bond survived the switch
```

The lock booted on the compiled-in default (`ozkie-legacy-v0`, our invented
map), asked its MCU what it was, parsed the reply, matched the PID to a
profile, **switched**, persisted the choice, and reported it. Ask → parse →
match → switch → persist → report, end to end.

**Before this it had never run at all.** LockSim did not answer `0x01` and no
real Tuya MCU has ever been on our wire, so `doorlock-1.86` shipped the path
code-reviewed only. It is exercisable now because LockSim answers with a
fictional PID (§5).

## 2. The behaviour, precisely

| Question | Answer |
|---|---|
| Where reported | BLE `info`, Thread presence beacon, Wi-Fi MQTT heartbeat |
| When unknown | **Field omitted entirely** — not empty string, not null |
| Late arrival | Handled. `info` is rebuilt when the MCU answers (`ozRefreshInfoChar()`), so a client that reads `info` seconds after connect gets the current value |
| Across a reboot | Persisted since 1.88 (NVS `mpid`/`mver`), restored before the first heartbeat |
| Re-confirmed | Yes, every boot. The stored value is a memory of the *last* MCU; this co-processor board may have been moved to another lock |
| If the MCU never answers | Up to 6 asks, 5 s apart, once the MCU wire is alive; then it stops. The lock keeps its current profile and reports no `tuya_pid` |
| Unknown PID | **Keeps the current profile** and logs loudly. Never guesses |

## 3. 🔴 The bug that confirming it found

`cfgMcuPid` was a RAM-only `String`. The **profile** it selected was persisted
(`prefs.putString("prof", …)`); the PID itself was not.

So a lock rebooted onto the correct DP map while reporting **no identity at
all** — an app asking "what lock is this" got `unknown` for a lock we had
already identified, until the MCU happened to answer again. On an MCU that is
slow, or one that predates `0x01`, that state is permanent.

Fixed in `doorlock-1.88`: persisted and restored, and still re-asked each boot
so a moved board re-identifies rather than lying.

**This is why F-21 ("no new firmware work required") did not hold.** The
confirmation was the work.

## 4. 🔴 Two traps for anyone verifying this again

**The `[PID]` exchange never appears in a serial capture.** It happens within a
second or two of boot, before the reader re-attaches after USB CDC
re-enumeration. Every `[PID]` line — the ask, the reply, the profile switch — is
in that blind spot. **Use the broker heartbeat**, exactly as for firmware
versions ([[confirm-fw-version-via-broker-heartbeat]] / `ozkey-35` §8).

**`has_doorbell` alone does not prove the profile switched.** It is
`g_bellObserved || ozDpFind(53)`, and `g_bellObserved` latches permanently the
first time a real doorbell is seen. On a lock where a bell has ever been rung it
reads `true` regardless of the profile. That confound is why `profile` was added
to both heartbeats in `doorlock-1.89` — without it there was no way to see which
DP map a lock was running without standing at it.

**And there are two heartbeat code paths, not one.** Thread locks emit the
bridge presence beacon; Wi-Fi locks emit the MQTT heartbeat. They are separate
blocks and it is entirely possible to add a field to one and not the other. Both
are verified:

```
LockA  (thread)  pid=ozsimfullfeature  profile=ozsim-fullfeature  bell=True
WIFI   (wifi)    pid=(absent)          profile=ozkie-legacy-v0    bell=False
```

The Wi-Fi row is the negative case working: no LockSim on that wire, so no MCU
answers, so `tuya_pid` is **absent** while `profile` still rides.

## 5. What this proves, and what it does not

🔴 **It proves the mechanism, not any product.** LockSim answers with
`ozsimfullfeature`, a PID invented on 2026-08-17 specifically so the path could
be exercised. Every DP in the matching profile is drawn from the *real*
catalogue, so the fiction is the **product**, not the DP numbers — but no real
Tuya MCU has ever been on this wire.

**What to watch when one finally is:** whether it answers `0x01` at all. Older
MCUs may predate the command. In that case the lock keeps whatever profile it
was given, reports no `tuya_pid`, and the app falls through to its unknown-PID
default — all of which is designed behaviour, not a fault.

## 6. Consumers

- **`profiles/products/*.json`** carry `supplier.pid`; `gen_profile.py` emits it
  as `OzProfile.tuya_pid`. Add a product there and the lock can recognise it —
  no C to edit.
- **NEXUS** `pid_capabilities` is keyed on the same PID; the bench fixture is
  seeded as `source: bench_fixture` so a fictional product cannot drift into a
  customer-facing model list (`nexus-11` §5).
- **BANOI** reads `tuya_pid` and resolves capabilities through Nexus with a
  local cache (`XFtposDecisions-109` §12). It should treat **absent as unknown**.

## 7. Still open

- **No real Tuya MCU has been tested** — §5.
- **`ozkie-legacy-v0` is still the compiled-in default**, and it is our invented
  map. A lock that never discovers a PID runs fiction. `profile` on the
  heartbeat now makes those locks findable across a fleet.
- **`ozsim-fullfeature` is deliberately fictional** and must never reach a
  customer-facing list.
