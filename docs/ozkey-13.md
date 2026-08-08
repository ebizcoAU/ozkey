# ozkey-13 — `ozlockserv` relay-opaque migration (PM priority #1, Option B)

**Status 2026-08-08: firmware F1-F5 code-complete, compiles clean, NOT YET
HARDWARE-VERIFIED.** Scoped per the Project Manager's priority list (`ozkey-12.md`
§14). Operator chose **Option B — full parity with the bond ceremony**. All three
open questions this doc raised are resolved (§7). Firmware side (F1-F5) built this
session — `doorlock-1.23`, compiles clean on all three boards, not yet flashed to
hardware. Server (S1-S6) and app (A1-A7, ftpos already responded in XF-69 §6 — see
below) not started. This doc is the spec of record; shareable with the app team.

## 1. What's actually broken — two things, not one

Read the whole of `ozlockserv/server.js` (1725 lines) plus the firmware's `control`
dispatch (`ozdoorlock_core.h`) to scope this. There are two separate gaps, and the
whitepaper promise ("server stops storing plaintext credentials") only names the
first, but the second is the more interesting finding:

**1a. Server-side plaintext storage.** `POST /locks/:id/grants`
(`server.js:1188-1295`) takes `raw_value` (the literal PIN digits or RFID hex)
straight from the app's request body, persists it in `grants.raw_value` (plaintext
MySQL column), and calls `buildCredentialFrame()` server-side to turn it into a Tuya
DP frame. This is the breach the whitepaper names.

**1b. The MQTT command channel has no per-command authentication at all.**
`onMqttMessage()` (`ozdoorlock_core.h:1616-1656`) — the doorlock's own handler for
its command topic — takes `payload_hex` out of the JSON and does a "PURE FORWARD" to
the Tuya MCU (`forwardHexToMcu()`). No bond lookup, no envelope, no signature check.
The only gate is possession of the per-device broker credential issued at
enrollment — not a per-app, per-command cryptographic proof like the BLE `control`
channel has. This is true today for **remote unlock, grant-key, and revoke-key
alike**, not just credential issuance. Worth knowing even though this doc scopes to
the credential path specifically (PM priority #1's own framing) — flagged in §6 as a
related, currently out-of-scope finding.

## 2. Why the current split exists — read before overriding it

`ozdoorlock_core.h:2480-2485`, inside `ozControlDispatch()` (the sealed-envelope
handler), states the reasoning explicitly:

> v1 carries exactly one MCU verb over BLE: DP 1, the at-the-door unlock. Credential
> frames 21-24 are NOT accepted here — they reach the MCU on the server path, which
> is where issuance lives. Accepting them over BLE too would mean two authorities
> for one operation, and the BLE one has no server-side record of what it issued.

This is a real, deliberate constraint, not an oversight: **something needs to remain
the record of "what PINs/RFIDs currently exist on this lock"** for the management UI
(list active credentials, revoke by slot) and for slot-collision avoidance. Today
that's `grants` in MySQL. Option B has to answer what replaces it — this is the
crux of the design, not a detail.

## 3. What "full parity with the bond ceremony" requires, concretely

The bond ceremony's pattern: app builds the DPID frame, wraps it in
`challenge(16B) ‖ frame`, seals with AES-GCM using a key derived from the bond's
X25519 pairing secret, writes `app_id_hex ‖ envelope` to the BLE `control`
characteristic. Lock opens it (`ozControlTry()`), dispatches by DPID
(`ozControlDispatch()`), forwards to MCU if it's an MCU verb. Fully authenticated,
fully sealed, zero plaintext transits or persists anywhere outside the two
endpoints.

Applying that to credential issuance is NOT a small change, because credential
issuance today is a **remote/queued** operation (issue a temp PIN for a cleaner
without being at the door), not an at-the-door BLE operation like unlock/revoke:

- **App**: build + seal the grant/delete frame using the same per-bond key
  derivation already built for `control` — but sealed **offline**, without a live
  BLE connection to the target lock. This matters: BLE's freshness check is
  `challenge == last-issued`, read live over the `challenge` characteristic
  (`…0005`) at connection time. A queued/remote command has no live connection at
  seal time, so it cannot echo a fresh challenge — freshness must fall back to the
  bond's monotonic counter alone (`counter > floor`), the same guarantee `list_bonds`
  etc. ultimately rest on, but **without** the extra live-challenge layer BLE
  commands get. This is a genuine, structural security-property difference between
  BLE-direct and remote-queued sealed commands — flagging it explicitly rather than
  letting it be silently weaker than advertised (see §6, needs sign-off).
- **Firmware**: a NEW entry point that opens a sealed envelope arriving over MQTT
  (not just BLE). The open/dispatch logic (`ozEnvOpen`, bond lookup, counter check)
  is already written for `ozControlTry()` — this is a factoring job, not new crypto:
  pull the "open + verify" core out so both the BLE `control` characteristic and the
  MQTT command topic can feed it, with the frame source as the only difference.
  Then extend `ozControlDispatch()`'s allowed-DPID set to include grant/delete
  (21-24), role-gated to bond #0 like 101/102/103, forwarding to MCU exactly like
  DP 1 does today.
- **Rollout**: don't repurpose `payload_hex` for this — add a distinct
  `envelope_hex` field so pre-B firmware (which does a blind forward) and post-B
  firmware (which requires a sealed envelope) can coexist during rollout. The server
  picks which field to send based on the lock's reported `fw`/`caps`.
- **Server**: `POST /locks/:id/grants` accepts `envelope_hex` from the app instead
  of `raw_value` + `type` + dates. Drop `buildCredentialFrame()`/`buildDeleteFrame()`
  server-side entirely — the server never builds a credential frame again, only
  relays what the app already sealed. `grants.raw_value` is deleted from the schema,
  not just left unused (a column that still exists is a claim it might still be
  written).

## 4. Credential-record question — RESOLVED: B1

`grants` stays, drops `raw_value` only: keeps `slot_number`, `type`, `user_name`,
`date_from/to`, `sync_status`. Enough for the management UI (list credentials,
revoke by slot, show who/when) without ever holding or deriving the actual PIN/RFID
value — the server genuinely cannot answer "what is Bà Ngoại's PIN" even under
subpoena or breach, which is the actual sovereignty property being promised. Chosen
over B2 (lock-as-record, mirroring `list_bonds`) because B2 had two open unknowns —
whether the Tuya MCU protocol can even enumerate stored slots, and a real UX
regression (management list waits for the lock's next 60-600s wake instead of
rendering instantly) — that B1 avoids entirely.

## 5. Remote-command freshness — RESOLVED: counter-only is accepted

BLE's `challenge == last-issued` live round-trip has no equivalent for a
queued/remote command (no live connection at seal time). Confirmed acceptable to
fall back to the bond's monotonic counter alone (`counter > floor`) for this case —
document the difference honestly rather than imply parity with BLE's freshness
guarantee: BLE commands get counter **+** live-challenge; MQTT-sealed commands get
counter only.

## 6. MQTT authentication gap — RESOLVED: bundled in, not deferred

§1b above (originally scoped out) is now explicitly **in scope**: the same
sealed-envelope machinery being built for grant/delete extends to **every** MQTT
command — remote unlock (DP 1), factory reset, assisted unlock included. One
mechanism, not a credential-only special case. `onMqttMessage()` moves from "pure
forward" to "open sealed envelope, then dispatch" for every verb it currently
blind-forwards. Verbs that aren't DP frames at all (`{op:'factory_reset'}`) need
their own reserved verb inside the envelope dispatch, the same way 101/102/103 are
in-lock verbs rather than MCU forwards — exact shape is firmware's call during F3.

## 7. Sign-off — all confirmed 2026-08-08

| # | Decision | Outcome |
|---|---|---|
| 1 | Credential record model | **B1** — metadata only, `raw_value` dropped |
| 2 | Counter-only freshness for remote commands | **Accepted** — documented as an honest, real difference from BLE |
| 3 | MQTT authentication gap | **Bundled** — same machinery covers all MQTT commands, not just grants |

## 8. Work breakdown by team

### Firmware (ozkey)

| Task | Description | Priority |
|---|---|---|
| F1 | Factor `ozControlTry()`'s open/verify logic so BLE and MQTT share it | High |
| F2 | Add MQTT-sealed-envelope entry point (`onMqttMessage()` calls the shared open/verify) | High |
| F3 | Extend `ozControlDispatch()` to accept DPID 21-24 (grant/delete) on the sealed path, plus a reserved verb for non-DP control-plane ops (`factory_reset` etc., per §6) | High |
| F4 | Role-gate DPID 21-24 to bond #0 (owner/admin only — members cannot issue PINs) | High |
| F5 | Add `envelope_hex` field to the MQTT command topic alongside legacy `payload_hex` | High |
| F6 | Keep legacy `payload_hex` support during rollout; drop after cutover | Later |

**Acceptance:**
- BLE `control` DP 1/101/102/103 continue to work (no regression).
- MQTT `envelope_hex` for DP 1/21-24 is opened, verified, forwarded.
- Legacy `payload_hex` continues to work for pre-B firmware.
- DPID 21-24 over the MQTT sealed path are role-gated to bond #0.
- A member (non-admin) bond gets `UNLOCK_DENIED` attempting DPID 21-24.

### Firmware gap found scoping the bench test (2026-08-08) — F2/F3 only cover
    WiFi-direct locks; Thread locks behind a bridge get nothing

Discovered trying to hardware-test F1-F5 against the actual bench fleet: **all
three bench locks are Thread-connected** (`bridge_id` set on every row,
`ozk-*` locks confirmed via `GET /locks`). F2's `envelope_hex` entry point
only lives in `onMqttMessage()` — the direct-WiFi MQTT client's own handler. A
Thread lock never runs that function; it receives commands through a
completely separate path: `bridge32.ino`'s own MQTT client relays `{target,
payload}` over Thread UDP multicast, and the lock's `pollThreadUdp()`
(`ozdoorlock_core.h`) pure-forwards whatever `payload` it gets straight to
`forwardHexToMcu()` — no crypto awareness anywhere in that chain.

This isn't just a bench-testing inconvenience — it's a real scope gap. The
whitepaper promise (no plaintext credentials, sealed issuance) is supposed to
cover the fleet, and Thread locks behind a bridge are likely the *majority*
of real deployments, not an edge case. Shipping F1-F5 alone would leave
sealed credential issuance working only for WiFi-direct locks.

**Extension is small — three places, same reused core, bridge stays dumb:**

| Task | Where | Description |
|---|---|---|
| S7 | `ozlockserv` — `flushQueueForDevice()` | When relaying to a bridged lock, send `envelope_hex` instead of `payload` in the `{msg_id, target, ...}` body when the queued job is sealed (keys off S6's `msg_type` marker — no new column). |
| BR1 | `bridge32.ino` — `mqttMessageReceived()` / `forwardOverThread()` / `sendToThreadGroup()` | Read `envelope_hex` alongside legacy `payload`/`payload_hex`; relay whichever field arrived under its own name, unchanged, over UDP multicast. **The bridge never decodes or understands it** — pure pass-through, consistent with its designed role (smarter relay, not an authority). One generalized field name instead of hardcoded `"payload"`. |
| F7 | `ozdoorlock_core.h` — `pollThreadUdp()` | Same branch F2 added to `onMqttMessage()`: check `envelope_hex` first — if present, `ozHexDecode` + `ozControlOpen` + `ozControlVerifyAndDispatch(hasChallenge=false)`, exactly the F1 core, just fed from the UDP datagram instead of the MQTT JSON. Falls back to legacy `payload` + `forwardHexToMcu()` unchanged when absent. |

No new crypto, no new wire format — `envelope_hex`'s byte layout is identical
whether it arrives via direct MQTT or via the bridge relay; only the
transport carrying it to the lock differs. F1's shared open/verify core
already accounts for "no live challenge" (§5), which is exactly what a
Thread-relayed command needs too — it was designed for this from the start,
just not wired to this second transport yet.

**Sequencing:** S7/BR1/F7 can go in the same pass as F2/F3 review, or as an
immediate follow-up — not a separate migration, since it's additive to
firmware that's already built and reuses everything F1 already did.

**Status 2026-08-08 (later same day): S7/BR1/F7 all built, compiled clean,
NOT YET HARDWARE-VERIFIED.** Built in parallel with a separate session that
had already landed S1/S2/S5/S6 server-side (live-verified against the real
broker/DB — see `[[ozkey-13-server-status]]` memory) and added the guard
this section describes (sealed+bridged left `queued` rather than silently
lost). S7 replaces that guard with the real relay, now that BR1/F7 exist to
receive it:

- **S7** — `flushQueueForDevice()`'s bridged branch now sends `envelope_hex`
  when `job.msg_type === 'sealed_envelope'`, `payload` otherwise — mirrors
  the direct-MQTT `envelope` object's existing pattern exactly. The "leave
  queued, can't reach it" guard is gone; it can reach it now.
- **BR1** — `bridge32-1.8`. `mqttMessageReceived()`/`forwardOverThread()`/
  `sendToThreadGroup()` generalized to a `(fieldName, valueHex)` pair instead
  of a hardcoded `payload` string — relays whichever field arrived
  (`envelope_hex` checked first, `payload`/`payload_hex` as fallback) under
  its own name, unchanged. Bridge still never decodes anything.
- **F7** — `doorlock-1.24`. `pollThreadUdp()` gets the identical branch F2
  added to `onMqttMessage()`: `envelope_hex` present → `ozHexDecode` +
  `ozControlOpen` + `ozControlVerifyAndDispatch(hasChallenge=false)`; absent
  → legacy `forwardHexToMcu()` path, unchanged.
- **Also fixed while touching `doorlock.ino`**: `FW_DISPLAY_VERSION` had been
  stuck at "V1.21" through the 1.22/1.23 bumps — `FW_VERSION` moved, the
  on-screen badge didn't. Same two-versions-disagreeing trap the file's own
  1.2 changelog entry exists to prevent. Fixed as part of 1.24.

Compiles clean: `doorlock` (1.47"), `doorlock19` (1.9"), `dlock19`, `bridge32`
(GEEKC6), `ozlockserv` (`node --check`, and live-reloaded cleanly under its
own `--watch` process — `/health` confirms DB+MQTT still up post-reload).
**Not flashed to any board yet** — firmware changes need the operator to
flash `doorlock-1.24` + `bridge32-1.8` before any of this can be exercised
against real hardware. `server.js` is uncommitted (shared with the parallel
session's uncommitted S1/S2/S5/S6 work) — not committing without explicit
confirmation, per [[workflow-conventions]].

### Server (ozlockserv)

| Task | Description | Priority |
|---|---|---|
| S1 | Accept `envelope_hex` in `POST /locks/:id/grants` (new field, alongside `raw_value` during rollout) | High |
| S2 | Store `envelope_hex` in `pending_queue` instead of building a frame | High |
| S3 | Drop `grants.raw_value` column (migration) | High |
| S4 | Delete `buildCredentialFrame()` / `buildDeleteFrame()` server-side | High |
| S5 | Keep `grants` metadata: `slot_number`, `type`, `user_name`, `date_from/to`, `sync_status` | High |
| S6 | Add a message-type marker to `pending_queue` — `legacy_payload` vs `sealed_envelope` | Medium |

**Acceptance:**
- `POST /grants` with `raw_value` still works during rollout (legacy apps).
- `POST /grants` with `envelope_hex` stores it, never decrypts it.
- `grants.raw_value` removed at cutover — no plaintext PINs anywhere.
- `GET /locks/:id/grants` still returns metadata (slot, type, user, dates, status).

### App (ftpos)

| Task | Description | Priority |
|---|---|---|
| A1 | Client-side sealing for grant/delete frames using the existing bond-key derivation | High |
| A2 | Adapt sealing for offline/no-live-challenge (counter-only freshness, §5) | High |
| A3 | Send `envelope_hex` to server instead of `raw_value` | High |
| A4 | Keep legacy `raw_value` path for older servers (fallback during rollout) | Medium |
| A5 | Update the `POST /grants` call site to the sealed-envelope path | High |
| A6 | BLE grant/delete — **not needed**: `XF-59` §4/§7.2 (AV) already confirms BLE `control` carries DP 1/101/102 only, never 21-24; this migration is MQTT-only by design, consistent with that. | — |
| A7 | **Found by ftpos in review, XF-69 §6, not originally scoped here.** The persistent credential-list UI (`banoi_doorlock.dart:3998`) displays `g.pin`, sourced from server `GrantInfo.rawValue` — for every row, not just freshly-issued ones. Once S3 drops `grants.raw_value`, that field is gone server-side by design (B1, §4) and the app can no longer redisplay a PIN it didn't itself just create. Fix: cache the PIN locally at issuance time (keyed by grant id/slot); rows the app holds no local PIN for (pre-existing grants, or issued from a different device) fall back to metadata-only display (label/type/slot/dates, no PIN) — honest rather than broken. The freshly-created-grant toast is unaffected (echoes a locally-held value, never round-trips through the server). | High |

**Acceptance:**
- App builds and seals DPID 21/23 (add) and 22/24 (delete) frames.
- Envelope freshness is `counter > floor` only (no live challenge) — matches §5.
- App sends `envelope_hex` to the server; no `raw_value` on the sealed path.
- Legacy `raw_value` path still works against an unmigrated server.
- Credential list renders correctly post-cutover: locally-known PINs shown, unknown ones fall back to metadata-only, never a blank/broken row.

**ftpos findings, XF-69 §6 (2026-08-08), worth recording here directly:**
- A1/A2 were **already built**, not new work — `Keyring.sealAddTempPin`/`sealAddTempRfid`/`sealDeletePin`/`sealDeleteRfid` (`keyring.dart:263-295`) already seal DPID 21-24 with no challenge parameter at all (built under XF-42/46 for a server path that was never finished). Existing round-trip test coverage is self-consistent but not yet checked against firmware's byte-exact vectors — the one residual item under §8's recommendation.
- A3/A5 are real, scoped work — `issuePin()` (`doorlock_service.dart:1472-1504`) still calls plaintext `raw_value`; `envelope_hex` doesn't exist yet on the request/response types (`directory_client.dart:574-592`). Straightforward once S1 ships.
- **Revoke is further behind than issue** — `revokeGrant()` doesn't build a frame client-side at all today, sealed or plaintext (bare `DELETE /grants/:grantId`). Reaching parity needs an extra step this doc didn't call out: look up the grant's `slot_number` (already returned by `listGrants()`) and build/seal the DPID 22/24 delete frame before the call.
- ftpos's estimate: 3-4 days holds, proceeding on A3/A5 + the revoke slot-lookup/seal path + A7 in one pass.

## 9. Implementation sequence

| Phase | Work | Teams | Duration |
|---|---|---|---|
| 1 | Firmware F1-F3 (factor shared open/verify; add MQTT entry; extend dispatch) | Firmware | 3-5 days |
| 2 | Server S1-S6 (accept envelope, drop `raw_value`, delete builders) | Server | 2-3 days |
| 3 | App A1-A5 (client-side sealing; update call site) | App | 3-5 days |
| 4 | Integration test: end-to-end sealed PIN issuance over MQTT | All | 1-2 days |
| 5 | Rollout: server + app + firmware in one coordinated release | All | TBD |

Phases 1-3 can run in parallel — F1 (the shared open/verify core) is the one piece
both BLE and MQTT firmware paths need, so it's the critical path within firmware
specifically, not a blocker for server or app work starting.

## 10. Rollout strategy

| Phase | What ships | Backwards compatible? |
|---|---|---|
| 1 | Server accepts both `raw_value` and `envelope_hex` | Yes — legacy apps keep working |
| 2 | App ships sealed-capable build | Yes — older servers ignore `envelope_hex` |
| 3 | Firmware ships sealed-envelope handler | Yes — supports both `payload_hex` and `envelope_hex` |
| 4 | Server drops `raw_value` | **No** — all apps must be sealed by this point |
| 5 | Firmware drops `payload_hex` support | **No** — all servers must be updated |

## 11. Security properties — before / after

| Aspect | Before | After |
|---|---|---|
| PIN stored on server | Plaintext in `grants.raw_value` | No PIN anywhere on the server |
| Server can read credentials | Yes (plaintext) | No (sealed envelopes) |
| Server can build frames | Yes (`buildCredentialFrame()`) | No (app builds/seals) |
| MQTT command authenticated | No (broker ACL only) | Yes (sealed envelope + counter) |
| Remote command freshness | Depends on lock wake, no cryptographic freshness | Counter-based, honestly documented as weaker than BLE's counter+challenge (§5) |
| Breach exposure | All PINs, all logs | Ciphertext + non-secret metadata only |

## 12. Success criteria

| Criteria | Verification |
|---|---|
| No plaintext PINs in `grants` | MySQL `grants` has no `raw_value` column |
| No `buildCredentialFrame()` server-side | Server code reviewed |
| App sends `envelope_hex` | `POST /grants` captures show no `raw_value` |
| Lock opens sealed envelope over MQTT | Bench test: issue PIN via app, lock stores it |
| Legacy locks still work | `payload_hex` path continues to work during rollout |
| Member cannot issue PIN | Non-admin bond #N gets `UNLOCK_DENIED` on DPID 21-24 |

## 13. Status

All three open questions from the original scoping pass are confirmed by the
Project Manager (§7). Work has not started. Next: raise the cross-team XF doc to
ftpos so the app team has this spec directly (this doc is written to be shareable
as-is), then begin Phase 1 (firmware F1) as the critical-path item.
