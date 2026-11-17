# ozkey-56: ozlock relay for OZKIE v2 locks (AK1037). Design note

Status: DRAFT rev 1, 2026-11-18, ozkey-srv (server lane). For PM nexus-46 and fw ozkey-c4.
Ticket: OZKIE-V2-SERVER-1, sections A + C of
`nexus-lab-backups/ozkey_notes/2026-11-07_OZKIE-V2-SERVER-1_acceptance.md`.
Spec: ozkey-48 rev 7, §2.3-§2.4 (the relay wire), §4.2 (Echo), §7 (server duties).
Branch: `server/ozkie-v2-relay`.

## 0. Today

- ozlock knows a lock's protocol: `locks.ozkie_version`. It is set by `POST /pairings` and
  overruled by the bridge's lock presence (`registry.js`).
- Every route that would carry bytes to a v2 lock refuses with 501 `ozkie_v2_relay_not_built`:
  grant, grant revoke, remote unlock, bond verbs, and factory reset (which drops the record only).
- So an AK1037 owner can pair at the door, and ozlock shows presence, but nothing else works
  remotely.

## 1. What ozlock does for a v2 lock (scope)

| Flow | Who does the work | What ozlock does |
|---|---|---|
| **First pairing** | the app at the door, BLE session link, keypad code (ozkey-48 §3.4) | nothing on the wire. Afterwards the app calls `POST /pairings {ozkie_version: 2, bridge_id}`, which already exists |
| **Relay bond** (the owner's persistent relay context) | the app runs EDHOC **through** the relay (or at the door) | carries the EDHOC frames (§3 below) |
| **Live unlock / lock / reset, reads, cred writes** | the app builds the OSCORE request; the lock answers | carries one request, returns the one response (§3) |
| **Status** | the bridge's lock presence (`ozkie_version`, online/offline, `via`) | as today: `GET /locks/:id`. Battery, firmware and clock (D1) are an OSCORE read the app makes through the relay |

The app is the OSCORE client end to end. ozlock never holds a key, never opens a frame and never
learns the verb (OSCORE hides the method). That is ozkey-48's "blind relay" applied to the server.

## 2. Transit-only rule (PM nexus-46, 2026-11-17, pending the operator's wording ruling)

- No v2 frame touches the DB: no `pending_queue` row, no audit row per frame, no log of payload
  bytes. Frames live in process memory only, for at most the request's wait (≤ 10 s, or 35 s for
  the EDHOC lease).
- In memory, ozlock counts dropped frames and refusals by reason. The C0b metrics read those counters later.
- One audit row per **relay bond setup** (an EDHOC that completed message_3): `ozkie_edhoc`,
  app_id, device_id. It's the same §4.1 "security events" class as v1 pairing, and holds no content. ⚠ This is
  the only stored trace. If the operator rules "no audit for v2", it's a one-line removal.
- Remote actuation is not audited per request, because ozlock cannot tell an unlock from a read. The
  lock's own audit trail (ozkey-48 §5.1) is the record.

## 3. The relay endpoint

```
POST /ozlockserv/api/locks/:id/ozkie
Content-Type: application/json
{ "app_id": "...", "frame_b64": "<type byte ‖ message, base64>" }
```

It's base64 in JSON because the rest of the API is JSON. On MQTT the frame goes as **binary** (§2.1).
The app sends one frame and gets back the lock's answer in the same HTTP response (a long poll, up to the wait below).
The 4.01 + Echo answer is just an answer: the app repeats with a new request (new token) inside
10 s. So "route Echo back to the original sender" (ozkey-48 §7) falls out of the request/response
shape. No server state is needed for it.

### 3.1 Checks, in order (all fail fast, nothing queued)

| # | Check | Refusal |
|---|---|---|
| 1 | frame decodes; length 2..1025; type ∈ {0, 1} | 400 `bad_frame` (counted). Type 2 from a sender is refused too: only the lock ends handshakes |
| 2 | lock exists, `ozkie_version = 2` | 404 / 409 `not_ozkie_v2` |
| 3 | caller is the owner (`app_id` = `locks.app_id`) | 403 `relay_owner_only`. UX only, as for v1 unlock: the lock enforces roles. Members have no relay context (§4.1) |
| 4 | `bridge_id` set and that bridge's presence online | 503 `bridge_offline` |
| 5 | MQTT connected | 503 `mqtt_down` |
| 6 | type 1: CoAP token ≥ 4 B, not already in flight for this lock | 400 `token_too_short` / 409 `token_in_flight` |
| 7 | type 0: the EDHOC lease (§3.3) | 409 `edhoc_busy` |
| 8 | topic ≤ 200 B | 500 `topic_too_long` (can't happen with the column maxima; asserted) |

### 3.2 Correlation (type 1, OSCORE)

- ozlock reads the **outer** CoAP header only: TKL and the token. That header is plaintext by design
  (RFC 8613). It reads nothing past the token.
- It keeps `pending[(lock_id, token)] = {resolve, bridge_id, deadline}` in memory and publishes to
  `ozkie/<site>/bridges/<bridge_id>/ozkie/<lock_id>`.
- On the uplink `ozkie/<site>/locks/<lock_id>/ozkie/<bridge_id>`, it parses the token and resolves the
  match: 200 `{ok, frame_b64, bridge_id}`.
- If nothing matches, the frame is dropped and counted. That covers late answers after a 504, and
  Observe notifications (see §6).
- No answer in 10 s (`OZLOCK_OZKIE_WAIT_MS`) gives 504 `no_ack`, with `outcome: "unknown"`. The request
  is never re-sent (ozkey-48 §2.4.4).

### 3.3 EDHOC (type 0)

- One lease per lock across all senders, 35 s (`OZLOCK_OZKIE_EDHOC_LEASE_MS`). Only the owner app
  can send here today, but the lease is keyed by sender so ozpms/ozlodge can share it later.
- **message_1** (first byte < 0x40, ozkey-48 §2.4.3) opens the lease for this app; any type 0 frame
  from a different sender while it's open gives 409 `edhoc_busy`. The HTTP request then waits for the next type 0 uplink: message_2 (or an EDHOC error). That reply goes back as 200. On a timeout the lease is released and the reply is 504.
- **message_3** (a byte string) is accepted only from the lease holder. ozlock publishes it, then
  waits up to 2 s for a type 0 uplink: an EDHOC error means the lock refused, so 200 with that frame
  and the lease released. Silence means success (the lock sends no message_4), so 200
  `{ok, frame_b64: null}`, the lease is released and `ozkie_edhoc` is audited.
- A type 0 uplink with no lease open is dropped and counted.

### 3.4 Failover

- An exchange is pinned to the bridge it was published on.
- `registry.js` already moves `locks.bridge_id` on presence `via` (XF-144/151). It now also calls
  `relay.abortPinned(lockId, oldBridge)`. Every open wait on the old bridge then ends at once with 503
  `bridge_moved`, `outcome: "unknown"`, and the lease is released.
- An uplink whose trailing `bridge_id` is not the pinned bridge doesn't resolve anything. It is dropped and counted.

## 4. The old routes for a v2 lock

Grant, revoke, bond verbs, unlock and the v1 reset envelope stay closed to v2. They carry v1 sealed
envelopes, which mean nothing to a v2 lock. Their 501 becomes 409 `ozkie_v2_use_relay`, which names
the endpoint above. Remote unlock keeps the owner-only 403 first. Factory reset for v2 keeps "record
only" on the DELETE route; `dev/reset` is something the app sends through the relay.

## 5. Bridge bond, section C (agreed 2026-11-06)

- Additive, nullable columns only, no backfill: `bridges.ozkie_pub`, `ozkie_pub_at`,
  `ozkie_pub_conflict`; `lock_bridges.bond_pub_fp`, `bond_slot`, `bonded_at`.
- `bridges.ozkie_pub` + `ozkie_pub_at`, taken from the bridge presence `ozkie_pub` on first sight (TOFU).
  A different value later: keep the pinned one, set `ozkie_pub_conflict`, and serve null with
  `reason: "conflict"`. Re-pinning is by admin only (the PM ruling on re-key confirmation is still open; until then a DB
  edit, logged).
- `GET /bridges/:id?app_id=` returns `{id, ozkie_pub|null, reason}`. It is owner-scoped: 404 unless the bridge is
  paired to one of the caller's locks.
- `PUT|DELETE /locks/:lock_id/bridges/:bridge_id/bond` takes `{app_id, bond_pub_fp, slot}`. It sets
  `lock_bridges.bond_pub_fp`, `bond_slot` and `bonded_at` (an advisory claim from the app). Refusals: 409
  `bridge_bonds_full` (max 2 per lock) and 409 `pub_mismatch` (the fingerprint doesn't match the pinned pub).
- The console shows a bridge that isn't enrolled, or a bond held by a decommissioned bridge. That waits for the C0b console work and is
  not in this branch.

## 6. Not in this branch (follow-ups, none blocks the kit)

1. **Observe `events` to the owner app.** Notifications reuse the registration's token long after
   the HTTP request ended. Delivering them needs a push channel to the app (MQTT `members/<app>`
   or an SSE stream). Until then they are dropped and counted, and the app reads `events` by GET.
2. **ozpms / ozlodge as service-bond OSCORE clients** (acceptance section B, SAFE mode). They're senders and not relays. That's a
   separate ticket.
3. REST auth on `app_id` (lab REST is open; ozkey-51). The relay inherits whatever lands there.

## 7. Tests (before any commit)

- New `ozlock/server/test/ozkierelay.test.js` against the testkit broker, with a **stand-in bridge**
  that does MQTT ⇄ the **host-built ozcore lock**'s relay port: `ozhostlock --relay PORT`
  (`blelock/ozcore/tools/hostlock.c`). That's TCP, framed `type ‖ len(2, BE) ‖ payload`, and the
  contexts persist in `--state`, so a restart = a lock reboot with Echo first. The test-side app is
  `ozapp.py` (EDHOC initiator + OSCORE client) or its JS equivalent. The stand-in bridge never
  opens a frame, just like the real one. Cases:
  - EDHOC m1→m2→m3 through ozlock;
  - an OSCORE GET `lock/state`;
  - an unlock with the 4.01 + Echo round trip;
  - the refusals in §3.1;
  - the lease busy case;
  - a token collision;
  - 504 with a late answer dropped;
  - failover abort;
  - the binary payload kept byte-exact (a max-size 1025 B frame with a 50-char site and 64-char ids);
  - **the DB row counts unchanged across the whole run** (transit-only).
- Full gate: core / ozpms / ozlock / ozlodge, ×3 quiet + ×3 concurrent; a negative control per
  refusal (each mutant diffed before trusting it).
- Then the lab: a real F1 bridge plus an ESP32-C6 v2 bench lock (`mqttrun.py`, now with ozlock instead of
  the script as the server). Then the AK1037 kit with ozkey-c4.

## 8. Forks for the PM

None blocks the build. Two defaults I've taken, so they can be overruled cheaply:
- (F1) one `ozkie_edhoc` audit row per completed relay bond (§2);
- (F2) the long-poll HTTP shape, with Observe deferred (§6.1).
