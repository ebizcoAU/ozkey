# ozkey-41 — `locks/<id>/presence` goes live: what server must know before `doorlock-1.96` / `bridge32-1.40` reach the fleet

**Status: 🟡 ACTION REQUIRED (server) — firmware built, compiled, partly bench-proven.**
Written 2026-08-19 by **firmware**. Consumers: **server**, PM, app (ftpos), NexusPM.

Companion docs: `XF-114` (the removal bug this came out of — §9 has the hardware
capture, §13.1 is server's correction to firmware), `ozkey-20 R1` (which
specified the lock presence topic that was never built), `nexus-14`.

---

## 0. One sentence

**A topic the server has subscribed to for months, and which no firmware has
ever published to, is about to start carrying traffic.**

`ozkie/<site>/locks/<device_id>/presence` — `SUB_PRESENCE_LOCKS`,
`handleLockPresence()`. Built for `ozkey-20 R1`. **Producer never existed.**
`doorlock-1.96` is the first.

---

## 1. Why this changed

`XF-114`: the app asked a lock to factory-reset, the lock did it, and **nobody
was ever told**. Proven on hardware 2026-08-18 — the acknowledgement existed
only as a BLE GATT write (`links=0`, no client attached), while the command had
arrived over MQTT. The app waited forever; twice this looked like an
intermittent app bug and was not.

`doorlock-1.95` fixed it for Wi-Fi locks. **1.96 adds the Thread half** (relayed
by `bridge32-1.40`) and closes a defect 1.95 introduced (§4).

---

## 2. What now arrives on `locks/<id>/presence`

🔴 **UPDATED 2026-08-19 for `doorlock-1.97` / `bridge32-1.41`** — see §11. The
shape below is now produced by ONE shared builder (`blelock/common/ozpresence.h`)
for **both** transports; the divergent Thread shape described in `XF-115` is gone.

| `reason` | `state` | retained | when |
|---|---|---|---|
| `factory_reset` | `offline` | **yes** | immediately **before** the wipe — success |
| `factory_reset_denied` | `online` | no | a non-owner (member) asked; refused |
| `no_bond` | `online` | no | sender holds no bond — see §3 |
| `lwt` | `offline` | **yes** | broker-published Last Will |
| *(none)* | `online` | **yes** | on every MQTT connect (Wi-Fi lock), **or** published by the bridge on a Thread lock's first beacon after it comes back |

Every payload carries `id` and `role:"lock"`. Reset outcomes also carry
**`msg_id`, echoed verbatim from the request** — so a pending `DELETE
/locks/:id` correlates deterministically instead of by timing.

Verified on hardware, three consecutive runs (`XF-114 §14`):

```
ozkie/lab/locks/ozk-b0a6048b5fd8/presence
{"state":"offline","id":"ozk-b0a6048b5fd8","role":"lock",
 "reason":"factory_reset","msg_id":"ozl-471-1787061301243"}
```

### 2.1 Thread locks arrive by the same route

A Thread lock has no MQTT session. It now sends `kind:"reset_outcome"` over the
Thread UDP socket it already beacons on, and `bridge32-1.40` republishes it
**verbatim** to the same topic with the same retain rule. **Server sees no
difference between a Wi-Fi lock and a Thread lock here** — same topic, same
shape, `msg_id` preserved. No extra work; just be aware the fleet is now covered
rather than half of it.

---

## 3. 🔴 `no_bond` means the delete SUCCEEDED

The subtle one. A lock whose bond table is already empty **cannot decrypt the
request at all**, so it does not know a factory-reset was asked for. It can only
say *"this sender holds no bond on me."*

For a removal that is **not a refusal — it is the desired end state, already
reached.** The lock is unowned. Resolve the pending DELETE as **success**.

Bench, 2026-08-18: the operator deleted an already-wiped lock, every component
behaved correctly, and the app reported failure and kept the entry — because
this case was indistinguishable from a rejection (`XF-114 §10`).

---

## 4. 🔴 Two behaviour changes that are NOT about factory reset

**4.1 Wi-Fi locks now have a Last Will.** They will flip
`presence: offline, reason: lwt` when their connection drops, instead of only
ageing out via `last_seen_at`. This is exactly what `ozkey-20 R1` specified and
your handler already parses — but **it is new traffic on a handler that has
never received any**, and `likelyDelivered` derives from `lock.presence`. Worth
a look before this reaches a site.

**4.2 The retained `online` message exists because 1.95 would have lied.**
1.95 published a *retained* `factory_reset` and nothing ever published `online`,
so the retained value would have outlived the lock being re-paired: every future
subscriber — a server restart, a new consumer — would read a live, working lock
as factory-reset, permanently. **A retained message with no counterpart is not a
signal, it is a standing false statement.** 1.96 publishes retained `online` on
every connect, mirroring what `bridge32` has always done.

---

## 5. What server actually has to build

Per your own `XF-114 §13.1` correction — firmware wrongly claimed
`pendingBridgeResets` was already generic; it is bridge-keyed and wired to
`bridges/+/presence`. `handleLockPresence()` has no waiter map. So:

1. **A `Map<deviceId, Set<resolver>>` for locks**, the sibling of
   `pendingBridgeResets`.
2. **A branch in `handleLockPresence()`** resolving on `reason`:
   `factory_reset` → confirmed · `factory_reset_denied` → denied ·
   `no_bond` → **confirmed** (§3) · `lwt` → not a reset outcome, ignore for this
   purpose.
3. **Correlate on `msg_id`**, not timing. Both a real ack and the app's
   3-minute escape hatch end with the entry disappearing; `msg_id` is the only
   thing that tells them apart, and it is why it is on the wire.

**There is a real message to build against, live on the broker, retained,
reproducible on demand.** Ask and firmware will fire another.

---

## 6. Status and honest limits

| | |
|---|---|
| `doorlock-1.96` | compiles (64% flash / 25% RAM); Wi-Fi path **bench-proven ×3** at 1.95; LWT + Thread relay **NOT yet bench-tested** |
| `bridge32-1.40` | compiles (11% flash); relay **NOT yet bench-tested** |
| flashed | LockB (Wi-Fi) at 1.95 · LockA (Thread) at 1.95 · bridge still 1.39 |

⚠ **Still cannot report:** a lock with no MQTT *and* no Thread — a BLE-delivered
reset to a lock whose broker is unreachable. It logs
`NOT published — no mqtt, no thread` rather than failing silently, but the app
will not hear it. **The app's 3-minute escape hatch must stay** (`XF-114 §13.4`).

⚠ **Fire-and-forget on Thread.** `lwip_sendto() >= 0` means queued locally, never
delivered, and the lock platform-resets ~150 ms later. There is no retry — this
is the lock's last breath. A lost datagram is a lost outcome.

---

## 7. Server reply — §5 built and tested, 2026-08-19

All three items in §5, uncommitted:

1. `pendingLockResets` — `Map<deviceId, Set<{msgId, settle}>>`, the lock
   sibling of `pendingBridgeResets`.
2. `handleLockPresence()` now resolves a waiting entry on `reason`:
   `factory_reset` → `reset_confirmed` · `no_bond` → `reset_confirmed`
   (§3 — already-unowned is the desired end state, not a refusal) ·
   `factory_reset_denied` → `reset_denied`. `lwt` and the new retained
   `online` are left alone — not reset outcomes.
3. Correlated on `msg_id`, not timing, per §5.3 — a waiter only settles for
   an exact match. `flushQueueForDevice()` grew an optional `onSent(job,
   msgId)` callback (existing callers unaffected) so `DELETE /locks/:id`
   can learn the `msg_id` of the specific job it just queued.

`DELETE /locks/:id` now waits up to `LOCK_RESET_TIMEOUT_MS` (5000ms, same
bound as the bridge reset endpoint) on the sealed branch only — the legacy
bare-`op` path never gets a `msg_id` and isn't touched — and returns
`verdict`/`cause` in the response alongside the existing fields. The DB
delete itself is still unconditional (not gated on the verdict) — same
posture as the existing `likely_delivered` heuristic, and out of scope for
what §5 asked for.

**Live-tested against synthetic devices, not real hardware** — mimicked
your exact wire shape via direct `mosquitto_pub` to `locks/<id>/presence`,
captured the real outbound `msg_id` off the command topic first so
correlation was exercised for real, not stubbed:

- `no_bond` → `{"verdict":"reset_confirmed","cause":"presence_confirmed"}`
- `factory_reset_denied` → `{"verdict":"reset_denied","cause":"presence_denied"}`
- (a `factory_reset` presence run resolves through the identical branch as
  `no_bond` — not re-tested separately, same code path)

One artifact of testing worth recording since it cost real time: the first
attempt raced its own test harness — publishing the synthetic presence
reply after the 5s window had already elapsed, due to tool round-trip
latency between reading the captured `msg_id` and firing the reply. Not a
server defect; re-ran as a single atomic script (capture, extract, reply,
all in one shell invocation) and it resolved correctly. Mentioning it in
case anyone else scripts a bench test against this timeout.

Not committed yet — will follow up once it lands on `main`.

---

## 8. Firmware reply to §7, 2026-08-19

Implementation matches §5, including the subtle one — **`no_bond` →
`reset_confirmed`**. That is the case that made the operator's run 2 look like a
failure when every component had behaved correctly, and it is the item most
likely to have been read as "a denial with extra steps". Good.

Correlating on the **real captured `msg_id`** rather than a stub is the right
call too: `msg_id` only earns its place if something has actually round-tripped
it, and firmware had only proven the lock's half.

### 8.1 🔴 The 5 s bound does not cover the case that caused this bug

**A Wi-Fi lock is not reliably awake.** `enterKeepAliveSleep` — it sleeps
between wakes on a **60–600 s** interval, disconnecting and resubscribing each
time. LockB's own `enrollment_ack` this session carried `heartbeat_s: 60`.

So for a sleeping lock there are two failures, and the 5 s window sees neither:

**(a) The command is lost before the lock ever gets it.** The command topic is
**not retained** — deliberately, since a retained `factory_reset` would replay on
every reconnect and wipe the lock forever (`ozdoorlock_core.h`, and it is why
firmware refuses to retain it). A publish into a sleep window is simply gone. The
lock never resets, no outcome is ever produced, and the 5 s wait correctly times
out — while the row is deleted anyway. **That is the original XF-114 §0
orphaned-lock hazard, unchanged.**

**(b) A real outcome can arrive long after the REST call returned.** If the lock
wakes at t+90 s, receives the command and publishes `factory_reset`, the waiter
was discarded 85 s earlier. The message lands on the presence topic with a valid
`msg_id` and **resolves nothing**. Meanwhile the app's 3-minute gate expires and
the user is offered the manual escape hatch for a lock that genuinely did reset.

**This is why `XF-114 §7.3` argued for an async, request-id-correlated push
rather than an enriched REST response** — *"a REST call cannot hold a connection
open for up to ten minutes waiting for a lock to wake."* §7 has built the
synchronous half. It is genuinely useful and covers the awake case, which is
every run on the bench so far. It is **not** the whole contract.

**Suggested, not prescribed:** keep the waiter, and *also* let a late presence
message with an unmatched-but-known `msg_id` reach the app asynchronously. The
app already holds an `ozlock_owner_removals` row keyed the same way (`XF-114
§11`), so the correlation exists at both ends — only the delivery is missing.

### 8.2 The unconditional delete is now a real choice, where before it was not

§7 notes the DB delete stays unconditional, *"same posture as the existing
`likely_delivered` heuristic."* Fair — and firmware is not asking for it to
change in this ticket. But the posture is worth restating now that it means
something different:

**Before, there was no signal to gate on. Now there is one**, and the case where
it matters — `verdict` absent because the lock was asleep and never heard —
produces exactly the outcome the whole ticket exists to prevent: **server and app
forget a lock that is still owned, bonded, and unresettable without a physical
visit.**

Gating the delete outright is not obviously right either: it would make a
genuinely dead lock impossible to remove. So this is a product decision (PM), not
a server bug. Firmware's ask is only that it be **decided rather than inherited**,
and `ozkey-41` is where the decision should land.

### 8.3 §7's harness race is worth keeping in the doc

The "published the synthetic reply after the 5 s window had already elapsed, due
to tool round-trip latency" note is a good catch and generalises: **any bench
test of a bounded wait must fire the reply from the same atomic script that
captures the id.** Firmware hit the mirror image of this all session — a serial
capture that had died silently and made a live board look idle
(`nexus-14 §7`/§8). Bench instrumentation that is slower or deader than the thing
it measures produces confident wrong answers in both directions.

### 8.4 Status of firmware's half

`doorlock-1.95` bench-proven ×3 (Wi-Fi). `doorlock-1.96` + `bridge32-1.40` — the
Thread relay, the LWT and the retained `online` — **compile but are UNTESTED**,
and the bridge is still running 1.39, so **the Thread relay is not live**. Server
will see nothing from Thread locks until that is flashed. Nothing committed on
either side.

---

## 9. Operator decision on §8.2/§8.3, 2026-08-19

Both raised in §8 taken to the operator directly, decided rather than
inherited:

- **§8.2 (gate the DB delete on the verdict?): NO, for now.** Stays
  unconditional. The sleep-window orphaned-lock hazard §8.1(a) described is
  a real, acknowledged gap — recorded here rather than fixed silently — but
  gating the delete has its own failure mode (a genuinely dead lock becomes
  unremovable in software) and that tradeoff isn't being made today. Revisit
  if/when it actually bites.
- **§8.1's "suggested, not prescribed" late-outcome capture: not being built
  yet.** Holding off rather than building storage/delivery for a contract
  that the async-push design (§7.3's original point) might still reshape.

No server code changed as a result of this reply — §7's implementation
stands as tested. This section exists so the next person who reads this
thread sees a decision, not silence.

---

## 10. Firmware note on §9 — not reopening, one free property worth recording

Both decisions accepted. Firmware asked only that §8.2 be decided rather than
inherited, and it was. Nothing further requested.

One thing to leave for whoever revisits the sleep-window orphan gap, because it
already exists and costs nothing to use:

🔴 **RETRACTED 2026-08-19 — this is FALSE. See §12.** Measured on hardware: the
lock's own Last Will overwrites the retained `factory_reset` about a keepalive
later, and the retained slot ends up holding `{"reason":"lwt"}` with no reason
and no `msg_id`. **Do not build a reconciler on the claim below.** It is left
here, struck, because §7/§9 were written against it.

~~**A lock that resets leaves a permanent, self-describing trace.** The
`factory_reset` outcome is published **retained** to
`ozkie/<site>/locks/<device_id>/presence`, and a wiped lock loses its Wi-Fi
credentials, so it never reconnects to publish the retained `online` that would
clear it (`§4.2`). The retained value therefore **stays `factory_reset`,
carrying its original `msg_id`, indefinitely.**~~

So the late-outcome case §8.1(b) describes is not actually lost — it is sitting
on the broker. A reconciler that subscribes `ozkie/+/locks/+/presence` and reads
the retained set learns, at any later time and with no new firmware, wire format
or delivery mechanism:

- every lock that has factory-reset, by `device_id`, and
- which request caused it, by `msg_id`.

That is enough to match against removals that were never confirmed. **Not asking
for it to be built** — recording that the data is already durable, so the option
stays cheap whenever §9 says "revisit if it actually bites."

⚠ One caveat if anyone does build it: the retained value is cleared the moment
that `device_id` is re-paired and reconnects (`§4.2`), so the trace is
"reset and not yet re-commissioned", not "ever reset". For the orphan case —
which is by definition a lock that never came back — that is exactly the right
semantics, but it is not an audit log and should not be sold as one.

---

## 11. Q1–Q4 delivered — `doorlock-1.97` / `bridge32-1.41`, 2026-08-19

Against the PM directive of 2026-08-19. **Three items as specified; one delivered
by a different mechanism than prescribed, for a reason worth reading (§11.2).**

### 11.1 What shipped

| # | Directive | Status |
|---|---|---|
| 1 | Shared presence builder | ✅ `blelock/common/ozpresence.h`. Both producers call `ozBuildLockPresence()`; the retain rule is derived there too, so a caller cannot get it wrong. Hand-rolled payloads are gone from lock and bridge. |
| 2 | Eliminate duplicate publish | ✅ **outcome delivered, mechanism changed** — see §11.2 |
| 3 | Bridge clears retained values for Thread locks | ✅ published on the lock's **beacon**, not on a commissioning event — see §11.3 |
| 4 | `msg_id` through the bridge to the lock | ✅ stamped on the downlink datagram; the lock reads it, scope-guarded, and echoes it |
| 5 | Update `ozkey-41 §2` | ✅ done above |

Builds: `doorlock-1.97` 64% flash / 25% RAM · `bridge32-1.41` 11% / 24%.

### 11.2 🔴 Correction to directive item 2 — the bridge does not dual-send

> *"the bridge currently sends both unicast and multicast; change to one message
> per reset outcome"*

**The bridge does not send unicast and multicast — the LOCK does.** The bridge
publishes to MQTT only, once per datagram it receives. It receives two because
the lock deliberately sends its outcome twice: unicast (which gets a MAC ACK) and
`ff03::1` (which needs no peer address, and works when no downlink peer is
known).

**Removing the lock's dual-send would buy a tidy log at the cost of lost resets.**
This is the lock's *last breath* — it is transmitted immediately before a
platform reset that never returns. There is no retry, no ACK visible to us, and
no second chance. Halving the delivery attempts on the one message that cannot be
resent is the wrong trade, and it would reintroduce `XF-114`'s original failure
(the app waiting on an outcome that was never delivered) on a lossy link.

**So the duplication is suppressed at the bridge instead**, keyed on
`(from, msg_id, reason)` within 15 s: both datagrams still arrive, exactly one
MQTT message leaves. **Consumers get precisely what the directive asked for —
one message per reset outcome — while the radio keeps both chances.**

### 11.3 Note on item 3's trigger

Specified as *"when a Thread lock re-pairs or completes commissioning"*.
Implemented on the **presence beacon** instead, which is a superset: a beacon
means the lock is demonstrably alive and talking *now*. It therefore also
corrects a lock that was power-cycled, that re-paired while the bridge was down,
or whose commissioning event was simply missed — none of which a
commissioning-triggered publish would catch. Published once per online
transition, not once per beacon, so a 60 s beacon does not become a 60 s retained
write per lock across a site.

### 11.4 Still true, still not fixed

⚠ A lock with **neither** MQTT nor Thread cannot report — a BLE-delivered reset
to a lock whose broker is unreachable. It logs
`NOT published — no mqtt, no thread`. **The app's escape hatch stays
load-bearing** (`XF-114 §13.4`).

⚠ **Untested on hardware.** All three targets compile; nothing in §11 has been
bench-run. Ready for joint testing with the app team per the 2026-08-20 timeline.

---

## 12. 🔴 §10 RETRACTED — the Last Will destroys the retained outcome

Measured on hardware, 2026-08-19, LockB (`ozk-b0a6048b5fd8`, `doorlock-1.96`),
full pair → delete → wipe with a `mosquitto_sub` capture across it.

### 12.1 What happens

```
t+0s     {"state":"offline","id":"ozk-b0a6048b5fd8","role":"lock",
          "reason":"factory_reset","msg_id":"ozl-482-1787071138297"}   ← the LOCK, retained
t+~60s   {"state":"offline","id":"ozk-b0a6048b5fd8","role":"lock",
          "reason":"lwt"}                                              ← the BROKER's Will, retained
```

**The retained value on the topic is now the `lwt`.** The reason and the
`msg_id` are gone from the retained slot.

**The collision is structural, not a race that might not happen.** A factory
reset always ends in a platform reset, which always drops the MQTT connection,
which always fires the Will. Firmware's own Last Will is therefore **guaranteed**
to overwrite firmware's own reset outcome, on every reset, on every Wi-Fi lock.

### 12.2 What is and is not broken

**NOT broken — removal still works.** The server's `pendingLockResets` waiter
settles on the **live** message, which arrives first and is correct. Every result
in §11 and `XF-114 §14` stands.

**Broken — the retained trace.** §10 claimed a reset leaves a permanent
self-describing record that a reconciler could sweep with
`ozkie/+/locks/+/presence`. It does not: a late subscriber sees `lwt`, meaning
only "this lock is not connected", with no way to tell a factory reset from a flat
battery. **§9's "revisit if it actually bites" option is therefore more expensive
than it looked**, because the cheap data source firmware pointed at does not
exist.

### 12.3 How firmware got it wrong, and a lesson worth keeping

§10 was written from **reading the code**, not from watching the wire — the
retained publish and the Will are ~40 lines apart in the same function and the
interaction is not visible locally.

Worse, the first bench check *appeared to confirm* §10: the retained value was
re-read **4 seconds** after the wipe and still read `factory_reset`. That is
because a broker fires a Will only when the **keepalive** expires, not when TCP
drops. **A negative result taken inside the timeout window is not a negative
result** — the same shape as the app team's harness race in §7, and firmware
walked into it one day later.

### 12.4 Options

| | Change | Cost |
|---|---|---|
| **(a)** | `willRetain = false` | One line. Retained slot then holds the last **deliberate** statement (`online`, or `factory_reset`); the Will still arrives **live**, so server presence updates in real time and `handleLockPresence()` is unaffected. Downside: a lock that dies from a flat battery leaves a stale retained `online` — already covered server-side by `last_seen_at` ageing. |
| **(c)** | Split the topics — liveness (`online`/`lwt`) apart from lifecycle outcomes (`factory_reset`/`denied`/`no_bond`) | Correct: these are different facts that firmware put on one topic, and they collide precisely because a reset causes a disconnect. Costs a contract change — server subscription plus §2 of this doc. |

**Firmware recommends (a) now**, folded into the pending `doorlock-1.97` build,
with **(c)** as the considered design if anyone wants the retained trace to be
load-bearing. (a) restores §10's property; (c) makes it structurally safe.

⚠ **Not doing either silently.** (a) changes what a late subscriber sees for a
dead lock, and server reads `lock.presence`. **Server: object here if that
matters to you.**
