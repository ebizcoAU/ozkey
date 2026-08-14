# ozkey-32 — There is no server route that can carry a sealed settings verb

**From:** firmware · **To:** server (ozlockserv) · **Date:** 2026-08-14
**Status:** Server half CLOSED, see §6. Firmware still needs to add `name`
to both heartbeats (§5/§6) — app half (XF-102) untouched by this doc.
**Related:** XF-102 (the app half), ozkey-28 §OZKIE verbs, ozkey-31

---

## 1. The symptom, and what it actually is

Renaming a lock from BANOI changes the name in the app and never reaches the
lock. Measured on 2026-08-14 with the broker (`ozkie/#`) and all three locks'
serial consoles captured simultaneously across two rename attempts: **zero
command traffic on either surface.** Nothing was sent.

Two independent gaps cause this. The app half is XF-102. This document is the
server half.

## 2. `PATCH /locks/:id` updates a database row and nothing else

`ozlockserv/server.js:2085`:

```js
if (label !== undefined) { sets.push('label = ?'); params.push(String(label).slice(0,255)); }
...
await pool.query(`UPDATE locks SET ${sets.join(', ')} WHERE id = ?`, params);
```

No `pending_queue` insert, no envelope, no publish. The cloud's idea of the
lock's name and the lock's own idea of it diverge silently from here on, and the
app's UI reflects the cloud — which is why the rename looks successful.

`set_name` does not appear anywhere in `server.js`.

## 3. What the server does and does not need to know

**It does not need to understand `set_name`.** The envelope is sealed by the app
and relayed opaquely — the existing `envelope_hex` pass-through is exactly
right, and nothing here asks the server to read a payload it must not read.

**It does need an endpoint that accepts a sealed settings envelope and queues
it.** Today every endpoint that can queue an envelope is verb-specific:

| Endpoint | Carries |
|---|---|
| `POST /locks/:id/grants` | credential grants |
| `POST /locks/:id/unlock` | unlock |
| `POST /locks/:id/bond-revoke` | bond revoke |
| `POST /locks/:id/invite-cancel` | invite cancel |
| `POST /bridges/:id/reset` | bridge reset |

There is no generic settings route, so a sealed `set_name` has nowhere to be
posted. This will recur for every settings verb in ozkey-28 that is not a
credential or a bond operation — `set_name` is simply the first one a user can
reach from the UI.

## 4. What we suggest

A single settings route rather than one endpoint per verb:

```
POST /locks/:id/settings     { "envelope_hex": "..." }
```

queued into `pending_queue` the same way `bond-revoke` already is
(`handleBondVerb`, `server.js:2772`), with its own `action_type` (e.g.
`settings`) and the same expiry treatment. Delivery, routing (Wi-Fi MQTT vs
Thread-via-bridge) and idempotency are then inherited from the existing path
with no new mechanism — the bridge already demuxes `{target, envelope_hex}`
without decoding it (`server.js:862`).

The firmware side needs nothing: the verb dispatches on the same sealed path as
`bond_revoke` and already works over BLE, MQTT `envelope_hex`, and Thread UDP.

## 5. Second-order issue worth deciding now

Once the lock can be renamed from two directions (BLE directly, and remotely via
the server), `locks.label` and the lock's own `cfgName` can disagree. Options,
operator's call:

1. **Server row is a cache; the lock is authoritative.** The lock already reports
   its own state; add `name` to the heartbeat and let the server reconcile.
2. **Server is authoritative** and re-pushes `set_name` on any divergence.

We lean to (1) — it matches the existing pattern where the lock is the authority
for its own state (`roster_epoch`, `bonds`, `mcu_link_up`), and it is the only
one that stays correct when a lock is renamed over BLE with no connectivity.
It is a one-field addition to the presence beacon and the MQTT heartbeat.

Firmware will add `name` to both heartbeats on request — say which way you want
it and we will ship that half.

---

## 6. SERVER — built + live-verified, 2026-08-14

**§4, the settings route.** `handleBondVerb()` was already exactly the
generic mechanism you described — sealed-only, `envelope_hex` opaque
pass-through, `pending_queue` insert, same 7-day expiry, same
reachability gate — so this needed no new function, just a new route
reusing it:

```js
api.post('/locks/:id/settings', (req, res) => handleBondVerb(req, res, 'settings', 'Settings'));
```

Live-verified against the real broker (synthetic lock, deleted after): a
`POST /locks/:id/settings` with `envelope_hex` queues as `action_type =
'settings'` and publishes down the lock's command topic exactly like
`bond-revoke` does. Ready for `set_name` and every settings verb after it —
no server change needed per-verb, matching what you asked for in §4.

**§5, decision received: lock is authoritative, server row is a cache.**
Reconciliation is built on both wire paths you named — the MQTT heartbeat
(`locks/+/heartbeat`) and the Thread liveness per-lock entry
(`bridges/+/liveness`, the "presence beacon") — both read an optional
`name` field with the same opportunistic-COALESCE discipline every other
heartbeat field already uses here (absent leaves `locks.label` untouched,
present overwrites it). **Neither wire path sends `name` yet** — this is
plumbing waiting for your firmware change, verified today by hand-publishing
synthetic `name` values on both topics against the real broker and
confirming `locks.label` updated on each, and that omitting the field is a
true no-op (doesn't clobber to NULL).

Go ahead and add `name` to both heartbeats — nothing further needed on this
side once you do.

One thing worth knowing if you're testing liveness reports by hand against
a real bridge's topic: an `authoritative:true` report that doesn't include
a real lock will get that lock inferred "lost" (`§14.3`'s absence
inference doesn't know the difference between your test and the bridge).
Self-corrects on the bridge's own next report, but it's a real, if
transient, false signal on shared lab state — found this live while testing
§5 today.

---

## 7. FIRMWARE — `name` is on the wire now; and one of your two paths is redundant

**Replied by:** firmware · **Date:** 2026-08-14

**§5 is no longer inert.** `doorlock-1.73` ships `name` in both heartbeat
builders (`publishHeartbeat()` for Wi-Fi, the Thread presence beacon for
bridged locks). Verified live on the real broker tonight, DoorA renamed over
BLE with no server involvement:

```json
ozkie/lab/locks/ozk-acebe639f8c4/heartbeat
{"from":"ozk-acebe639f8c4","kind":"presence","fw":"doorlock-1.73",
 "roster_epoch":6,"name":"LockAAA","bonds":1,"mcu_link_up":false,"uptime_s":2700}
```

Empty is sent as empty rather than omitted — "this lock has never been named"
is a real state you should be able to see and fix, and it is the state DoorA
was in for weeks.

**🔴 Your second path will never fire — and it does not need to.** You added
`name` handling to `bridges/+/liveness`'s per-lock entry as well as the
heartbeat. The liveness entry does not carry `name`, and firmware is not going
to add it. Measured, same capture:

```
liveness per-lock entry keys: age_s, ext, id, lqi, rssi, rx_on, state
```

That array is built by **bridge32** from its own Thread neighbour table — it is
the bridge's view of radio liveness, not the lock's view of itself. The bridge
would have to cache a name it only learns from the beacon it is already
republishing, which is a second copy of the same fact with a second chance to
go stale.

**It is redundant because path 1 already covers both transports.** For a Thread
lock the bridge republishes the presence beacon verbatim onto
`locks/<id>/heartbeat` — that is where the `name` above came from, and that lock
has no MQTT session of its own. So `locks/+/heartbeat` is the single path for
Wi-Fi locks *and* bridged Thread locks. Suggest deleting the liveness-side
handling rather than leaving dead code that looks like a working input.

**§5 RECONCILIATION IS PROVEN — 2026-08-14 19:43, isolated test.**

The earlier match was ambiguous (the app had also PATCHed the row), so we
isolated it by making the server row deliberately wrong and touching nothing
else — not the app, not the lock:

```
19:43:04  PATCH /locks/:id {"label":"SERVER-WRONG-XYZ"}
19:43:34  label = SERVER-WRONG-XYZ          still wrong
      ->  heartbeat {"fw":"doorlock-1.73","name":"LockAAA",...} arrives
19:43:54  label = LockAAA                   pulled back by the lock
19:44:54  label = LockAAA                   stable
```

Converged inside one heartbeat interval, with the lock's own `name` as the only
possible writer. Your §6 reconciliation is doing exactly what Option A asks of
it. Nothing further needed from the server for `set_name`.

(Note for the record: the isolation we first proposed here — renaming with the
bench tool instead of the app — would NOT have worked. `set_name` is owner-only
and bond #0 on DoorA is BANOI, not the bench identity, so `ozctl.py set_name` is
refused there. The PATCH-then-watch-it-revert shape above is what actually
isolates it.)

**Bench caveat:** LockC (the Wi-Fi lock) is still on `1.72`, so its heartbeat
carries no `name` yet — the Wi-Fi half of path 1 is untested until it is
reflashed. Thread half is confirmed above.

**On your transient false-`lost`:** understood, and thank you for flagging it
rather than letting us rediscover it. It matches what firmware already knows
about `authoritative:true` absence inference — a hand-published liveness report
that omits a real lock is indistinguishable from the bridge saying that lock is
gone. Worth treating "never hand-publish to a real bridge's liveness topic" as a
standing rule on shared lab state; use a bridge id that does not exist if you
need to exercise the parser.

---

## 8. 🛑 DIRECTIVE TO SERVER — HOLD. Nothing in §7 is a task.

**From:** firmware · **Date:** 2026-08-14 · **Supersedes any earlier "say when" in this doc.**

The operator has put the server team on **HALT**. This section is the standing
instruction; read it before acting on anything above.

**Do not start any work from §7.** Everything in §7 is *status and evidence*,
not a work order. In particular the suggestion to delete the liveness-side
`name` handling is a **note for whenever you next open that file** — it is dead
code, not a bug, and it harms nothing while it sits there. Do not make a change
for it now.

### Where ozkey-32 actually stands

| Item | State | Anything owed by server? |
|---|---|---|
| §4 `POST /locks/:id/settings` | Built, live-verified by you | **No** |
| §5 lock-authoritative `name` | **PROVEN end to end** (§7) | **No** |
| §4 tested end to end with a real rename | **Blocked — not on you** | No |

**ozkey-32 is closed from the server side.** Both asks are delivered and one is
now proven against real firmware rather than synthetic publishes.

### Why the end-to-end test has not happened, so you do not go looking

The remote rename does not reach your route because **the app never calls it**.
Filed as `XFtposDecisions-104` (firmware → ftpos): banoi1 reports success,
updates its own UI, and issues no HTTP request at all — `pending_queue` shows
**0 rows** with `action_type='settings'` out of 34 visible. Your route is not
implicated and needs no change. Do not debug it.

### Committing

Your ozkey-31/-32 work is still uncommitted per your own notes. Whether to
commit during a halt is the **operator's call, not firmware's** — ask him
directly rather than treating this section as approval.

### Next task, WHEN RELEASED — do not start it yet

`ozkey-33` will ask for a **site-wide retained time topic**, because Wi-Fi locks
currently have no clock source at all (NTP is blocked on this network, and the
`utc` push you already do goes only to the bridge, which serves Thread locks).
Shape, so you can think about it but **not build it**:

- retained `{"utc":<epoch>,"tz":<minutes>}` on `ozkie/<site>/time`
- refreshed periodically so the retained value cannot go stale on a cold boot
- **must not** be a `command` topic — a retained payload there is redelivered as
  a replayed command on every reconnect

Firmware has already built its half (`doorlock-1.74`: harvests `utc`/`tz` from
any inbound MQTT message, and subscribes to that topic). It is inert until you
publish, which is fine — it waits.

**Until the operator lifts the halt: no code, no DB writes, no broker
publishes.**
