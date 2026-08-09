# ozkey-19 — OZKIE reliable delivery: acknowledgement, retransmission, and
    why the uplink was built wrong

**Status: DESIGN, approved to write 2026-08-10 by the operator.**
Supersedes ozkey-17 U1's delivery model. The transport, sealing, routing
and semantics from ozkey-17 all stand — only the *delivery guarantee* is
replaced.

---

## 1. The problem, stated correctly

The lock→app uplink (ozkey-17 U1) carries **state changes** —
`roster_changed` today, query responses and door events tomorrow — over a
channel that loses packets, with **no acknowledgement of any kind at any
layer**.

That is not an unreliable protocol. It is an **incomplete** one. A protocol
that transmits state and cannot determine whether the state arrived has no
delivery semantics to reason about, and no amount of retry tuning creates
them — retries only move the loss probability around.

### What was built, and why it was the wrong technique

U1 sends **blind redundancy**: three bursts × three destinations = nine
copies of the same datagram in ~220 ms, then forgets it ever happened.

Blind redundancy is a legitimate technique in exactly one situation: **when
there is no back channel.** Broadcast, one-way telemetry, a probe with no
return path. Under those constraints, sending N copies and hoping is the
best available answer.

**We have a back channel.** The link is bidirectional and — critically —
*one direction is already reliable* (§3). The feedback path was available
and unused. The correct technique has been standard since the 1960s: **ARQ
— acknowledge, time out, retransmit.**

Measured cost of the wrong choice, on this bench, 2026-08-09/10:

| | |
|---|---|
| Uplink messages attempted | 12 |
| Delivered | 9 |
| **Silently lost** | **3** |
| Datagrams spent per delivery | ~9 |
| Observed user-visible failure | banoi1 stuck showing *"waiting for QR scan"* for a member who had already enrolled and was opening the door |

That last row is the one that matters. It is not a statistic — it is an
admin phone displaying the opposite of reality, with no mechanism to
discover it for up to 20 minutes.

### The irony, recorded so it is not repeated

In `XFtposDecisions-84.md` §14 I told the app team:

> *"Only the lock's own word may move a row to Confirmed. Nothing the
> server or bridge says is evidence about lock state; they are couriers,
> and a courier confirming it posted your letter is not the recipient
> confirming they read it."*

That argument is correct, and I made it about **their** state machine on
the same night I shipped an uplink in which **nothing confirms anything, at
any layer.** The principle was right there and I applied it only outward.

---

## 2. Design principles

1. **Every uplink message is acknowledged end-to-end, by the endpoint.**
   Not by the bridge, not by the server, not by the broker. §1's courier
   rule applies to us.
2. **The lock retransmits until acknowledged**, on a backoff, with a
   bounded give-up that is *surfaced* rather than silent.
3. **Unacknowledged messages survive reboot.** A brownout mid-retransmit
   must not lose a state change. This hardware browns out — see
   `doorlock-brownout-suspicion`.
4. **Delivery semantics belong to the transport layer, not to each verb.**
   `roster_changed`, query responses, door events and battery alarms all
   inherit one mechanism rather than each re-deriving it.
5. **Backoff, not bursts.** The failure mode we measured was partly
   self-inflicted: nine datagrams in 220 ms saturates the very radio the
   delivery depends on.

---

## 3. The asymmetry that makes this cheap

This is the observation the whole design rests on.

| Direction | Path | Reliability |
|---|---|---|
| **app → lock** (downlink) | MQTT/TCP → `pending_queue` (7-day expiry, retry on wake) → bridge → Thread | **Already reliable.** Built, tested, in production use. |
| **lock → app** (uplink) | Thread UDP → bridge → MQTT | **Lossy.** No ACK, no retransmit, no persistence. |

So the ACK travels **the direction that already works**. We are not
building bidirectional reliability — we are adding a return receipt to a
lossy path using a delivery mechanism that already exists and is proven.

This also means the ACK is *free* in the sense that matters: no new
transport, no new server route (§6), no new failure mode on the reliable
side.

---

## 4. The mechanism

### 4.1 Message identity

Every uplink carries a monotonically increasing `msg_id`, **distinct from
the envelope counter**:

- the **envelope counter** (U0) is anti-replay — it must advance on every
  transmission, including retransmissions, or the app's replay check
  rejects the retry as stale.
- the **`msg_id`** identifies the *logical message* and is **stable across
  retransmissions** — it is what the ACK names.

Getting these confused is the obvious trap: if retransmissions reused the
counter they would be rejected as replays; if `msg_id` changed per attempt
the app could not deduplicate.

`msg_id` lives **inside the seal** (ozkey-17 §8's decision): a correlator in
the clear would let a broker observer link "lock reported something" to
"admin acknowledged something" and infer administrative rhythm per door
without decrypting anything. The app can decrypt; no middle hop needs it.

### 4.2 Wire shape

Uplink, sealed plaintext:

```json
{"kind":"roster_changed","msg_id":41,"reason":"member_enrolled","bonds":2}
```

ACK, sealed plaintext on the **downlink**, same envelope machinery as every
other command:

```json
{"kind":"ack","msg_id":41}
```

`ack` becomes a new `kind` in the ozkey-17 §6c allow-list, handled in-lock
and never forwarded to the MCU — same class as `bond_revoke` /
`invite_cancel`.

### 4.3 Lock-side state

Per pending message, persisted:

| Field | Purpose |
|---|---|
| `msg_id` | identity, stable across retransmits |
| `payload` | the plaintext to re-seal (re-sealed per attempt for a fresh counter) |
| `target_slot` | which bond it is addressed to |
| `attempts` | for backoff and give-up |
| `next_at` | when to retransmit |
| `created_at` | for the give-up deadline |

**One pending message per (bond, kind) is sufficient** for
`roster_changed`: it is idempotent state ("something changed, resync"), so
a newer one supersedes an older unACKed one rather than queueing behind it.
Query responses are *not* idempotent and need their own slot keyed by the
request's `msg_id`.

Storage: `txlogAppend()`'s LittleFS ring buffer already exists, is durable,
survives reboot, and is currently used only for offline event capture.
Extending it — or adding a small sibling — is preferable to inventing a
second persistence mechanism.

### 4.4 Retransmission schedule

```
attempt 1   immediately
attempt 2   +2 s     ± jitter
attempt 3   +8 s     ± jitter
attempt 4   +30 s    ± jitter
attempt 5   +2 min   ± jitter
attempt 6   +10 min  ± jitter
give up     at 1 hour, or 6 attempts
```

Jitter is not decoration: several locks reacting to one mesh event would
otherwise retransmit in lockstep and collide with each other repeatedly —
the classic synchronised-retry failure.

**One datagram per attempt, not nine.** Unicast to the peer's ML-EID when
known (link-layer ACKs and MAC retries come free at that layer); multicast
to `ff03::1` only when no peer is known yet. Never `ff03::4f5a` alone —
see ozkey-17 §6d, it has never delivered a packet on this mesh.

Compare: current design spends **9 datagrams in 220 ms and then forgets**.
This spends **1 datagram, then 1 more if needed**, and does not stop caring
until it knows.

### 4.5 Give-up must be loud

On give-up the lock records the failure in its txlog and **includes a
"pending notifications" count in its heartbeat**. That makes an undelivered
state change visible to the server as operational metadata — which is
permitted under ozkey-17 §6a (a count is metadata, its content is not) —
so a lock that has been shouting into a void for an hour is discoverable
rather than silent.

Silence on failure is what produced tonight's entire investigation.

---

## 5. What this makes possible

- **ftpos can delete the backstop poll legitimately.** Today's 20-minute
  backstop (XF-83) is load-bearing precisely because delivery is not
  guaranteed. With ARQ it becomes a defence-in-depth measure rather than
  the actual mechanism.
- **Every future uplink inherits delivery.** Query responses (Q1), door
  events once C7 seals them, battery alarms, `bonds_listed`. None of them
  need to re-solve this.
- **"Sent" becomes a claim we can defend.** The `[UPLINK] … queued` log
  line currently means "handed to lwIP," which is why I misread the
  system's health for hours. With ACK the lock knows, and can say so.

---

## 6. Work breakdown

| # | Where | Task |
|---|---|---|
| R1 | `ozdoorlock_core.h` | `msg_id` on uplink messages; stable across retransmit, distinct from the envelope counter |
| R2 | `ozdoorlock_core.h` | Pending-message store, persisted (extend `txlog` or a sibling); survives reboot |
| R3 | `ozdoorlock_core.h` | Retransmit scheduler with backoff + jitter; **one datagram per attempt** |
| R4 | `ozdoorlock_core.h` | `{"kind":"ack","msg_id":N}` handler — in-lock, clears the pending entry |
| R5 | `ozdoorlock_core.h` | Give-up path: txlog entry + pending count in the heartbeat |
| R6 | ftpos app | Send the ACK on receipt of any uplink, **before** acting on the payload — an ACK means "I have it," not "I processed it" |
| R7 | — | **No server work.** The ACK is an ordinary sealed downlink on the existing `bond-revoke`-shaped path. Confirm rather than assume. |
| R8 | bench | Measure delivery over ≥30 cycles. Target: 100% delivered or a loud give-up. Nothing less counts as done. |

### Ordering

R1–R5 are firmware and can land before R6, because an unACKed message
simply retransmits until give-up — the current behaviour, but noisier and
visible. So **the firmware half is independently shippable** and improves
observability even alone.

---

## 7. What this does NOT change

Sealing, key derivation, the `kind` contract, routing, the metadata/content
split, the bridge's relay role, `pending_queue` — all of ozkey-17 stands.
This adds a delivery guarantee *underneath* the semantics; it does not
alter them.

---

## 8. Acceptance

Not "it worked on the bench." Specifically:

1. **≥30 roster changes**, every one either ACKed or a logged give-up.
   Zero silent losses.
2. **Bridge powered off mid-retransmit** → message still delivered when it
   returns, within the backoff window.
3. **Lock power-cycled with a message pending** → retransmission resumes
   after boot. This is the one that proves R2.
4. **Duplicate suppression**: a retransmitted message that the app already
   received is discarded by `msg_id`, and the app ACKs it again rather than
   ignoring it — a lost ACK must not become a permanent retransmit loop.
5. Delivery latency under normal conditions **no worse than today's**
   (~200–500 ms), since attempt 1 is still immediate.

Item 3 is the one most likely to be skipped and the one that matters most
in the field, on hardware with a known brownout problem.

---

## Server reply (R7), 2026-08-10 — confirmed, one labeling nuance flagged

Checked the actual code rather than taking "confirm rather than assume" as
rhetorical. `pending_queue`/`flushQueueForDevice`/the MQTT publish path
(`server.js` ~550-603) never inspects `envelope_hex` — it stores and
relays whatever it's given, keyed only on `device_id`/`expires_at`/
`msg_type`. `action_type` is a label used in two places: the log line
text, and one status-update branch (`job.action_type === 'revoke-key' ?
'revoked' : 'synced'`) that only fires for grant-linked jobs
(`grant_id` non-null) — an ACK would have `grant_id NULL`, same as
bond-revoke/invite-cancel/unlock today, so that branch never runs for it
either. **R7 holds: zero server code changes needed.** The app can send
an ACK's `envelope_hex` through any existing sealed-envelope route
(`bond-revoke`, `invite-cancel`, `unlock`) and it reaches the lock
identically to what's already live-verified for S10/S11.

**One nuance, not a blocker, worth a decision rather than silent
adoption:** literally calling `POST /locks/:id/bond-revoke` to send an ACK
works mechanically but logs `"Bond revoke queued for..."` and an
`audit_log` row with `action='bond-revoke'` for what's actually routine,
potentially-frequent ACK traffic — not a correctness bug, but a real
audit-trail quality problem given ozkey-18's own directive that this log
is relied on for OZPMS/OZLODGE compliance. Two options, both truly zero
new delivery machinery:
1. Accept the mislabeling — ACKs are infrequent enough (one per uplink
   message) that it may not be worth the naming friction.
2. A trivial one-line addition — `api.post('/locks/:id/ack', (req, res) =>
   handleBondVerb(req, res, 'ack', 'ACK'))` — same handler, same
   mechanism, just a route/label that reads correctly in the log and audit
   trail. This is genuinely one line, not new design, so it doesn't
   contradict "no server work" in spirit even if it's technically a diff.

Not doing either unprompted since R7 explicitly scoped this as no server
work — flagging the choice rather than picking one silently.
