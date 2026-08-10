# ozkey-19 (v2) — The uplink was multicast. Thread's own retries were switched off.

**Status: REWRITTEN 2026-08-10. Supersedes ozkey-19 v1 (commit `b2a22ba`)
in full.**

**🔴 Server team: the R7 work you replied on is CANCELLED. Stop. Details in
§6.2 — your analysis was correct and is preserved in Appendix B; the
feature it supported is no longer being built.**

v1 specified a complete application-layer ARQ — queue, `msg_id`, backoff
ladder, app-side ACK. **That was over-engineering.** The operator called it,
and he was right. Thread already provides retransmission; we had disabled it
by sending the uplink to multicast addresses. This document replaces a
~200-line protocol design with a six-line firmware fix.

---

## 0. Standing instruction

ozkey-18 §0 applies, and this document is the strongest argument for it yet.
**Verify before building.** v1 was reviewed by the server team, who correctly
confirmed its server-side implications — and none of us checked whether the
transport already solved the problem. Confirming a design is internally
consistent is not the same as confirming it is necessary.

---

## 1. The actual defect

`blelock/common/ozdoorlock_core.h:1803`:

```c
// One burst = unicast (if known) + both multicast groups.
static bool ozUplinkBurst(const String &payload) {
  bool anySent = false;
  if (g_haveDownlinkPeer &&
      ozThreadUdpSendOnce(payload, (const uint8_t *)&g_lastDownlinkPeer.sin6_addr,
                          "unicast-peer"))
    anySent = true;
  if (ozThreadUdpSendOnce(payload, OZ_ALLNODES_BYTES, "ff03::1")) anySent = true;
  if (ozThreadUdpSendOnce(payload, OZ_THREAD_GROUP_BYTES, "ff03::4f5a")) anySent = true;
  return anySent;
}
```

Three destinations per burst, three bursts (`OZ_UPLINK_TRIES 3`, ~40 ms
apart + jitter) = the nine datagrams in ~220 ms.

Of those nine:

| Destination | Copies | Link-layer reliability |
|---|---|---|
| `ff03::4f5a` | 3 | **none** — and the group has never delivered a packet, 0 of 18 |
| `ff03::1` | 3 | **none** — multicast frames are never acknowledged |
| `unicast-peer` | 3, **only if a downlink has been seen** | full 802.15.4 ARQ |

**Six of nine copies go to a transport mode with no acknowledgement and no
retries. Three go to an address that does not work.** The only copies that
get the radio's own retransmission are conditional on `g_haveDownlinkPeer`.

### 1.1 The comment that already said this

`ozdoorlock_core.h:1788`, written during the v1 investigation:

> *"Note `lwip_sendto()` returning >= 0 means QUEUED LOCALLY, never
> delivered — there is no ACK at this layer for multicast, and for unicast
> the MAC ACK is invisible from here."*

It was documented and the implication was not followed. I then designed an
application-layer ARQ to replace retries the radio was already capable of.

### 1.2 `g_haveDownlinkPeer` is RAM-only — a second, concrete bug

The peer address is learned from an inbound downlink and never persisted. A
lock that reboots has no unicast peer until the bridge next speaks to it, so
**a freshly-booted lock sends pure multicast** — zero reliability.

This is exactly the case v1 §8 flagged as most likely to be skipped in
testing and most likely to matter in the field, on hardware with an open
brownout suspicion.

---

## 2. What Thread actually guarantees

*Verified against the Thread/OpenThread documentation, 2026-08-10. Sources
at the end.*

**Layer 1 — 802.15.4 MAC, per hop.** Every *unicast* frame requests an ACK.
No ACK → the MAC retransmits, up to `macMaxFrameRetries` (3 in Thread 1.1.1;
raised to 15 in later revisions specifically to fix throughput/reliability
problems of this kind). Multi-hop paths get this independently on every hop.

**Multicast is the documented exception.** Broadcast frames carry no
acknowledgement. Silicon Labs is explicit that even with L2 retries,
reliable multicast delivery in low-power lossy networks remains poor.

**Layer 2 — CoAP Confirmable (RFC 7252).** Thread's native application
protocol. CON messages carry ACK + exponential-backoff retransmission,
`MAX_RETRANSMIT` default 4. Thread's own management traffic uses it. This is
the end-to-end ARQ the ecosystem expects — and it is what v1 hand-rolled a
worse version of.

**We use neither.** The uplink is raw UDP via lwIP sockets, mostly to
multicast.

### 2.1 And the MQTT leg is QoS 0

Worth recording because it was the same error one layer up. `PubSubClient.h:151-154`
— none of the four `publish()` overloads takes a `qos` parameter, and
`PubSubClient.cpp:465` never sets the QoS bits. **Every publish from lock and
bridge is QoS 0.** `ozlockserv` subscribes at QoS 1 (`server.js:715`), which
achieves nothing, since QoS is bounded by the publisher.

`server.js:1326` already carried the note that *"delivered is not derivable
from MQTT (QoS 1 only confirms the…)"*. Known on the server side, never
propagated to the publish side.

---

## 3. The reframe that removes most of the work

**We are shipping idempotent state, not events.**

`roster_changed` means *"something changed, resync."* Sending it twice is
harmless. Sending a stale one is harmless. Its entire content is reproducible
by asking.

For idempotent state, **delivery guarantees are the wrong tool.** A version
number plus a way to ask gives convergence, and the push becomes a latency
optimisation rather than a correctness requirement. Reliable delivery is what
you need for events that cannot be reconstructed — and we have none of those
on the uplink today (§8).

This is why v1 was structurally wrong, not merely expensive: it applied a
messaging solution to a state-synchronisation problem.

| | v1 (ARQ) | v2 (epoch + poll) |
|---|---|---|
| Correctness depends on | every message arriving | a counter and a poll |
| Lock-side state | queue, `msg_id`, timers, NVS | one integer |
| App-side work | send ACKs, dedupe | compare an integer |
| Server work | ACK route (see §6.2) | none |
| Recovers from total push failure | no | **yes** |

---

## 4. The fix

### R1 — Unicast the uplink

Send to the bridge's mesh-local address. Multicast is demoted to **bootstrap
discovery only** — never a delivery path. This alone enables 802.15.4's
per-hop ARQ.

### R2 — Persist the peer address in NVS

Fixes §1.2. A rebooted lock must not silently drop to multicast.

### R3 — Bridge answers "who is your bridge?"

So a booted lock can learn its unicast peer without waiting for an inbound
downlink. This is the correct fix for the bootstrap problem that multicast
was papering over. Pairs with ozkey-20 R2 — the bridge is mains-powered and
already the mesh's natural warden.

### R4 — Delete the burst

`OZ_UPLINK_TRIES` × 3 destinations goes away. One unicast datagram, with the
MAC handling retransmission.

### R5 — `roster_epoch`

Monotonic counter, bumped on any roster mutation, NVS-persisted. Reported in
the heartbeat (ozkey-20 R4) and answerable via `get_status` (ozkey-20 R7).
The app compares its last-reconciled epoch and resyncs on mismatch — **with
no push having to succeed.**

### R6 — Evaluate CoAP Confirmable

Not committed, deliberately. If R1–R5 leave measurable residual loss, CoAP
CON is the standards-based answer and we should adopt it rather than
re-derive one. Decide *after* measuring, not before.

---

## 5. Ordering

R1 + R2 + R4 are one small firmware change and should be measured together —
they are the whole hypothesis. R5 is independent and cheap. R3 pairs
naturally with ozkey-20 R2. R6 is gated on the R1 measurement.

Estimated: R1–R5 is about a day, against v1's multi-week protocol build.

---

## 6. What this deletes

### 6.1 From v1

Gone: the pending queue, `msg_id`, the `msg_id`-vs-envelope-counter rule, the
2s→8s→30s→2min→10min backoff ladder, NVS persistence of queued messages, the
app-side ACK, and the give-up policy. Recorded in **Appendix A** so the
reasoning is not lost.

Surviving from v1: only the framing in §1 — that a protocol carrying state
with no acknowledgement is incomplete. That was right. The conclusion drawn
from it was wrong, because the acknowledgement already existed one layer
down.

### 6.2 🔴 To the server team — R7 is cancelled

You analysed v1's R7 correctly and in detail (Appendix B): you read
`pending_queue` / `flushQueueForDevice` / the publish path, confirmed
`envelope_hex` is never inspected, established that zero server changes were
needed, and flagged the `audit_log` mislabeling as a decision rather than
adopting it silently. That was exactly the process ozkey-18 §0 asks for.

**There is now no ACK to route, so neither option is needed. Do not add the
`/locks/:id/ack` route. Do not change the audit labels for this.**

Your queue remains correct and unchanged for downlink traffic.

What replaces it on your side is in **ozkey-20 R5/R6** — presence, health,
and fault attribution — which is real work and is where your effort should
go. Note especially ozkey-20 §2.3: `likelyOnline()` feeds a `delivered` flag
from heartbeats that Thread locks never send.

---

## 6a. RESULT — hardware-verified 2026-08-10, `doorlock-1.33` on DoorA

Admin revoke from banoi1, Thread transport, bridge live:

```
[REVOKE] bond 1 ('Gg') revoked by admin bond 0 (1/16 remain)
[ROSTER] epoch -> 1 (bond_revoked)
[UPLINK]   -> unicast   fd51:2839:8840:6499:ad0f:fec6:645e:7b4e queued 343 B
[UPLINK] unicast 1 datagram (MAC-acknowledged)
[UPLINK] bond 0 counter 391 343 B -> thread
```

| Requirement | Result |
|---|---|
| R1 unicast | ✅ no `ff03::1`, no `ff03::4f5a` on the uplink at all |
| R4 burst deleted | ✅ **1 datagram**, was 9 — `grep -c queued` = 1 |
| R5 roster epoch | ✅ `epoch -> 1`, persisted |
| R2 NVS peer | ✅ `peer restored from NVS` across **two** real reboots |

Datagram size 315 B → 343 B (the added `roster_epoch` field); still 4 6LoWPAN
fragments, so ozkey-20 §4.1's airtime table is unaffected.

**R2 is the one that mattered most and it held** — the peer was restored
*before* any uplink could fire, so a rebooted lock never drops back to
unacknowledged multicast. That was §1.2's whole concern.

### 🔴 Still open: the BRIDGE still sprays the dead group

Same capture, downlink direction:

```
port141201  [UDP] >> [ff03::4f5a] {…}   <- dead: 0 of 18 ever delivered here
port141201  [UDP] >> [ff03::1]    {…}   <- the one that works
```

`bridge32` was out of scope for R1 (which fixed lock→bridge only), so **every
downlink command still spends half its airtime on a multicast group that has
never delivered a packet on this bench.** The reasoning in §1 applies
unchanged. Track as **R7 — drop `ff03::4f5a` from bridge32's downlink** and
prefer unicast to the target lock's ML-EID, which the bridge already knows.

---

## 7. Acceptance

1. **Measure first.** Log the actual `[UPLINK]` datagram size and 6LoWPAN
   fragment count (ozkey-20 §4.1 assumes ~315 B / ~4 fragments and wants this
   confirmed).
2. ≥30 enrol/revoke cycles with R1+R2+R4, **zero silent losses**. Same bench,
   same method as the 12/9/3 v1 measurement, so the numbers are comparable.
3. **Lock power-cycled with a roster change pending** — verify it unicasts
   after reboot and does not fall back to multicast (§1.2).
4. Bridge powered off mid-sequence, then restored — lock reconverges.
5. Epoch mismatch forces a resync with the push path deliberately broken.
6. Confirm datagrams-per-delivery drops from ~9 to 1.

If (2) still shows losses, that is the signal for R6 (CoAP CON) — and the
measurement will tell us, which v1 never would have.

---

## 8. Where ARQ *is* still the right answer — later

**Door events.** *"Door opened 14:03, member X"* cannot be reconstructed from
a version number; there is no epoch that recovers a missed audit entry. That
genuinely needs delivery guarantees.

But even there the right shape is **not** ARQ — it is a durable local log
plus **sync-with-a-cursor**: the lock keeps entries, the app acknowledges up
to sequence N, the lock discards below N. Log shipping, not message
retransmission.

**This lands directly on the server team's S13 (seal the door-event log, C7).
Design for a cursor from the start** rather than inheriting this conversation
a second time.

---

## Sources

- [Thread Technology Overview — Silicon Labs / OpenThread](https://docs.silabs.com/openthread/latest/thread-fundamentals/02-thread-technology-overview)
- [UG103.11: Thread Fundamentals](https://www.silabs.com/documents/public/user-guides/ug103-11-fundamentals-thread.pdf)
- [Thread Usage of 6LoWPAN](https://www.threadgroup.org/Portals/0/documents/support/6LoWPANUsage_632_2.pdf)
- [otCoapTxParameters — OpenThread](https://openthread.io/reference/struct/ot-coap-tx-parameters)
- [OpenThread default config — `macMaxFrameRetries`](https://software-dl.ti.com/simplelink/esd/simplelink_cc13x2_sdk/2.10.00.48/exports/docs/thread/doxygen/openthread-docs-0.01.00/html/d7/d08/openthread-core-default-config_8h.html)

---

# Appendix A — v1's ARQ design, and why it is not needed

Recorded so the reasoning survives and is not re-derived.

v1 specified: a bounded NVS-backed queue of pending uplinks; a `msg_id`
stable across retransmissions while the envelope counter advanced on each
one (so the app could dedupe without rejecting retries as replays); a
retransmission ladder of 2s → 8s → 30s → 2min → 10min with jitter; give-up
at one hour; and a pending count in the heartbeat so failure was loud.

**Each piece was individually sound.** The `msg_id`/counter separation is a
real trap and the analysis of it was correct. The jitter requirement was
correct. "Give-up must be loud" was correct and **survives** — as ozkey-20 R4
`pending_uplinks`.

**Why the whole is unnecessary:**

1. **The retransmission already exists** one layer down (§2), and we had
   disabled it by using multicast. v1 rebuilt at the application layer what
   the MAC does in hardware.
2. **The payload is idempotent state** (§3). Retransmitting a `roster_changed`
   is pointless when the receiver can simply ask for current state; an epoch
   plus a poll converges even when *every* push fails, which no amount of
   ARQ achieves.
3. **Hop-by-hop reliability does not compose into end-to-end reliability**
   (Saltzer/Reed/Clark, 1984) — which is a real argument *for* an end-to-end
   check, but the cheap end-to-end check for idempotent state is the epoch,
   not a per-message ACK.

The measurement that motivated v1 — 12 attempted, 9 delivered, 3 silently
lost — stands. The diagnosis was wrong: those were not losses a retry policy
needed to cover, they were sends into a transport mode with no retries at
all.

---

# Appendix B — Server team's R7 reply (preserved verbatim, 2026-08-10)

*Superseded by §6.2 — the feature is cancelled. Retained because the analysis
is correct, the method was right, and the `pending_queue` findings remain
accurate for downlink traffic.*

> Checked the actual code rather than taking "confirm rather than assume" as
> rhetorical. `pending_queue`/`flushQueueForDevice`/the MQTT publish path
> (`server.js` ~550-603) never inspects `envelope_hex` — it stores and
> relays whatever it's given, keyed only on `device_id`/`expires_at`/
> `msg_type`. `action_type` is a label used in two places: the log line
> text, and one status-update branch (`job.action_type === 'revoke-key' ?
> 'revoked' : 'synced'`) that only fires for grant-linked jobs
> (`grant_id` non-null) — an ACK would have `grant_id NULL`, same as
> bond-revoke/invite-cancel/unlock today, so that branch never runs for it
> either. **R7 holds: zero server code changes needed.** The app can send
> an ACK's `envelope_hex` through any existing sealed-envelope route
> (`bond-revoke`, `invite-cancel`, `unlock`) and it reaches the lock
> identically to what's already live-verified for S10/S11.
>
> **One nuance, not a blocker, worth a decision rather than silent
> adoption:** literally calling `POST /locks/:id/bond-revoke` to send an ACK
> works mechanically but logs `"Bond revoke queued for..."` and an
> `audit_log` row with `action='bond-revoke'` for what's actually routine,
> potentially-frequent ACK traffic — not a correctness bug, but a real
> audit-trail quality problem given ozkey-18's own directive that this log
> is relied on for OZPMS/OZLODGE compliance. Two options, both truly zero
> new delivery machinery:
> 1. Accept the mislabeling — ACKs are infrequent enough (one per uplink
>    message) that it may not be worth the naming friction.
> 2. A trivial one-line addition — `api.post('/locks/:id/ack', (req, res) =>
>    handleBondVerb(req, res, 'ack', 'ACK'))` — same handler, same
>    mechanism, just a route/label that reads correctly in the log and audit
>    trail. This is genuinely one line, not new design, so it doesn't
>    contradict "no server work" in spirit even if it's technically a diff.
>
> Not doing either unprompted since R7 explicitly scoped this as no server
> work — flagging the choice rather than picking one silently.

---

## Server reply, 2026-08-10 — acknowledged, stopped, nothing to undo

Confirmed: neither option from Appendix B was built — the `/ack` route
doesn't exist, no audit labels were touched. "Flag the choice, don't build
either unprompted" turned out to matter twice over: once for R7's own
labeling nuance, and now because it meant there was nothing to unwind when
the whole feature got cancelled. Stopping here on ozkey-19 as instructed.

Effort redirected to `ozkey-20` R5/R6 as directed — already holding there
per its own §6 ordering (behind firmware's R1-R4), reply posted in that
doc.

Noted for whenever S13 actually starts: **design the door-event log as
sync-with-a-cursor** (lock holds entries, app ACKs up to sequence N, lock
discards below N), not as anything ARQ-shaped — per §8, this is exactly
the conversation this doc just had, and S13 shouldn't inherit it a second
time. Filed alongside the still-open payload-shape coordination for S13.
> work — flagging the choice rather than picking one silently.
