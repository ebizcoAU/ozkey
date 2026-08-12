# ozkey-28 — OZKIE Protocol v1: verb reference

**Audience: the app team (ftpos — BANOI/MAOI) and the server team.** This is
the *normative* companion to `ozkey-27` (which carries the rationale, the
findings and the roadmap). If you are building a feature, build against this.

**Status 2026-08-13: DRAFT FOR REVIEW.** Every row is marked with what is
actually true today. Nothing here is aspirational unless it says so.

| Mark | Meaning |
|---|---|
| 🟢 **LIVE** | ships in `doorlock-1.58` / `bridge32-1.34`, hardware-verified |
| 🟡 **SPEC** | designed, shape agreed, **not built** |
| 🔴 **BLOCKED** | needs the supplier's DP payload layouts (`ozkey-27 §2.5`) |

---

# 1. OZKIE already exists — this formalises it

This is not a new protocol. The lock already speaks a JSON verb protocol inside
the sealed envelope. What v1 adds is a **complete catalogue**, a **stable error
vocabulary**, and a **namespace** — not a rewrite.

What ships today (`ozdoorlock_core.h`):

| Concept | Field | Status |
|---|---|---|
| the verb | `kind` | 🟢 LIVE |
| correlation id | `msg_id` | 🟢 LIVE (echoed in `*_response`) |
| state version | `epoch` | 🟢 LIVE (in `roster_response`) |
| replay floor | `counter_floor` per bond | 🟢 LIVE |
| role gate | admin vs member, enforced lock-side | 🟢 LIVE |
| result | ~30 stable status strings | 🟢 LIVE |

**The transport and crypto are unchanged and out of scope here:** X25519 ECDH
at pairing, AES-256-GCM sealed envelope, AAD-bound to `device_id`, monotonic
`counter_floor`. Verbs travel *inside* that envelope. Relays never see `args`.

## 1.1 Naming: `noun.action`, with the shipping names kept as aliases

v1 introduces a `noun.action` namespace so dispatch is a table rather than a
chain of `strcmp`. **The names that ship today keep working** — they are
registered aliases, not deprecated-on-arrival. No flag day (this answers
`ozkey-27` Q7).

| v1 verb | ships today as | |
|---|---|---|
| `lock.unlock` | `unlock` | 🟢 |
| `cred.put` (pin) | `grant_pin` | 🟢 |
| `cred.put` (rfid) | `grant_rfid` | 🟢 |
| `cred.delete` (pin) | `delete_pin` | 🟢 |
| `cred.delete` (rfid) | `delete_rfid` | 🟢 |
| `bond.revoke` | `bond_revoke` | 🟢 |
| `bond.invite.cancel` | `invite_cancel` | 🟢 |
| `bond.list` | `list_bonds` | 🟢 |
| `bond.query` | `query_roster` / `query_bond_state` | 🟢 |
| `device.reset` | `factory_reset` / `unpair` | 🟢 |

---

# 2. Message shape

Request (inside the sealed envelope):

```json
{
  "v":      1,
  "kind":   "cred.put",
  "msg_id": "01JB2R7QK8ZC3F4H5N6P7Q8R9S",
  "ts":     1786000000,
  "exp":    1786000600,
  "args":   { }
}
```

| Field | Req? | Notes |
|---|---|---|
| `v` | v1 | omit = 0 = the legacy flat shape that ships today |
| `kind` | ✅ | the verb |
| `msg_id` | ✅ | **ULID. Idempotency key** — replaying the same `msg_id` must be safe. Echoed in every response |
| `ts` | ✅ | **UTC epoch seconds, always.** Never local (`ozkey-21 §8.4`) |
| `exp` | ⚪ | do not execute after. Replaces ad-hoc TTLs |
| `args` | per verb | in v0 these fields sit flat at top level |

Response:

```json
{
  "kind":   "cred.put_response",
  "msg_id": "01JB2R7QK8ZC3F4H5N6P7Q8R9S",
  "ok":     true,
  "code":   "OK",
  "epoch":  17,
  "data":   { "slot": 3 }
}
```

`code` is always a **stable string**, never a bare number.

---

# 3. Verb catalogue

## 3.1 `lock.*` — the door itself

| Verb | Args | Returns | Role | Status |
|---|---|---|---|---|
| `lock.unlock` | — | `UNLOCK_OK` \| `UNLOCK_DENIED` | member+ | 🟢 LIVE |
| `lock.state` | — | `{bolt, battery, inside_open, last_event}` | member+ | 🟡 SPEC |
| `lock.settings.get` | — | see §3.5 | admin | 🟡 SPEC |
| `lock.settings.set` | see §3.5 | `OK` | admin | 🟡 SPEC |

> **`lock.unlock` is the one verb a member may issue.** Everything else in this
> document that changes state is admin-only, enforced lock-side — a member's
> envelope is authenticated and then *refused*, not ignored.

## 3.2 `cred.*` — PINs, cards, fingerprints

**This is the group the app team is waiting on, and it is the group with the
one real hole.** The *shape* below is settled; what is missing is the byte
layout the DL MCU expects (`ozkey-27 §2.5`, supplier Q2).

| Verb | Args | Role | Status |
|---|---|---|---|
| `cred.put` | `{kind, cred_id, secret, from, to, uses}` | admin | 🟢 LIVE for `pin`/`rfid` — 🔴 **against our invented DP numbers** |
| `cred.delete` | `{kind, cred_id}` | admin | 🟢 LIVE — same caveat |
| `cred.list` | `{kind?}` | admin | 🟡 SPEC |
| `cred.sync` | `{creds:[…]}` — **full-state replace** | admin | 🟡 SPEC — maps to the MCU's own `0x13` |

```json
{ "kind": "cred.put",
  "args": {
    "kind":    "pin",            // pin | rfid | fingerprint
    "cred_id": 3,                 // stable id, NOT a slot index
    "secret":  "482915",          // PIN digits, or card UID hex
    "from":    1786000000,        // UTC epoch seconds
    "to":      1786600000,        // UTC epoch seconds
    "uses":    0                  // 0 = unlimited in window, 1 = one-shot
  } }
```

Three notes the app team should design around:

- **`cred_id`, not slot.** Today's `grant_pin` takes a 2-byte `slot` and the
  caller owns allocation. v1 makes the *lock* own allocation and the app hold a
  stable id. This removes a whole class of "two admins picked slot 3" bug.
- **`uses` is real.** The supplier's credential record carries a use-count
  (`0` unlimited within window, `1` one-shot), so a genuine one-time PIN is
  available from the MCU rather than emulated by us.
- **`cred.sync` is how you recover.** Send the complete intended set; the lock
  replaces its state and returns the new `epoch`. This is not our invention —
  it is exactly what the MCU's `0x13` service does (全量下发, full set every
  time). **Poll and replace beats acknowledge and retry.**

> 🔴 **Do not ship credential features against the current DP numbers.**
> `grant_pin` writes DP 21, which on the real catalogue is *navigation volume*
> (`ozkey-27 §2.1`). The verb is right; the byte mapping underneath is not, and
> it changes when the supplier answers. **The verb contract will not change —
> so building the app UI against these verbs now is safe.** Only the firmware's
> internal mapping moves.

## 3.3 `bond.*` — who is allowed at all

The digital-passport layer. Entirely ESP32; the DL MCU has no concept of a bond
and could not enforce one.

| Verb | Args | Returns | Role | Status |
|---|---|---|---|---|
| `bond.invite` | `{label, role, me, expires}` | invite blob | admin | 🟢 LIVE (XF-87 v2) |
| `bond.invite.cancel` | `{nonce}` | `OK` | admin | 🟢 LIVE (DPID 102) |
| `bond.revoke` | `{pubkey}` | `REVOKE_OK` \| `REVOKE_NOT_FOUND` \| `REVOKE_DENIED` | admin | 🟢 LIVE (DPID 101) |
| `bond.list` | — | roster | admin | 🟢 LIVE (DPID 103) |
| `bond.query` | `{}` | `{bonds, epoch, …}` | admin | 🟢 LIVE |

🟢 **Membership expiry is enforced as of `doorlock-1.58`** — an expired member
is refused at the door, proven both directions on hardware (`ozkey-21 §10`).
The app must render `MEMBER_EXPIRED` distinctly; ftpos confirmed in XF-98 §8
that it already does.

## 3.4 `event.*` — what the lock tells you

**Upstream. 🟢 The entire event surface is fully specified in the supplier
catalogue** — types, ranges and every enum value — so this group is
implementable now and is where I would point app work first.

| Event | Payload | Source DP | Status |
|---|---|---|---|
| `event.access` | `{result, kind, cred_id, occurred_at, time_basis}` | 61/63/64/69/72/73/76 | 🟡 SPEC (today: DP 8, invented) |
| `event.alarm` | `{type}` — 18 values incl. `pry`, `unclosed_time`, `system_lock` | 60 enum | 🟡 SPEC |
| `event.duress` | `{}` | 98 bool | 🟡 SPEC |
| `event.battery` | `{percent}` −1..100 | 45 | 🟡 SPEC (today: DP 5 alarm only) |
| `event.doorbell` | `{}` | 53 | 🟡 SPEC |
| `event.bolt` | `{locked}` | 47 | 🟡 SPEC |
| `event.inside_open` | `{}` | 52 | 🟡 SPEC |
| `roster_changed` | — | in-lock | 🟢 LIVE |

Two requirements that fall out of the MCU's behaviour and **must** reach the app:

1. **`event.duress` is not an alarm.** DP 98 is a separate line because a
   coerced entry needs a different escalation path from a low battery. Do not
   collapse it into `event.alarm`.
2. **Events are replayed and backdated.** The MCU buffers **up to 20 records**
   offline and replays them on reconnect, oldest overwritten, each stamped with
   a time-basis flag (`0` none / `1` device-local / `2` GMT). So events **arrive
   out of order and may duplicate**. Dedupe on `msg_id`; order on
   `occurred_at`, never on arrival. *(This is the OZLODGE L8 audit-trail
   requirement.)*

## 3.5 `lock.settings.*` — 🟢 fully specified, nothing blocking

These are real catalogue DPs with real enum values. Free to build.

| Setting | Values | DP | Status |
|---|---|---|---|
| `volume` | `mute` \| `low` \| `normal` \| `high` | 21 | 🟡 SPEC |
| `autolock` | bool | 23 | 🟡 SPEC |
| `autolock_delay` | 5–1800 s | 24 | 🟡 SPEC |
| `ble_enabled` | bool | 42 | 🟡 SPEC |
| `conn_mode` | `keep` \| `sleep` \| `lock_keep` \| `lock_sleep` | 11 | 🟡 SPEC |

⚠️ **DP 21/23/24 are exactly the numbers our firmware currently uses for
credentials.** Settings and credentials are on a collision course inside our own
code until the mapping is corrected — see `ozkey-27 §2.1`.

## 3.6 `device.*` and `time.*`

| Verb | Role | Status |
|---|---|---|
| `device.info` | member+ | 🟡 SPEC — must return `pid`, `profile_id`, `profile_rev`, `fw` |
| `device.reset` | admin | 🟢 LIVE (`factory_reset`/`unpair`, sealed) |
| `device.provision` | — | 🟢 LIVE (`provision_assign`, unsealed pairing path) |
| `time.set` | server | 🟢 LIVE — **UTC only** |
| `time.get` | any | 🟢 LIVE |

**`device.info` is how the app stops guessing.** Once profiles exist, two locks
from two suppliers will not have the same feature set. Read `profile_id` and
grey out what a given lock cannot do rather than assuming uniformity
(`ozkey-27` Q8).

---

# 4. Result codes

Shipping today, unchanged:

`UNLOCK_OK` `UNLOCK_DENIED` `BOND_OK` `BOND_DENIED` `MEMBER_OK` `MEMBER_FAIL`
`MEMBER_FULL` `MEMBER_EXPIRED` `MEMBER_REPLAY` `REVOKE_OK` `REVOKE_DENIED`
`REVOKE_NOT_FOUND` `LIST_DENIED` `QUERY_DENIED` `QUERY_THROTTLED`
`QUERY_UNDELIVERABLE` `PAYLOAD_REJECTED` `FACTORY_RESET` `ENROLLED`
`ENROLL_FAIL` `BLE_OK` `WIFI_OK/JOINING/FAIL` `THREAD_OK/JOINING/FAIL`
`BROKER_OK/JOINING/FAIL`

Proposed additions (🟡 SPEC): `OK` `NO_SLOT` `UNSUPPORTED` `MCU_TIMEOUT`
`PROFILE_MISSING` `NOT_PROVISIONED` `CRED_NOT_FOUND` `BAD_WINDOW`.

⚠️ `UNLOCK_DENIED` is currently overloaded — it is returned for a bad `slot`, a
malformed grant, an unknown verb *and* a genuine refusal. An app cannot tell a
programming error from a security decision. The additions above split it.

---

# 5. Authority — who may originate what

Enforced lock-side from the bond role in the envelope. The `?` cells are the
open tier decision (`ozkey-27 §6.1`, Q4) and are **not** settled.

| Verb group | owner app | member app | on-prem PMS | cloud relay | NEXUS |
|---|---|---|---|---|---|
| `lock.unlock` | ✅ | ✅ | ? | ✗ | ✗ |
| `lock.settings.*` | ✅ | ✗ | ✅ | ✗ | ✗ |
| `cred.*` | ✅ | ✗ | ? hospitality | ✗ | ✗ |
| `bond.*` | ✅ | ✗ | ? | ✗ | ✗ |
| `device.reset` | ✅ | ✗ | ? | ✗ | ✗ |
| `event.*` (receive) | ✅ | own only | ✅ | ✗ blind | ✗ blind |

---

# 6. 🟢 What the app team can start on today

Ordered by how safe it is to build now:

1. **`event.*` — the whole group.** Fully specified in the supplier catalogue,
   no supplier dependency, and it is the largest gap in what BANOI shows a
   user today. Start with `event.battery` (a real percentage, not just an
   alarm), `event.duress`, and `event.doorbell`.
2. **`lock.settings.*`.** Volume, auto-lock and delay are real, enumerated
   settings a user would expect to control and we have never exposed.
3. **`cred.*` UI against the v1 verb shape.** The *contract* is stable —
   `cred_id`, `from`/`to`, `uses`. Only the firmware's byte mapping underneath
   is unsettled, and that never reaches the app.
4. **Dedupe/order handling for replayed events** (§3.4). Cheap now, and
   retrofitting an audit trail that assumed arrival order is not.

**Do not** ship anything that depends on the current DP numbers being right, and
**do not** treat a bench pass of the credential path as evidence — until phase 0
lands, that path is validated only against our own emulator (`ozkey-27 §2.1`).

---

# 7. Open, and needed from others

- **App:** does BANOI's model tolerate per-lock capability (`profile_id`), or
  assume uniformity? (`ozkey-27` Q8)
- **App:** appetite for `cred_id`-instead-of-slot — it moves allocation from
  app to lock.
- **Server:** `state_epoch` per lock, reconcile via `cred.sync` rather than
  trusting delivery; dedupe events on `msg_id`, order on `occurred_at`.
- **Operator:** the `?` cells in §5 — does the on-prem server mint bonds?
  (`ozkey-27` Q4, the trust-model fork.)
- **Supplier:** the RAW payload layouts (`ozkey-27` Q2/Q2e). One answer
  unblocks §3.2 entirely.

---

*Firmware team, 2026-08-13. Rationale, findings and roadmap: `ozkey-27`.
Firmware capability reference: `ozkey-26`. Time and expiry: `ozkey-21`.*
