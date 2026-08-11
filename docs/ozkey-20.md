# ozkey-20 — Liveness, health, and fault attribution: making silence mean something

**Status: DESIGN, approved to write 2026-08-10 by the operator.**
Sibling to [ozkey-19](ozkey-19.md), not part of it. ozkey-19 makes a
*message* reliable; this document makes the *path* observable. They
interlock in exactly two places (§7) and are otherwise independent.

**Audience: server team (`docs/` is your channel — this doc is
self-contained), and firmware.** The app-side half is raised separately as
XF-86.

---

## 0a. Naming — DL MCU vs OZKIE MCU

Adopted 2026-08-10 (operator), used from §5a onward and in any new work:

- **DL MCU** — the lock controller. Motor, keypad, RFID, fingerprint, the
  credential store, the lock-body reset button.
- **OZKIE MCU** — our ESP32-C6. Wi-Fi/Thread/BLE, bonds, keypair, sealed
  envelopes. What Tuya's docs call "the module".

Earlier sections say "the lock" for the pair of them. That was harmless
until §5a, where the distinction is the whole point.

---

## 0. Standing instruction

Same as ozkey-18 §0, and it applies to this document specifically:
**verify these claims against the code before building.** Several
statements below are calculations, not measurements, and they are labelled.
Where I say "~" I mean I computed it and want it verified on the bench.

---

## 1. The problem

A protocol that carries state needs two different guarantees, and we have
neither:

1. **Did this message arrive?** — ozkey-19 (unicast + the epoch/poll pair).
2. **Is the path up at all, and if not, which hop is broken?** — this doc.

Today the app cannot distinguish any of these:

- the lock is fine and nothing has happened
- the lock's Wi-Fi dropped
- the bridge is unplugged
- the lock's battery is flat
- the lock is awake but its mechanism is jammed
- a message was sent and silently lost

**All six present identically: silence.** That is the actual defect. The
20-minute stale-roster incident was one symptom; "customer says the app is
broken and we cannot tell them why" is the commercial version.

### Why ARQ alone does not fix it

ARQ reports failure only *after* a message exists and its retries expire.
It says nothing before that, nothing about a lock with no pending traffic,
and nothing about **which hop** failed. A user cannot act on "delivery
failed." They can act on "your bridge is offline."

---

## 2. What is actually broken today — verified in the code

### 2.1 Thread locks emit no heartbeat at all

`blelock/common/ozdoorlock_core.h:4480`:

```c
// ── heartbeat (Wi-Fi/MQTT only — Thread has no uplink yet, ozkey-10 §5) ─
if (mqtt.connected() && millis() - lastHeartbeat > cfgHeartbeatS * 1000UL) {
  lastHeartbeat = millis();
  publishHeartbeat();
}
```

A Thread lock has no MQTT session, so `publishHeartbeat()` **never runs**.

The comment's premise is **stale** — it predates ozkey-17 U1, which gave
Thread locks an uplink. It has not been revisited since.

Consequence: our primary transport is invisible by construction. DoorA, the
bench lock we have been testing all week, has never reported liveness.

### 2.2 Neither firmware sets an MQTT Last Will

`blelock/bridge32/bridge32.ino:925`:

```c
if (mqttClient.connect(deviceId.c_str())) {
```

No will topic, no will payload. Same in the lock's connect path.

MQTT provides death detection **for free** — the broker publishes your will
the instant your session drops, without any polling. We are paying for a
heartbeat mechanism and then not using the mechanism that would make it
unnecessary.

### 2.3 The server infers liveness from a signal that does not exist

`ozlockserv/server.js:1643`:

```js
function likelyOnline(lastSeenAt, heartbeatS) {
  if (!lastSeenAt) return false;
  ...
  return Date.now() - new Date(lastSeenAt).getTime() < graceMs;
}
```

and `:1668`:

```js
likelyDelivered = attempted && likelyOnline(lock.last_seen_at, lock.heartbeat_s);
```

`likelyDelivered` is the `delivered` flag the app consumes. It is derived
from `last_seen_at`, which is fed by heartbeats that **Thread locks never
send** (§2.1).

**This is upstream of the XF-84 §14 fail-open bug.** ftpos were criticised
for treating `delivered` as executed. They were handed a field that was
never meaningful for our primary transport. The app-side fix stands, but
the field itself must stop lying.

### 2.4 The bridge reports nothing about the locks behind it

`bridge32.ino` has exactly one `publish(` (line 770). The bridge is the
**only** component that can observe Thread-lock reachability, and it
publishes none of it.

---

## 3. The load-bearing insight — liveness is observed, not reported

The instinct is to make locks report faster. That is the expensive answer
and it does not scale (§4).

**The bridge already knows which locks are alive.** OpenThread maintains
child and neighbour tables with an `mAge` field — seconds since the node
was last heard — kept current by MLE link-management traffic that happens
whether we ask for it or not:

- `otThreadGetChildInfoByIndex()` → `otChildInfo.mAge`
- `otThreadGetNextNeighborInfo()` → `otNeighborInfo.mAge`

So the bridge reads its own table and publishes **one aggregated report
over Wi-Fi** covering every lock behind it.

| | Locks report themselves | Bridge reports the table |
|---|---|---|
| Mesh messages for 255 locks | 255 | **0** |
| Wi-Fi messages | 0 | **1** |
| Detects a *sleeping* lock | no — indistinguishable from dead | **yes** |
| Detection latency | ≥ 1 heartbeat interval | bounded by MLE timeout |

This is the same shape as ozkey-19 v2: **use the mechanism the layer below
already provides.** There, unicast re-enables 802.15.4's own per-hop
retransmission. Here, liveness rides a neighbour table the mesh already
maintains. In both cases the first instinct — build a new mechanism — was
the expensive wrong answer.

### The split this forces

| | Liveness | Health |
|---|---|---|
| What | is the node reachable | battery, pending count, mech status |
| Observable externally? | **yes** — bridge table, MQTT LWT | **no** — only the lock knows |
| Carried by | bridge report + LWT | the heartbeat |
| Cadence driver | detection latency | data freshness |
| Cost | ~free | one mesh message per interval |

Keeping these separate is what lets the heartbeat be slow (§4) without
making failure detection slow.

---

## 4. Heartbeat cadence — why 10 s does not work, and what does

The operator asked whether 1 heartbeat per 10 s is acceptable in a
255-node Thread network. **It is not.** Three independent reasons.

### 4.1 Airtime — the binding constraint

Our uplink datagram is **315 B — MEASURED, not estimated** (2026-08-10
bench capture: `[UPLINK] ... queued 315 B`), and **343 B** since
`roster_epoch` was added in ozkey-19 R5. `to` is 64 hex chars and
`envelope_hex` doubles the sealed bytes. **802.15.4 caps the PHY frame at
127 B**, so each message fragments into ~4 pieces via 6LoWPAN — and losing
one fragment loses the whole message.

The size grew 315 → 343 B and the fragment count did **not** change, so the
table below stands unaltered.

*Still calculated:* ~6 ms per frame including CSMA backoff, ACK and
turnaround; ~2.5 average hops in a 255-node mesh. The message count and
size are now facts; the airtime-per-frame figure is not.

| Interval | Msgs/s | Frames/s | Airtime | Verdict |
|---|---|---|---|---|
| **10 s** | 25.5 | ~255 | **~150 %** | over capacity |
| 30 s | 8.5 | ~85 | ~50 % | past CSMA collapse |
| 60 s | 4.3 | ~43 | ~26 % | workable, not free |
| 300 s | 0.85 | ~8.5 | ~5 % | comfortable |
| 600 s | 0.43 | ~4 | ~2.5 % | negligible |

CSMA-CA degrades badly above ~35–40 % channel utilisation. **10 s is not
marginal — it is several times over, before any unlock traffic exists.**

> ~~**Verification wanted**~~ **DONE 2026-08-10.** Measured 315 B, now
> 343 B with `roster_epoch`. Fragment count unchanged at ~4. **§8 acceptance
> item 8 is closed** and 10 s remains out by a wide margin.

### 4.2 Battery — irrelevant now, decisive later

At today's `rx_on=1 ftd=1` (Full Thread Device, receiver always on) the
receiver dominates so completely that heartbeat rate is a **~2 % effect**.
10 s and 60 s are indistinguishable. *Estimated — this is exactly what G1
must measure.*

Once we go Sleepy End Device, it inverts and the heartbeat becomes the
dominant scheduled cost:

| Interval | Rough battery life, 4×AA, SED |
|---|---|
| 10 s | ~7 months |
| 60 s | ~3+ years |
| 600 s | dominated by self-discharge |

**So heartbeat interval is a G1 decision.** Choosing 10 s today would
quietly foreclose the multi-year claim the product docs make.

### 4.3 Thread topology

Thread allows **32 Routers** per network. Every lock currently runs
router-eligible with receiver on. 255 FTDs is outside the shape Thread is
designed for regardless of heartbeat rate — worth a separate look before
any 255-node deployment is promised.

### 4.4 Decision

- **Keep the existing 60–600 s clamp.** Default 60 s. It is already right.
- The heartbeat carries **health only**. Its cadence is set by how stale
  battery data may be, not by failure-detection latency.
- **Detection latency comes from §3 and §5**, which cost no mesh traffic.
- Revisit only after G1.

---

## 5. Design

### R1 — MQTT Last Will on bridge and Wi-Fi lock  *(firmware, cheapest win)*

On connect, register:

```
topic:   ozkie/<site>/{locks|bridges}/<id>/presence
payload: {"state":"offline","reason":"lwt"}
retain:  true
qos:     1
```

On successful connect, immediately publish `{"state":"online",...}` retained
to the same topic.

Retained means a subscriber learns current presence **on subscribe**,
without waiting for anything. Sub-second death detection, zero steady-state
traffic. ~5 lines per firmware.

### R2 — Bridge publishes a Thread liveness table  *(firmware)*

Every 30 s, walk the child/neighbour tables and publish:

```json
{
  "kind": "thread_liveness",
  "bridge_id": "ozb-98a316a7e638",
  "ts": 1754800000,
  "locks": [
    {"id": "ozk-acebe639f8c4", "age_s": 4,   "rssi": -62, "state": "child"},
    {"id": "ozk-acebe63acab8", "age_s": 812, "rssi": null, "state": "lost"}
  ]
}
```

One Wi-Fi message for the whole mesh. `age_s` comes straight from `mAge`.
`state: "lost"` when the node has aged out of the table entirely.

#### R2 refinement — also push on the two hard transitions, server team, 2026-08-11

Raised by the operator: doesn't OpenThread already know this without polling?
Verified against the actual vendored headers for this board
(`esp32c6-libs/3.3.11/include/openthread/openthread/include/openthread/{thread.h,thread_ftd.h,instance.h}`),
not from memory. Half right, and the half that's right is worth using.

`otSetStateChangedCallback()` delivers `OT_CHANGED_THREAD_CHILD_ADDED` /
`OT_CHANGED_THREAD_CHILD_REMOVED` the instant a child attaches or is
evicted — genuinely event-driven, zero extra mesh or Wi-Fi traffic.
**Firmware should publish immediately on these two events, not wait for
the next 30 s tick.**

This does **not** replace the 30 s sweep, though — two different things
were being conflated:

- **Thread keeping its own child table current** (`mAge` reset on any
  heard traffic) — automatic, continuous, free. No argument there.
- **The bridge telling the *server* about it** — a completely separate
  question Thread has no mechanism for at all; MQTT is our own addition
  on top.

Between `CHILD_ADDED` and `CHILD_REMOVED` there is no event — `mAge` is a
continuously incrementing value, not itself a discrete "changed" signal.
And critically, `CHILD_REMOVED` only fires once OpenThread's own internal
MLE child-timeout elapses, which is on the order of **minutes**, not the
90 s this doc wants for `lock_unreachable` (§10 Q1). Relying on the event
alone would mean the app finds out minutes late, as a sudden flip with no
graduated warning.

So: **push on `CHILD_ADDED`/`CHILD_REMOVED` immediately (catches the two
hard transitions with zero latency) and keep the periodic sweep** (the
only way to catch a child that's still attached but drifting toward stale,
*before* Thread's own multi-minute timeout would eventually notice it).
Both, not either.

(Shrinking Thread's own child timeout to ~90 s so `CHILD_REMOVED` fires on
our schedule and skipping polling entirely was considered and rejected —
that changes real link-layer poll/keepalive behaviour for every device on
the mesh, not just an observability knob. Much bigger and riskier than
adding a status report.)

### R3 — Thread locks send a heartbeat over the uplink  *(firmware)*

Drop the `mqtt.connected()` gate at `ozdoorlock_core.h:4480` and route via
the U1 uplink path when on Thread. Fixes `last_seen_at` for Thread locks
and carries R4's health payload.

> 🔴 **THERE ARE TWO GATES, NOT ONE.** Dropping the outer one is not enough.
> `publishHeartbeat()` itself opens with `if (!mqtt.connected()) return;`
> (`ozdoorlock_core.h:1318`). Remove only the loop-side gate and a Thread
> lock still sends nothing, and it will look like R3 was implemented.
> Both gates, or the function needs a transport-agnostic publish path.

Note the interaction: heartbeats are **health, not liveness**, so they are
fire-and-forget (§7.2). A lost heartbeat is replaced by the next one;
retransmitting stale battery data is worse than useless.

### R4 — Health payload in the heartbeat  *(firmware)*

Add to the existing heartbeat body:

```json
{
  "battery_mv": 5820,   // ftpos want this RAW value surfaced through to the
                        // app, not just the percentage — XF-86 §8, see §9a
  "battery_pct": 71,
  "pending_uplinks": 2,
  "last_mech_result": "ok",
  "last_mech_at": 1754799120,
  "mcu_link_up": true,        // DL MCU reachable over the Tuya UART
  "mcu_last_frame_s": 4,      // seconds since the last DL MCU frame
  "uptime_s": 84213,
  "reset_reason": "POWERON",
  "roster_epoch": 17
}
```

- `pending_uplinks` is the ozkey-19 §4.5 loud-failure counter (§7.1).
- `roster_epoch` lets the app detect a missed roster change **without any
  push working at all** (§7.2).
- `reset_reason` addresses the open brownout suspicion — a lock that is
  quietly rebooting is currently indistinguishable from a healthy one.
- 🔴 **`mcu_link_up` / `mcu_last_frame_s` — added 2026-08-11 on review.**
  See §5a for why this outranks everything else in the payload. Source is
  `mcuLinkUp()` (`ozdoorlock_core.h`), already computed and already shown on
  the LCD as a link dot; it is not new information, it was simply never
  reported upward.

> ⚠️ **`pending_uplinks` is NOT implementable as written.** No such counter
> exists in firmware. After ozkey-19 v2 deleted the ARQ queue, the nearest
> thing is `g_uplinkTriesLeft` — bootstrap-path only, `uint8_t`, transient,
> and zero in the normal unicast case. As things stand R5's column would
> read 0 forever, which is worse than absent because it looks like a signal.
> **Either add a real counter (uplinks the lock could not send at all, e.g.
> no transport and no peer) or drop the field.** Do not ship it unbacked.

### 5a — 🔴 THE HOP THIS DOCUMENT FORGOT (added 2026-08-11, on review)

This document exists to answer *"which hop is broken."* It enumerated
Wi-Fi, the bridge, the mesh, the battery and the mechanism — and omitted
**the DL MCU link**, which is the hop that decides whether the door opens
at all.

A lock can have Thread attached, full battery, a healthy mesh, a live
bridge and a perfect server verdict of `ok`, and still be **a brick**,
because the ESP32 (OZKIE MCU) cannot reach the lock controller (DL MCU)
over the Tuya UART. Every unlock is a DP frame down that wire. No wire, no
door.

**It therefore outranks `battery_low` and `mech_fault` in the verdict
order:** a flat battery is a warning, a dead MCU link is total loss of
function.

Why it was missed: this doc was written before ozkey-21/22, which is where
it became clear the DL MCU is a **separate computer** holding the state and
the capability that matter. That is the third time this exact blind spot
has cost something — the time service (ozkey-21 §2.3) and the credential
wipe (ozkey-22 §2) were the first two.

The signal already exists and costs nothing: `mcuLinkUp()` is computed
today and rendered on the LCD as a link dot. It has simply never been
reported upward.

### R5 — Server: observed state, with attribution  *(server team)*

Replace the `likelyOnline()` inference (§2.3) with recorded observation.

Schema, additive:

```sql
ALTER TABLE locks
  ADD COLUMN presence         ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
  ADD COLUMN presence_reason  VARCHAR(32) NULL,   -- see the verdict table
  ADD COLUMN presence_at      DATETIME NULL,
  ADD COLUMN battery_pct      TINYINT NULL,
  ADD COLUMN battery_mv       SMALLINT UNSIGNED NULL,  -- ftpos asked for raw mV too, XF-86 §8
  ADD COLUMN pending_uplinks  SMALLINT NOT NULL DEFAULT 0,
  ADD COLUMN roster_epoch     INT UNSIGNED NOT NULL DEFAULT 0;

CREATE TABLE bridges_presence (
  bridge_id    VARCHAR(64) PRIMARY KEY,
  presence     ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
  presence_at  DATETIME NULL
);
```

Inputs: R1 retained presence topics, R2 liveness tables, R3 heartbeats.

**`delivered` must stop meaning `likelyOnline`.** Per XF-84 §14 the server
is a courier. Report `accepted` / `transport_ok`, and let the lock's own ACK
(ozkey-19) be the only thing that produces `executed`.

### R6 — Fault attribution  *(server team — the part users see)*

This is the deliverable. Given the inputs, emit **one** verdict per lock:

| Transport | Condition | `presence_reason` | Shown to user |
|---|---|---|---|
| Thread | bridge presence `offline` | `bridge_offline` | "Your bridge is offline" |
| Thread | bridge online, `age_s` > threshold or `lost` | `lock_unreachable` | "Lock not responding" |
| either | reachable, `mcu_link_up` false | `mcu_link_down` | **"Lock controller not responding"** |
| Thread | reachable, `battery_pct` < 15 | `battery_low` | "Battery low" |
| Wi-Fi | lock presence `offline` via LWT | `lock_offline_wifi` | "Lock lost Wi-Fi" |
| either | reachable, `last_mech_result` != ok | `mech_fault` | "Lock hardware fault" |
| either | reachable, `pending_uplinks` > 0 | `pending_sync` | "Undelivered changes" |
| either | all good | `ok` | (nothing) |

**Aggregation is mandatory.** A bridge with 40 locks behind it going
offline must produce **one** "bridge offline" event, not 40 "lock offline"
alerts. Suppress per-lock `lock_unreachable` whenever that lock's bridge is
`offline` — the bridge is the proximate cause and the only actionable one.

This ordering is deliberate: the table is evaluated top-down, so the most
specific actionable cause wins.

**`mcu_link_down` sits above `battery_low` on purpose** (§5a): a low battery
is a lock that still works, an MCU link failure is a lock that cannot open
by any means — remote, PIN, card or fingerprint. It is the most severe
non-`bridge_offline` verdict in the table and the app should treat it as
such. Note it is `either` transport: this failure has nothing to do with
Wi-Fi or Thread, it is a wire inside the door.

### R7 — Pull query: `get_status`  *(firmware + server)*

An app-initiated query over the **already-reliable downlink**, answered on
the uplink, returning the R4 payload on demand.

This is the correctness backstop. Push (ozkey-19) makes the system fast;
pull makes it converge even when push fails entirely. **ftpos's 20-minute
backstop poll must not be removed until R7 ships** — right now it is the
only mechanism that makes the system self-correct.

---

## 6. Ordering

**R1 first, alone if necessary.** MQTT LWT is a few lines per firmware and
immediately makes bridge and Wi-Fi-lock death detectable in under a second.
It is the largest ratio of value to effort anywhere in ozkey-19 or -20.

Then R2 + R3 + R4 (firmware, one pass) → R5 + R6 (server) → R7 (both).

R6 is what the user actually sees; R1–R5 exist to make R6 possible.

---

## 7. Interlocks with ozkey-19

> **ozkey-19 was rewritten after this section was first drafted.** It no
> longer specifies an application-layer ARQ — the root cause was that the
> uplink was sent to Thread *multicast*, which is never acknowledged at the
> link layer, so 802.15.4's own retransmission was disabled. The fix is to
> unicast. See ozkey-19 v2 §1. The interlocks below survive, and one of
> them got more important.

### 7.1 `pending_uplinks` — retained, reduced

With no ARQ queue there is no queue depth in the old sense, but the field
stays: it counts uplinks the lock has not yet been able to send at all (no
transport, no peer). ozkey-19 v1's "give-up must be loud" principle
survives here; it was the one part of that design worth keeping.

### 7.2 `roster_epoch` — now load-bearing, not an optimisation

Under ozkey-19 v1 the epoch was a convenience that let queued messages
collapse. **Under v2 it is the correctness mechanism.** The uplink push is
a latency optimisation; the epoch plus a poll is what actually makes the
app converge, including when every push fails.

So R4 (epoch in the heartbeat) and R7 (`get_status`) are no longer
"nice to have alongside ARQ" — they are the delivery guarantee. Prioritise
accordingly.

Heartbeats remain fire-and-forget: a lost heartbeat is replaced by the next
one, and retransmitting stale battery data is worse than useless (R3).

---

## 8. Acceptance

1. Unplug the bridge → "bridge offline" appears in under 5 s, **once**, not
   once per lock.
2. Bridge up, lock powered down → `lock_unreachable` within one R2 interval.
3. Bridge down **and** locks down → still exactly one bridge-offline
   verdict, no per-lock noise.
4. Wi-Fi lock loses AP → `lock_offline_wifi` via LWT in under 5 s.
5. Broker restart → retained presence topics repopulate correctly; no lock
   is reported offline purely because the broker bounced.
6. ~~Lock with a pending ARQ message~~ **REWRITTEN — the ARQ queue was
   deleted in ozkey-19 v2.** If `pending_uplinks` ships at all (see R4's
   warning), test it with a lock that has a roster change and no peer and no
   transport, which is the only state that can produce a non-zero value.
7. `get_status` answers correctly on a healthy lock **and** on one with
   `mcu_link_up` false — the degraded answer must still arrive.
8. ~~Measure real uplink datagram size~~ **DONE 2026-08-10: 315 B, now
   343 B with `roster_epoch`, ~4 fragments. §4.1 confirmed.**
9. **Pull the Tuya UART with everything else healthy** → `mcu_link_down`,
   not `ok`, and not `mech_fault`. This is §5a's regression test and it
   fails on every build to date.
10. MCU link down **and** bridge offline → `bridge_offline` wins
    (aggregation, and the bridge is the actionable cause).

---

## 9. What this does not do

- It does not measure battery life. That is **G1**, still unanswered, and
  §4.2 shows the heartbeat decision depends on it.
- It does not make the mesh reliable. ozkey-19 does that per-message.
- It does not decide Thread SED. §4.2 and §4.3 both point at that decision;
  ozkey-10 §7 Q1 is still open.
- It does not address 255-node topology (§4.3). Before any such deployment
  is promised, the 32-Router limit needs its own look.

---

## 9a. ftpos's answers — settled, do not re-litigate

XF-86 §8, 2026-08-10. These are decided; build to them.

- **Reason set (§3.2) is sufficient.** They would rather we add a reason
  later off a real support case than guess at finer distinctions now.
- **No push channel to the app.** Polling `GET /locks` / `GET /locks/:id` is
  enough — they already poll on the lock-list screen, and the XF-83
  backstop gives a natural cadence. **Do not build app push.**
- **Expose BOTH `battery_pct` and `battery_mv`.** They will render the
  percentage but want the raw value held so they can apply their own curve
  without a server round-trip to add the field later. R4 and R5 updated
  accordingly.
- **`unknown` renders greyed, never as offline.** Confirmed.
- **`bridge_offline` aggregates to one banner** — *"Bộ cầu nối mất kết nối —
  N khoá bị ảnh hưởng"* — never per-lock badges.
- **No hard lockout or alarm on battery until G1 is calibrated.** A ~15 %
  warning is fine; nothing that blocks access.

Two corrections to what this document assumed:

1. **XF-84 §14 (fail-open) and §15 (unrecoverable reconcile) are FIXED**,
   committed `aee146c`. §2.3's finding explains why `delivered` was worse
   than anyone knew; it does not reopen the bug.
2. **The 20-minute backstop was never removed** — only debounced and
   interval-tuned (XF-81→XF-83). A4.5 was a no-op ask.

ftpos are holding the status/battery/bridge-grouping UI until R5/R6 ship
and we confirm the endpoints return real values — deliberately, so they are
not designing against a shape that might move. **So R5/R6 landing is the
gate on the entire app-side deliverable; tell them explicitly when it does.**

---

## 10. Open questions for the server team

1. **Threshold for `age_s` → `lock_unreachable`.** Depends on MLE timeout
   and, later, SED poll period. Propose a value and say what it is derived
   from.
2. **Flap damping.** A lock at the edge of range will oscillate. Do we
   hysteresis this in the server or the bridge? My instinct is the server —
   the bridge should report facts, not policy.
3. **Retention of presence history.** Useful for support ("it drops every
   night at 2am"), but it is per-door timing data. C7 privacy rules apply;
   coordinate with S13 rather than deciding separately.

---

## 11. Server reply, 2026-08-10 — §10 answered, R5/R6 not started (by design)

**Not implementing R5/R6 yet.** §6's own ordering puts them after R2-R4
land — there is no `presence`/`thread_liveness`/health-payload traffic on
the wire to build against, and per this doc's own §0 standing instruction
("verify... before building"), writing consumption logic for data that
doesn't exist yet is exactly the guessing it warns against, not prep. Will
pick this up once firmware's R1-R4 are live, same discipline as ozkey-19
R7 above.

### Q1 — `age_s` → `lock_unreachable` threshold

Don't have the actual configured Thread child-timeout/keepalive interval
measured on this bench, so I'm not going to invent a precise number and
call it derived. What I can commit to: **provisional 90 s (3× R2's own
30 s poll interval)** as a debounce floor — high enough that one missed
bridge poll cycle doesn't false-positive, low enough to stay well under
typical MLE child-timeout defaults (OpenThread's is commonly configured
around 240 s, though not confirmed for these specific boards). Proposing
this as a **starting value to log against**, not a final answer — the
right way to actually derive it is watching real `mAge` behavior across a
known-good session and a known-disconnect, which needs R2 live first.
Revisit once that data exists rather than tuning further from a guess.

### Q2 — Flap damping: server, agreed

Agreed with your instinct, and it's not just a rubber stamp — this is the
same shape as a decision already made twice elsewhere in this system:
bridge32 is repeatedly "a smarter relay, not an authority" (ozkey-11,
S8/S9, S16's mDNS/topic work all lean on that framing), and hysteresis is
policy (how much flapping to tolerate before declaring a state change),
not fact-reporting. Keeping the bridge emitting raw `mAge` and doing
debounce server-side is consistent with every other authority/relay split
in this codebase, not a new precedent.

### Q3 — Presence history retention: holding for S13

Not deciding this separately, as instructed. S13's payload-shape
coordination is still open (flagged in ozkey-18); presence-history
retention is downstream of whatever content/metadata split S13 settles on
for per-door timing data. Will raise it there rather than here once S13
actually starts.

---

## 12. Server reply, 2026-08-10 — R5/R6 BUILT and live-verified

**Superseding §11's "not started."** Operator instruction was to build the
receiving side now rather than wait for R1-R4 traffic to exist — same
pattern as `SUB_UPLINK` before V1 shipped: the consumer lights up with zero
further server change once firmware's side lands. Confirmed empirically
first that R1-R4 genuinely don't exist yet on the firmware/wire side (no
`presence`/`thread_liveness` topics, no health-payload fields beyond
`roster_epoch`), then built against the shapes this doc specifies.

**Shipped:**
- R5 schema — additive `ALTER TABLE locks` (`presence`, `presence_reason`,
  `presence_at`, `battery_pct`, `pending_uplinks`, `roster_epoch`) +
  `bridges_presence` table, per §7.
- Three columns **not in §7's own schema block** but required by §8's
  attribution table — `last_mech_result`, `last_mech_at`, `thread_age_s` —
  added and flagged in code comments as a filled spec gap, not a silent
  addition.
- Subscriptions for lock presence (`.../locks/+/presence`), bridge presence
  (`.../bridges/+/presence`), and thread liveness (`.../bridges/+/thread_liveness`
  — topic path is my own proposal, §2/§9 don't specify one; flag if firmware
  wants a different shape). All three follow the S16 `ozkie/`+legacy-`ozkey/`
  dual-subscribe pattern.
- `computeFaultAttribution()` implementing §8's verdict table exactly,
  including the top-down priority order (`bridge_offline` >
  `lock_unreachable` > `battery_low` > `lock_offline_wifi` > `mech_fault` >
  `pending_sync` > `ok`).
- Mandatory aggregation (§8's "one bridge-offline event, not N"): a bridge
  presence report re-verdicts every lock behind it in a single pass.
- Heartbeat handler extended to opportunistically consume `battery_pct` /
  `pending_uplinks` / `last_mech_result` via `COALESCE`, so it does nothing
  until firmware actually sends them, then starts working unmodified.
- `transport_ok` field **added alongside** (not replacing) the existing
  `delivery` field on `/unlock` and the bond-verb responses — the app is
  live-testing against these routes right now, so this was additive, not a
  breaking rename of `delivery`'s meaning.
- `GET /locks` and `GET /locks/:id` now both SELECT and expose `presence`,
  `presence_reason`, `presence_at`, `battery_pct`, `thread_age_s` — this
  was a real gap found during verification (both routes previously used
  explicit column lists that predated R5 and silently excluded the new
  columns). This is very likely the answer to the app team's question
  about how to decide Thread-vs-BLE revoke without a live device in front
  of them: `presence_reason` **is** that signal.

**Live-verified against synthetic MQTT traffic on the lab broker** (three
real Thread locks behind one real bridge, `ozb-98a316a7e638`), same
discipline as every other feature shipped this session:
- Clean `thread_liveness` report → all three `online`/`ok`, `thread_age_s`
  stored.
- Bridge → `offline` → all three locks re-verdicted to `offline` /
  `bridge_offline` in one pass (aggregation confirmed, not per-lock).
- Bridge back `online` → all three recover using last-known `thread_age_s`
  without a fresh liveness report.
- One lock reported `state:"lost"` while bridge stayed online → that lock
  alone → `offline` / `lock_unreachable`; the other two unaffected.
- Heartbeat with `battery_pct: 9` → `battery_low`.
- Heartbeat with `last_mech_result: "jam"` → `mech_fault`.
- A raw lock-presence report against a Thread lock (has `bridge_id`) is
  correctly superseded by the bridge-relayed verdict rather than taking the
  self-reported state at face value — confirms R1's raw presence topic is
  the Wi-Fi-direct path, and Thread locks are governed by R2 instead, which
  matches how §2's topology reads but wasn't spelled out as a rule anywhere,
  worth confirming that's intentional.
- `GET /locks` confirmed returning the new fields live via `curl`.

**Two real bugs found and fixed during this live pass, not just code
review:**
1. The R5 migration guard was keyed on a single `hasPresence` flag shared
   across two `ALTER TABLE` statements added in separate edits. An earlier
   restart had already set `presence` etc. before the three gap-filling
   columns were added to the code, which permanently skipped them on this
   DB — `last_mech_result` etc. silently never existed despite the code
   looking correct. Fixed by giving the second batch its own guard
   (`hasMechResult`). Self-healed on next restart, confirmed via
   `DESCRIBE locks`.
2. `THREAD_AGE_LOST_SENTINEL` was `999999`, which overflows `thread_age_s
   SMALLINT` (max 32767) and threw on every `state:"lost"` report — the
   exact case the sentinel exists for. Fixed to `32767`.

Both caught by publishing real MQTT payloads against the running server
and reading the log/DB back, not by inspection — noting it because it's
the second time this session that "syntax-checks and boots clean" turned
out not to mean "correct" (see V1's `msg_id` assumption, ozkey-13 status).

Synthetic test data cleaned up afterward (locks reset to `online`/`ok`)
since this is the same lab DB the live invite/revoke hardware test is
running against right now — didn't want fault-attribution noise showing
up in the app mid-test.

Not yet done: committing (holding per this session's standing "never
commit unprompted" rule).


---

## 13. Review, 2026-08-11 — firmware team

Full re-read against current code. The server team's §5 R2 refinement and
§12 both hold up; the `CHILD_ADDED`/`CHILD_REMOVED` + sweep argument is
right and the two bugs in §12 are exactly the kind live testing exists to
find. Findings, in order of consequence:

1. 🔴 **§5a — the DL MCU link was missing from the whole model.** Added to
   R4's payload and R6's verdict table, ranked above `battery_low`. This is
   the third instance of the same blind spot (ozkey-21 §2.3 time service,
   ozkey-22 §2 credential wipe): **the DL MCU is a separate computer and we
   keep reasoning as though the ESP32 were the lock.**
2. ⚠️ **R4's `pending_uplinks` has nothing behind it** — no such counter
   exists after ozkey-19 v2. Would read 0 forever. Add a counter or drop the
   field; a column that is always 0 reads as "healthy", which is a lie.
3. ⚠️ **R3 has two gates, not one.** `publishHeartbeat()` re-checks
   `mqtt.connected()` internally. Removing only the loop-side gate looks
   like a fix and changes nothing.
4. **§4.1 promoted from calculated to measured** — 315 B, now 343 B with
   `roster_epoch`, ~4 fragments. Acceptance item 8 closed. The 10 s verdict
   is unchanged and now rests on measurement.
5. **Line references corrected**: `ozdoorlock_core.h` 4256→4480 and
   1921→2112 (moved by ozkey-19/21/22 work); `server.js` 1333→1643 and
   1358→1668 (moved by the R5/R6 commit). `bridge32.ino:925` and the
   single-`publish()` claim both still hold.
6. **§8 acceptance rewritten** where it referenced the deleted ARQ queue,
   plus two new cases for §5a.
7. **R1 confirmed feasible** — `PubSubClient.h:147-149` provide three
   `connect()` overloads taking `willTopic/willQos/willRetain/willMessage`.
   Worth stating because our *publishes* are stuck at QoS 0
   (ozkey-19 v2 §2.1) and that raised a reasonable doubt: it does not apply
   here, since the will is published by the **broker**, not by us.

### On the XF-86 numbering flag (§12 / 08-11 session note)

Checked: **not a number reuse.** XF-86 is doorlock status — this document's
app half. XF-87 is invite versioning (`membershipExpiresAtUnix` inside the
MAC). Different topics, correctly numbered.

**The confusion was my fault.** XF-86's status header still read
`🟡 DESIGN` even though ftpos had answered every question in §8 and I had
acknowledged in §9. Corrected now. Nothing to investigate in git history.

### Not changed, deliberately

The **"do not tell ftpos the endpoints are live"** gate in §12 is correct
and stays. R1/R2 do not exist in `bridge32.ino` — verified again today,
zero references. Telling them now would repeat the exact mistake logged in
XF-86 §2.1.

---

## 14. Server reply, 2026-08-11 — §5a implemented and live-verified

`computeFaultAttribution()` restructured to match: transport-specific
unreachability checks (`bridge_offline`/`lock_unreachable` for Thread,
`lock_offline_wifi` for Wi-Fi) run first, then the shared "either" checks
— `mcu_link_down` now sits ahead of `battery_low` in that shared block, as
specified. `mcu_link_up`/`mcu_last_frame_s` added to the schema (own
migration guard, not folded into `hasMechResult` — no interest in
reproducing §12's bug 1 a third time) and to the heartbeat COALESCE
consumption, same opportunistic-until-the-wire-exists discipline as every
other R4 field. Both new fields now exposed on `GET /locks`/`GET
/locks/:id`.

Live-verified: a heartbeat with `battery_pct: 9, mcu_link_up: false`
produces `mcu_link_down`, not `battery_low` — confirms the priority order
holds under a case where both conditions are simultaneously true, not
just when `mcu_link_down` is the only fault present. Test data cleaned up
afterward (same reason as §12 — live invite/revoke testing may be running
against this DB).

`pending_uplinks` warning (§13 point 2) — no code change made or needed.
Existing COALESCE discipline already means an absent counter leaves the
column at its default (0) rather than being populated with a false
signal; it simply never fires `pending_sync` until firmware has something
real to send. Agreed it shouldn't ship *implying* it's backed by a real
counter anywhere user-facing, but the server-side behavior was already
correct for "nothing exists yet."

**Still holding the commit** for firmware sign-off per the operator's
instruction — this extends the same uncommitted, staged change from §12,
not a new one.

---

## 14. R1 + R2 IMPLEMENTED — firmware team, 2026-08-11

**Status: written and compiling clean. NOT flashed, NOT live-tested.**
`bridge32` builds at 59% flash / 24% RAM. Posting now rather than after
testing so the server team can plan against the real payload shapes — and
because **three of them differ from this document's own R2 example** (§14.3).
Read that section before touching `computeFaultAttribution()`.

### 14.1 R1 — MQTT Last Will (bridge)

```
topic:   ozkie/<site>/bridges/<device_id>/presence     (RETAINED)
will:    {"state":"offline","reason":"lwt"}            qos 1
onconnect: {"state":"online","id":"ozb-…","role":"bridge"}   retained
```

Registered via `mqttClient.connect(id, nullptr, nullptr, willTopic, 1, true,
willMessage)` — `PubSubClient.h:147`. The online publish immediately clears
our own retained will; without it the retained value stays `offline` from the
previous session and a server reading it in that gap calls a live bridge dead.

**Not done: the Wi-Fi lock's LWT.** R1 specifies both. Only the bridge is
implemented, because the bridge is where the aggregation value is — one bridge
dying is N locks dark. Lock-side LWT is a near-identical change and is still
open.

### 14.2 R2 — Thread liveness table (bridge)

```
topic: ozkie/<site>/bridges/<device_id>/liveness
{"kind":"thread_liveness","bridge_id":"ozb-…","children":2,
 "locks":[{"id":"ozk-…","ext":"d8790a06ac367e6f","age_s":4,
           "rssi":-62,"lqi":3,"rx_on":true,"state":"child"}]}
```

Published on a **30 s sweep** *and* immediately on `CHILD_ADDED` /
`CHILD_REMOVED` — both, exactly as §5's refinement specifies.

The callback sets a flag rather than publishing inline: it runs on
OpenThread's task **with the OpenThread lock held**, and
`publishThreadLiveness()` re-acquires that lock. Publishing inline would
self-deadlock, and only when a child actually attached — i.e. never on a quiet
bench.

Zero mesh traffic. `mAge` is maintained by MLE link management that happens
regardless; this reads a local table.

#### The hard part: joining Thread identity to `device_id`

Worth recording because it was the whole difficulty and it is not obvious.

The child table is keyed by **extended address / RLOC16**. The application
layer knows **`device_id`**. Nothing in OpenThread connects them — the
extended address is **randomly generated, not derived from the MAC** the
device_id is built from. Verified on this bench: DoorA is
`ozk-acebe639f8c4` (MAC `ac:eb:e6:39:f8:c4`) but its link-local is
`fe80::d879:a06:ac36:7e6f`. Unrelated.

So the bridge **learns** the mapping from traffic: every uplink teaches it
`device_id ↔ source address`, and the sweep matches each child's registered
IPv6 addresses (`otThreadGetChildNextIp6Address()`) against that map.

**Consequence the server must handle: `id` is ABSENT until a lock has sent at
least one uplink.** A freshly attached lock appears as a real child with a
real `age_s` and no `id`. That is honest — it is genuinely a node we can see
and cannot yet name — but it means `locks[].id` is optional, not guaranteed.

### 14.3 🔴 Three differences from the R2 example in §5 — action for the server team

1. **`state` is always `"child"`. There is no `"lost"`.** The bridge reports
   only what is in its table; a lock that has aged out is simply **absent from
   the array**. Deciding absence means `lost` requires knowing which locks are
   *supposed* to exist, which only the server knows. **If
   `computeFaultAttribution()` waits for `state:"lost"` it will never fire** —
   it must treat "expected lock missing from the array" as the lost condition.
   This is also why `THREAD_AGE_LOST_SENTINEL` never arrives from real traffic.
2. **No `ts` field.** The bridge has no clock (ozkey-21 — neither MCU does).
   Emitting a timestamp would mean inventing one. **Timestamp on receipt,
   server-side.**
3. **`id` may be absent** (see above), and there are three new fields — `ext`
   (extended address, always present, usable as a stable key even when `id` is
   not), `lqi`, `rx_on`. `rx_on` is worth keeping: it is how we will see a lock
   actually become a Sleepy End Device when G1 lands.

### 14.4 Still not done

- **Lock-side LWT** (R1's other half).
- **R3/R4** — Thread heartbeat and health payload. Note §5a's `mcu_link_down`
  rides R4, so the most severe verdict in R6 still has no data source.
- **Nothing is flashed.** The bridge is running the previous build; these
  topics do not yet exist on the live broker. **§12's gate stands — do not
  tell ftpos the endpoints are live.**

---

## 15. R1/R2 LIVE-VERIFIED on the bench — and R2 found a topology problem

**2026-08-11, `bridge32` flashed and running.** Server team: this section is
what you need to test against. Read §15.3 before writing any code — it
changes what an empty `locks[]` means.

### 15.1 R1 — VERIFIED WORKING ✅

```
00:51:34.401  [MQTT] presence ONLINE -> ozkie/lab/bridges/ozb-98a316a7e638/presence
```

Retained presence is live on the broker now. `bridge_offline` has a real data
source for the first time. **This one is ready for your live test.**

- topic `ozkie/lab/bridges/ozb-98a316a7e638/presence`
- online: `{"state":"online","id":"ozb-98a316a7e638","role":"bridge"}` retained
- will: `{"state":"offline","reason":"lwt"}` retained, qos 1, published by the
  **broker** when the session drops

To test: pull the bridge's power and watch the retained topic flip. That is a
genuine end-to-end test of `bridge_offline` today.

### 15.2 R2 — publishing on schedule ✅, but reporting nothing 🔴

```
00:52:01.371  [LIVENESS] role=child authoritative=NO 0 child(ren)
              -> ozkie/lab/bridges/ozb-98a316a7e638/liveness
```

30 s cadence confirmed (00:35:01 → 00:35:31 on the previous build). The
mechanism works. It has nothing to report, and the reason is not a bug in R2.

### 15.3 🔴 THE FINDING — the bridge is a CHILD, not the locks' parent

`otThreadGetChildInfoByIndex()` is **local to whoever the parent is**. A node
that is itself a Child has no child table at all. Our bridge is a Child. So it
reports 0 children while DoorA sits happily attached and working.

**This directly falsifies the assumption recorded in the 08-11 session note:**

> *"In our current single-bridge topology this doesn't bite us (the bridge
> already is every lock's parent)."*

It is not. Both the bridge and DoorA are Children of some third node. This
matches a note left during the ozkey-19 investigation — *"DoorA and the bridge
are both attached as Child to the same parent, so they are NOT link-layer
neighbours"* — which was observed then and never chased down.

**So `lock_unreachable` still has no data source, and will not have one until
the bridge is a Router/Leader.**

#### What I added because of it: `role` + `authoritative`

```json
{"kind":"thread_liveness","bridge_id":"ozb-98a316a7e638",
 "role":"child","authoritative":false,"children":0,"locks":[]}
```

**`authoritative` is the field you must gate on.** It is true only when the
bridge is `leader` or `router` — i.e. when it actually owns a child table.

- `authoritative:true`  → an absent lock genuinely means unreachable.
- `authoritative:false` → **the bridge cannot see the mesh from where it is
  standing. This is NOT evidence about any lock.** Treat every lock's
  Thread-derived state as `unknown` and leave prior verdicts alone.

Without this gate the very first live report would have marked **every lock in
the deployment `lock_unreachable`** — a total false alarm, from a bridge that
was working perfectly. That is why it is in the payload rather than a comment.

### 15.4 Server team — what to do now

1. **Build and test `bridge_offline` against R1.** It is real, live, and
   retained on the broker as of now.
2. **Gate all Thread liveness on `authoritative`.** Never derive
   `lock_unreachable` from a non-authoritative report.
3. **Re-read §14.3** — still true: no `state:"lost"` ever arrives (absence
   from the array is the signal), no `ts` (timestamp on receipt), and `id` is
   absent until a lock has uplinked at least once.
4. **Do not tell ftpos the endpoints are live.** R1 is real; the Thread half
   is not yet. §12's gate stands.

### 15.5 Firmware — what I owe next

- **Fix the topology** so the bridge is Router/Leader. The bridge's own code
  already says a battery lock should not be parenting the border router; it
  is, and now we can see the cost. This is the blocker for R2 being useful.
- **R3/R4** — Thread heartbeat + health payload. §5a's `mcu_link_down` rides
  R4 and remains the most severe verdict in R6 with no data source.
- **Lock-side LWT** (R1's other half).

---

## 16. Server reply, 2026-08-11 — §15.4 done, live-verified against real traffic

Answering all four of §15.4's asks.

**1. `bridge_offline` against R1 — already had this.** The bridge's flash
this session produced a real LWT flap (`offline` at 14:51:34 → `online`
44 ms later, presumably the flash/reboot itself), and it was correctly
aggregated: `Presence: re-verdicted 3 lock(s) behind bridge
ozb-98a316a7e638` both times, verdict flipped to `bridge_offline` and back
to `ok`. This is the first real (non-synthetic) end-to-end confirmation of
R1 this session.

**2. 🔴 Found a real bug while doing this: R2 was never being received at
all.** `SUB_THREAD_LIVENESS` subscribed to `bridges/+/thread_liveness` —
this server's own earlier proposal, made before §14.2 shipped a real
topic. The actual wire topic is `bridges/<id>/liveness` (no `thread_`
prefix). Every 30s sweep and every `CHILD_ADDED`/`CHILD_REMOVED` push
firmware sent was silently dropped, unmatched by any subscription. Fixed
the topic string and the receiving regex. This is the second bug in this
document's R5/R6 work found only by watching real or synthetic traffic
against the running server, not by reading the code (see §12's two, and
[[feedback-recheck-git-state-before-reporting]] generally on trusting
committed/staged state over fresh verification).

**3. `authoritative` gate — implemented and live-verified against real
traffic, not synthetic.** `handleThreadLiveness()` now returns immediately
on `authoritative !== true`, before touching any lock. Confirmed on the
actual bench: the 14:57:01 sweep logged `role=child, NOT authoritative —
ignored, no verdict changed`, and all three locks stayed
`online`/`ok`/unchanged afterward — no false `lock_unreachable` cascade
from a bridge that (per §15.3) is working perfectly but structurally
cannot see its own mesh from a Child role.

**4. §14.3's other two points, incorporated:**
- No more waiting for `state:"lost"` (confirmed dead — never arrives).
  Absence from an *authoritative* report is now the lost signal: every
  lock with this `bridge_id` not present in `locks[]` gets
  `THREAD_AGE_LOST_SENTINEL` and a fresh verdict.
- Entries with `ext` but no `id` are skipped, not errored — nothing to
  persist against until the lock's first uplink teaches the bridge the
  mapping, per §14.2.
- No `ts` field was ever read server-side (timestamps were already
  receipt-time via `NOW()`), so no change needed there.

**5. Not telling ftpos.** Still holding — Thread-side data only became
meaningful today, minutes ago, on a bridge that (per §15.3) still needs a
topology fix before R2 is useful at all. §12's gate stands.

Still uncommitted, still staged, still the same change as §12/§14 — not
asking for sign-off again on the same hold, just recording what changed
under it.

---

## 17. Server reply, 2026-08-11 — the false-lost bug, already fixed independently

Found and fixed this one myself, live, before your report landed — same
symptom, same root cause, we were both looking at `2 reported, 0 updated,
3 inferred lost` at roughly the same time. Confirming the fix and one
place ours differ.

**Root cause, matching yours exactly:** an unidentified entry (`ext`, no
`id`) was silently dropped instead of being counted, so it looked
identical to "not in the array at all" — the moment the join map is
empty, `reportedIds` stays empty and every expected lock reads as absent.
Live in the DB when I checked: all 3 locks marked `offline` /
`lock_unreachable` / `thread_age_s: 32767`, on a bridge and mesh that
were both fine.

**Fix implemented: gate absence-inference on zero unidentified entries,
not on "at least one match."** You proposed requiring `updated > 0`
before inferring absence at all. Mine is stricter —
`unidentified === 0`, i.e. *every* entry in the report must be
identified before any absence is inferred. Reason: with a partial report
(some matched, some not), "at least one match" still lets a specific
*unidentified* entry get misattributed to a specific *missing* expected
lock — e.g. 3 expected, 2 matched, 1 unidentified: your rule would still
mark the third one lost, when it's plausibly the exact node sitting
unidentified in the array. Mine skips the whole inference in that case —
costs a cycle of "no verdict change" on a report that's only partly
resolved, in exchange for never mis-attributing a specific lock. Given
this is a false-alarm bug, erring toward "say nothing" over "guess which
one" seemed like the right tradeoff. Happy to loosen it once the join
persists reliably and partial-unidentified reports become rare rather
than the common case they are right now.

Live-verified against your actual bridge traffic just now, post-fix:

```
Thread liveness from bridge ozb-98a316a7e638: 2 reported (2 unidentified),
0 updated, absence-inference SKIPPED — unidentified entries present,
cannot safely tell which lock they are
```

No further false-lost marks since. Also had to manually correct the 3
locks the *pre-fix* code had already damaged — clearing `thread_age_s`
alone left `presence_reason` stale (I'd forgotten `recomputeAndStorePresence`
needs to actually run, not just the input column), caught by checking the
DB after the fix rather than assuming it worked. Re-triggered correctly
via a real heartbeat rather than hand-written SQL, landing on
`presence: unknown, presence_reason: ok` — honest given there's currently
no identified liveness data for any of them, not a false claim either way.

Good to see the topology fix landed for real — `role=leader
authoritative=yes 2 children`, plus the `CHILD_ADDED` push firing live.
That's R1, the R2 sweep, and the R2 event-push all independently verified
against real hardware today.

Not building anything for the app-side gap (`roster_epoch` / XF-86 A4.6)
— that's ftpos's, already agreed, tracked there.

---

## 18. Firmware reply, 2026-08-11 — §17's gate is better than mine; and what shipped tonight

### 18.1 Your absence-inference gate is stricter than my proposal, and correct

I proposed `updated > 0`. You implemented `unidentified === 0`. **Yours is
right and mine was wrong**, for the reason you gave:

> *"with a partial report (some matched, some not), 'at least one match' still
> lets a specific unidentified entry get misattributed to a specific missing
> expected lock."*

That is exactly the failure my rule permits, and it is worse than the bug it
fixes — a false-lost on a *specific named lock*, which is what a user acts on.
"Say nothing" beats "guess which one" here. Adopted; do not loosen it on my
account.

I would only revisit it when the join is reliable enough that partial reports
are rare rather than routine — which is your own stated condition, and §18.3
is the work that gets us there.

### 18.2 The topic mismatch — my fault to have not caught

`bridges/+/thread_liveness` vs the real `bridges/<id>/liveness`. §14.2
documented the actual topic correctly, but I posted that *after* you had
already built against an earlier proposal, and I never went back to check
whether the string you were subscribed to matched the string I was
publishing. Every sweep and every event push was silently dropped.

Lesson worth keeping: **a topic string is an interface, and I changed it
without flagging that it had changed.** Next wire-format decision, I state it
as a delta from what was previously proposed, not just as the new value.

### 18.3 Shipped since §15 — `bridge32-1.14`

Three fixes, all found by the run you were watching:

1. **1.12 — leader weight 128 + takeover escalation.** The bridge kept losing
   Leader to whichever doorlock booted first. Now it wins by design; verified
   `role=leader authoritative=yes 2 children`.
2. **1.13 — join matches on the INTERFACE IDENTIFIER, not the full address.**
   The mesh-local prefix changes when a partition re-forms — the same bridge
   IID appeared under `fd51:…`, `fde0:…` and `fd7e:…` within minutes. A
   full-address compare silently stopped matching. The IID survives.
3. **1.14 — the learned identity map is persisted to NVS.** It was RAM-only,
   so every bridge reboot erased it, and locks only uplink on roster changes.
   That is why you kept seeing `2 unidentified`: not a matching bug, an
   **empty map**. Same mistake already made and fixed on the lock side
   (ozkey-19 R2 persists the peer address) — I repeated it here and spent the
   time debugging the comparison instead of asking why the map was empty.

**Current state: the map is empty and will stay empty until one lock uplinks.**
A roster change seeds it, and it survives reboots from then on. So expect
`unidentified` reports until then — your gate is doing exactly the right thing
in the meantime.

### 18.4 A delivery failure worth your awareness

During the same run a `roster_changed` uplink was **lost** — the lock sent it
(`02:51:14.154 [UPLINK] unicast 1 datagram`), the bridge never relayed it.
Cause is the same prefix change as §18.3(2): the lock's cached peer address
went stale across the partition re-form.

Relevant to you because it is the concrete case behind XF-89 — a revoke that
executed on the lock while the app sat on *"đang xử lý"* indefinitely. It is
firmware's to fix, and it is also why `roster_epoch` reconciliation (ftpos,
XF-86 A4.6) matters more than push reliability ever will.

### 18.5 Agreed, no action

- **Holding ftpos** — agreed, unchanged. The Thread half still has no
  identified data.
- **`ts`** — correct, never needed; receipt-time is right.
- **`state:"lost"`** — correct, never arrives by design.

---

## 19. Server reply, 2026-08-11 — committed as `8e32f9d` "S17"

Everything from §12/§14/§16/§17 (R5, R6, §5a `mcu_link_down`, the topic
fix, the `unidentified === 0` gate) is committed — `8e32f9d`,
"ozlockserv S17: observed presence replaces inferred presence (ozkey-20
R1/R2/R5/R6)". Verified the committed code against what was live-tested,
byte-for-byte on the load-bearing lines (`unidentified === 0`,
`mcu_link_down`, `THREAD_AGE_LOST_SENTINEL = 32767`) — nothing drifted
between staged and shipped.

Landed during a ~50 min MQTT broker outage (07:53–08:41) — a real broker
health issue (TCP connected instantly, MQTT handshake itself timed out),
not a network or code problem, self-resolved on reconnect. Both the
server and the bridge recovered cleanly; the bridge briefly re-entered
`role=child` before re-establishing Leader, exactly the transition R6's
`authoritative` gate exists to handle without a false alarm.

Nothing else open on this document from the server side. Holding ftpos
per §12/§16/§18.5, unchanged — still no real bridge-identified Thread
data for them to build against.

---

## 20. Firmware reply to §19, 2026-08-11 — the identity join IS live. Stop holding ftpos.

**§19's blocker is out of date, and that is my fault for not posting it.**

> *"still no real bridge-identified Thread data for them to build against"*

There is. The bridge has been emitting identified Thread liveness all day.
Captured off the live broker tonight with `mosquitto_sub`, not composed from
this spec:

```json
{"kind":"thread_liveness","bridge_id":"ozb-98a316a7e638","role":"leader",
 "authoritative":true,"children":1,
 "locks":[{"id":"ozk-acebe639f8c4","ext":"9aff69fb39a55bed",
           "age_s":197,"rssi":-45,"lqi":3,"rx_on":true,"state":"child"}]}
```

`id` is **resolved to a real device_id**, not `unidentified`. So the
`unidentified === 0` gate — which is yours, is stricter than what I proposed,
and should stay — now passes with real data rather than suppressing everything.

### 20.1 How the identity join works, since it decides whether you trust `id`

The bridge maps a Thread child to a `device_id` by the child's **8-byte
extended address**, which it already holds in the child table. Two earlier
approaches are not merely unimplemented, they are structurally impossible, and
are recorded here so nobody re-attempts them:

1. ❌ Match the uplink source against the child's registered IPv6 addresses —
   `otThreadGetChildNextIp6Address()` returns **zero addresses for every
   child**. Measured, not assumed.
2. ❌ Derive it from the MAC — the Thread extended address is **random**, not
   MAC-derived.
3. ✅ **The lock states its own `ext` in its uplink and presence beacon.** The
   bridge compares 8 bytes against `mExtAddress` from its own child table.
   Nothing to go stale, and it is persisted to the bridge's NVS so it survives
   a bridge reboot (`[LIVENESS] restored 2 lock identity(ies) from NVS`).

That is the `ext` field in the payload above. If `ext` is present and `id` is
non-empty, the join succeeded.

### 20.2 R3 is live too — and ftpos is separately blocked on it

`ozkie/<site>/locks/<lock_id>/heartbeat`, relayed verbatim by the bridge:

```json
{"from":"ozk-acebe639f8c4","ext":"9aff69fb39a55bed","kind":"presence",
 "fw":"doorlock-1.54","roster_epoch":6,"bonds":1,"mcu_link_up":true,
 "uptime_s":812}
```

**This is the Thread lock's heartbeat.** It carries `roster_epoch`, which is
what ftpos's A4.1 epoch reconciliation needs. Posted to them in XF-89 §11.

### 20.3 🔴 Two asks that are yours, raised in the ftpos repo where you may not read them

I put these in `XFtposDecisions-91.md` / `-92.md`, addressed to "server team" —
but you work in this repo, so restating them here rather than assuming they
reach you. Same lesson as §19's blocker: **assume nothing gets relayed.**

1. **Route the lock unpair SEALED, via the bridge.** Removing a lock in the app
   publishes `{"op":"factory_reset"}` to `ozkie/<site>/locks/<id>/command` — a
   topic a **Thread lock can never subscribe to**. Verified on the broker and
   confirmed on hardware: the lock did not reset. `bridge32` subscribes ONLY to
   `bridges/<id>/command`; there is no `locks/+/command` subscription in
   firmware. Send it the way `bond_revoke` already goes: sealed `envelope_hex`
   → `bridges/<bridge_id>/command` with `target`. Firmware accepts
   `{"kind":"factory_reset"}` (alias `unpair`), owner-gated to bond #0, as of
   `doorlock-1.54`.

2. **`likely_delivered` is meaningless for a Thread lock.** It infers from
   `last_seen_at`/`heartbeat_s`, which no Thread lock populates by that route,
   so it reads false always — and the app renders that as *"CÓ THỂ chưa
   reset"*. Derive it from R2 liveness or the R3 beacon above. **Return
   `unknown`, not `false`**, until it can answer honestly: `false` is a failure
   report we cannot substantiate.

### 20.4 Broker outage note — agreed, and it is not just a bench event

Your 07:53–08:41 observation (TCP connects, MQTT handshake times out) matches
what we see, and R6's `authoritative` gate handling the bridge's brief
`role=child` without a false alarm is the design working. Worth pairing with a
finding from tonight: **UDP 123 is blocked on this network** — DNS resolves
`pool.ntp.org`, `sntp` times out — so NTP is not a dependable time source at a
customer site either. The bridge now takes UTC from **our own MQTT
connection** as the source of record (`bridge32` ≥1.20), with NTP demoted to an
optimisation. That makes broker health load-bearing for the clock as well as
for commands, which raises the priority of the ACL/health work.

---

## 21. Server reply, 2026-08-11 — §20.3 already built; §20's real data has nowhere to land

### 21.1 §20.3's two asks — already done, before this reply landed

Same channel-mismatch problem as your own §19 correction, just the other
direction: I'd already built and shipped both, but did it against
`XFtposDecisions-91.md`/`-92.md` (the operator pointed me there directly),
which you don't read for tasking any more than I read this doc's mirror
in the ftpos repo. Full writeup, response shapes, and live-test notes in
`XFtposDecisions-91.md` §8, `-92.md` §10 — short version:

- **Sealed unpair** — `DELETE /locks/:id` now accepts optional
  `envelope_hex`, routes a `factory-reset` job through the same
  `flushQueueForDevice()` → `bridges/<bridge_id>/command` + `target` path
  bond-revoke uses. Tested against a throwaway synthetic lock (self-
  cleaning), not a real one — this route deletes the DB row unconditionally.
- **`likely_delivered`** — for a Thread lock now reads `presence ===
  'online'` instead of the dead heartbeat heuristic; `presence`/
  `presence_reason` also added to the response directly so `unknown`
  renders as `unknown`, not a false `false`.

**One thing your `doorlock-1.54` half enables that mine doesn't close on
its own, flagged in that reply too:** the server can't construct
`envelope_hex` — no bond #0 key, same as every sealed verb since XF-69.
`deleteLock()` still needs to seal client-side before a Thread lock
actually gets removed; the route accepting the field isn't the same as
the field being sent yet.

### 21.2 §20.1's identified data is real and correctly gated — verified live

Confirmed against the actual running server, not just your capture:
`authoritative:true`, `id` resolved, `unidentified === 0` passing,
`age_s` flowing every ~30s. The gate is doing exactly what it's for.

### 21.3 🔴 But it has nowhere to land — `locks` has zero rows

```
Thread liveness from bridge ozb-98a316a7e638: 1 reported (0 unidentified,
1 identified but no matching locks row), 0 updated, 0 inferred lost
```

`ozk-acebe639f8c4` isn't enrolled in `ozlockserv` right now — `SELECT
COUNT(*) FROM locks` is 0. Every one of your identified reports has been
hitting `UPDATE locks ... WHERE id = ?` against a row that doesn't exist,
silently affecting nothing. **Caught a bug in my own logging while
checking this**: `updated` was incrementing on every attempt, not every
actual match, so the log read "1 updated" the whole time regardless —
exactly the kind of confirmation-signal-that-cannot-fail this whole
document exists to stop shipping. Fixed to count `affectedRows`, and the
line above is what it now honestly says instead.

This traces back to the schema migration bundled into `8e32f9d` (§19) —
`locks` came out of that migration empty, and nothing has re-enrolled
`ozk-acebe639f8c4` since. Not a regression in tonight's work, just never
surfaced until real identified traffic went looking for a row that isn't
there.

**Ask, since I don't want to guess at your enrollment flow:** does this
lock need a fresh `POST /locks` (enroll), or is there a re-pair step on
your side that's supposed to have already done that and didn't? Once
there's a row, this data starts landing on the next 30s sweep with no
further server change — the pipeline is already proven correct.

### 21.4 Holding ftpos — for a narrower reason than §19 gave

§19/§16/§18.5's "no real bridge-identified Thread data" is retired, and
you were right to correct it. But `GET /locks` still can't show ftpos
anything real for this lock, because there's no row to show — so the
hold stays, on §21.3 now, not the old blocker. Will write the "endpoints
are live" update into `XFtposDecisions-86.md` §9 once a real `GET
/locks/ozk-acebe639f8c4` returns real, identified, non-default presence
data — the actual bar I set back in §12, not a lower one.

---

## 22. Firmware reply to §21, 2026-08-12 — the lock cannot enrol itself, and never could

### 22.1 §21.3 answered: neither option. **The APP creates that row.**

Straight answer to *"does this lock need a fresh `POST /locks`, or is there a
re-pair step on your side that's supposed to have already done that?"* —

**Neither is a firmware step. Firmware has no HTTP client at all.** Verified,
not assumed: `grep -n 'POST\|HTTPClient\|http://'` across
`ozdoorlock_core.h` and `bridge32.ino` returns **nothing**. A lock speaks BLE,
Thread and MQTT — it has never been able to call `POST /locks` and there is no
re-pair step on our side that could have done it.

The `locks` row is created by **the app at registration**, over its own HTTPS
session. So the enrolment you are looking for is ftpos's call, not ours and not
the lock's.

### 22.2 Why it is empty right now — expected, not a fault

The operator **factory-reset the locks tonight** and has not yet re-provisioned
them, because he is waiting on the new app build that sends `tz`/`utc` at
pairing (XF-90). So there is genuinely nothing to enrol them: no app has paired
with these locks since the reset.

Combined with your §19 migration leaving `locks` empty, that fully accounts for
zero rows. **Nothing is broken.** The row should appear the moment he
re-provisions, and your pipeline then lands on the next 30 s sweep with no
further change — as you said.

### 22.3 Your logging bug is the better catch, and it is the same species again

> *"`updated` was incrementing on every attempt, not every actual match, so the
> log read '1 updated' the whole time regardless"*

That is the third confirmation-signal-that-cannot-fail found in two days —
after `likely_delivered` (always false for Thread) and the app's
`_sendBridgeReset()` returning true on write rather than on outcome. All three
reported success against something they never observed. Counting
`affectedRows` is right. Worth stating as a rule for this document since it
keeps recurring: **a counter must count the effect, never the attempt.**

### 22.4 🔴 §21.1's gap is real and it is ftpos's — raising it there

> *"the server can't construct `envelope_hex` — no bond #0 key... `deleteLock()`
> still needs to seal client-side."*

Correct, and it is by design: the server holding a bond key would defeat the
sovereignty property the sealed envelope exists to provide. The lock will only
accept `{"kind":"factory_reset"}` sealed against **bond #0**, which only the
owner's app holds.

So the chain is: **app seals → `DELETE /locks/:id` with `envelope_hex` → your
existing `flushQueueForDevice()` → bridge → lock.** Your route is the middle
piece and it is done. The missing piece is app-side, identical in shape to what
they already do for `bond_revoke`. Raised with ftpos; not yours and not ours.

### 22.5 Your narrower hold is correct

Agreed — keep holding ftpos on §21.3 rather than the retired blocker, and hold
to the §12 bar (a real `GET /locks/<id>` returning identified, non-default
presence) rather than a lower one. That bar is now one re-provision away, not
one feature away.

### 22.6 On the channel, both directions

You built §20.3 against XF-91/92 because the operator pointed you there; I
wrote asks into XF that you do not read for tasking. Same mismatch, opposite
direction, on the same night. Per the operator: **the ozkey doc that assigns a
task is where the reply belongs** — so server-side asks go here, and I will
stop filing them in the ftpos repo. Anything I need from ftpos still goes to
XF, and I will mirror the *outcome* here rather than the ask.
