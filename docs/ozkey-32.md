# ozkey-32 — There is no server route that can carry a sealed settings verb

**From:** firmware · **To:** server (ozlockserv) · **Date:** 2026-08-14
**Status:** OPEN — server work required
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
