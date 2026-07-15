# OZKEY-07 — OZKEYSERV Commercial / Managed-Fleet Contract (Mode A)

> **HOTEL SLICE BUILT 2026-07-11 (OZKEYSERV, lab-verified, not committed).**
> Implemented in `ozkeyserv/server.js`: **`POST /pms/rooms`** (§4 — upsert +
> reconcile, `id`-keyed idempotent upsert, non-destructive deactivation, bound-
> lock / live-credential `conflicts[]`, in-band `lock_device_id`), **`GET
> /pms/rooms/status`**, extended `GET /rooms`, the **`X-OZKEY-Secret`** write
> gate on all `/pms/*` writes (§4.4 — enforced when `OZKEY_PMS_SECRET` is set,
> open in the lab otherwise), and the **auto-seed is retired** (rooms come from
> the PMS; `OZKEY_SEED_ROOMS=1` restores the old lab seed). Additive schema
> migrations (`maoi_id` unique, `name`, `room_type`, `capacity`,
> `lock_device_id`, `active`, `last_synced_at`). Verified: upsert, rename-by-id
> (no orphan), reconcile deactivates absent rooms non-destructively + reports a
> bound room as a conflict without dropping its binding, and the secret gate
> (401 without / 200 with, reads open).
>
> **Device-scoped topic refactor (§10) + `room_no→device_id` resolution (§5)
> BUILT 2026-07-11, bundled (not fast-followed).** `POST /locks/pair` now
> writes `lock_device_id` (PMS-pushed value if present, else derived from the
> paired MAC) and the `provision_assign` handshake carries `site_id` +
> `device_id` alongside the legacy `room_no`. Every queue flush
> (`flushQueueForRoom`) resolves the room's bound device once and
> **dual-publishes** each command — the legacy `hotel/rooms/<room_no>/lock/
> command` topic and the new `ozkey/<site_id>/locks/<device_id>/command` topic
> — so either a legacy-paired or device-scoped lock stays in sync during the
> transition. OZKEYSERV subscribes `ozkey/<SITE_ID>/locks/+/{heartbeat,log}`
> (site-pinned, not wildcarded — `OZKEY_SITE_ID`, default `hotel`, keeps it
> distinct from ozlockserv's `lab` site on the same shared broker) and resolves
> inbound device_id → room via `lock_device_id` or the MAC-derived id. LockSim
> (`locksim/lib/provisioning.ts`, `hooks/useProvisioning.ts`, `app/page.tsx`)
> adopts the granted `site_id`/`device_id` from the handshake and moves its
> heartbeat/log/command traffic to the device-scoped topics, ignoring the
> legacy room-topic command copy once device-scoped (avoids double-executing
> credential writes). Verified end-to-end over the live broker + MySQL: pair →
> handshake carries `site_id`/`device_id` → `lock_device_id` persisted → queue
> flush dual-publishes on both topics → device-scoped heartbeat/log resolve to
> the correct room and land in `lock_logs`; legacy handshakes without
> `site_id`/`device_id` still parse and pair the old room-scoped way (no
> regression for not-yet-migrated locks). `issue-key`/`revoke-key` unchanged,
> still work end-to-end. Not yet done: MAOI never sees or sends `device_id`
> (still §5-invisible, per spec) — this refactor is server + lock internals
> only, no API-surface change to `/pms/*`.
>
> **ROSTER-PUSH RECONCILIATION 2026-07-12 (lab-verified).** Driven by MAOI's
> manual "Gắn khoá" lock-bind UI and first real roster pushes hitting stale
> pre-PMS rows ("room already exists" app errors). Three additive changes to
> `POST /pms/rooms` + one new endpoint, all in §4.2: **adoption** (a pushed
> room_no matching a `maoi_id`-NULL row claims it, preserving its pairing +
> binding + credentials; new `adopted[]` in the response), the
> **`lock_device_id` tri-state** (absent = keep; explicit null = guarded clear
> with a live-credential warning; value = bind, refused with a conflict when it
> disagrees with a paired lock's binding of record — prevents the §10
> stranded-commands hazard), and **`POST /pms/reset`** (operator-confirmed
> `{"confirm":"ERASE"}` mirror wipe behind the §4.4 gate, exposed as the
> cockpit's type-to-confirm RESET MIRROR button). Verified live: adopt
> preserves a paired row's binding; mismatched bind refused + reported while
> roster fields still apply; explicit-null clear applies and warns on live
> credentials; owned-row `room_no` collisions still refuse; reset wipes all
> five tables and requires the confirm literal.
>
> **RECONCILED WITH THE APP 2026-07-12 (XF-43 §7.4–§7.6).** FtposPM confirmed
> the contract and corrected one earlier ask: MAOI's **bulk roster push emits
> `lock_device_id` only when bound** (omit-when-unbound → tri-state "keep"),
> NOT explicit null — a bulk null would guarded-clear every binding MAOI
> doesn't locally know (cockpit-paired rooms, post-reset tablets). **Deliberate
> unbind ("Gỡ khoá") = `POST /locks/unpair` per room**, never the bulk path.
> The explicit-null guarded clear remains a server capability for per-room
> deliberate ops. Authority model adopted (§6.2): MAOI originates · OZKEYSERV
> records · cockpit observes (fallback writer only under declared emergency).
> Accordingly `/locks/pair` + `/locks/unpair` now sit behind the same
> `X-OZKEY-Secret` gate as all `/pms/*` writes (lab: open while no secret is
> configured). MAOI's recovery half is built app-side: `serverBindings()` +
> "Khôi phục khoá từ máy chủ" re-learn bindings from `GET /rooms` after a
> tablet loss; `adopted[]`/`conflicts[]` surface in the sync dialog. The
> cockpit's **DECLARE EMERGENCY** takeover control (arms the secret on cockpit
> writes, §6.2) is built and the widened gate verified 401/pass. Incoming
> firmware asks from XF-43 §7.5 (BLE bootstrap) recorded in §11.
>
> **FLEET SLICE V1 BUILT 2026-07-14 (lab-verified).** The §3/§8/§9 core on
> OZKEYSERV: `orgs` (with the §2.1 `root_mode` company|owner recorded per
> org — the reclaim ceremony is later), `operators` (role + bearer token, the
> ozkey-05 §4 auth-gap v1), `operator_scopes` (portfolio = one-op reassign),
> `revocations`, persistent `audit_log`, `rooms.suspended`,
> `credentials.issued_by`. `/pms/issue-key`+`/pms/revoke-key` now accept EITHER
> the shared secret (full scope) OR `X-OZKEY-Operator-Token` (scoped,
> revocable): invalid token 401, disabled operator 403, out-of-scope room 403,
> suspended door 409. **Disable an operator = §9 scenario 1 end-to-end:**
> token authority dies instantly AND every live credential they issued is
> auto-revoked (DPID delete frames queued per room, land on next heartbeat).
> New endpoints: `POST/GET /fleet/operators`, `/fleet/operators/:id/
> scope|disable|enable`, `GET /fleet/audit`, `POST /locks/suspend|resume`,
> and **`POST /locks/resync`** (closes the §6.1 step 5 lock-swap gap:
> re-queues all live unexpired credentials for a room so a replacement lock
> receives the full set on first heartbeat; expired/revoking excluded,
> idempotent). All admin surfaces behind the §4.4 secret. Verified live:
> scoped issue 200 + attribution, out-of-scope 403, bogus token 401,
> suspend 409/resume, disable→auto-revoke(1)+403, re-enable, resync
> requeue, audit trail app-attributed. Still outstanding: owner-root reclaim
> ceremony, org hierarchy/regions, revocation lists for app-key (BLE)
> credentials, e2e envelope + KMS (ozkey-06).
>
> **DRAFT 2026-07-11.** The ozkey-team contract for **Mode A** (on-prem
> OZKEYSERV `:3200` + cockpit `:3300`) covering the two commercial shapes that
> share one server: (1) **hotel/motel PMS** — MAOI front desk, roster sync +
> auto-issue at check-in (FTPOS `XFtposDecisions-43` §7/§11, being built P7),
> and (2) **managed fleet** — a property-management company with many operators
> over many properties (the 10k-property / 100-manager scenario). The hotel is
> the single-org case of the fleet, so one doc. Consumers: **MAOI** (hotel
> PMS), the **OZKEY Flutter app** (commercial fleet front end), **OZKEYSERV**,
> **cockpit** `:3300`. Depends on: ozkey-02 (room matrix, issue/revoke, queue +
> DPID codec), ozkey-04 §1/§9 (market C, device-scoped topics), ozkey-05 §1.3/§4
> (same-codebase-two-deployments, the auth gap), ozkey-06 (e2e envelope).

---

## 1. Positioning — and the sharp contrast with OZLOCK

Mode A is the **opposite trust model** to OZLOCK (market A residential). Getting
this contrast right is the whole design:

| | **OZLOCK (residential, ozkey-05)** | **OZKEYSERV commercial (this doc)** |
|---|---|---|
| Server authenticates | **nobody** | **every principal** |
| Who owns the lock keys | the **app** (self-generated) | the **organization** (escrowed) |
| Revocation of a lost/departed phone | **physical re-pair at the door** | **central, server-side, no site visit** |
| System of record | none (blind relay) | **OZKEYSERV** |
| Right for | a sovereign individual | a company managing others' access |

A company with 100 managers and 10,000 doors **cannot** use the OZLOCK model —
"disable an operator" and "revoke a manager who kept their phone" are exactly
what an account-less, physical-recovery design can't do. So Mode A is
server-authoritative by necessity.

Both commercial shapes are the **same OZKEYSERV codebase**:
- **Hotel** = one org, front-desk operators, rooms = properties, PINs issued
  *by the dozen automatically* as a byproduct of bookings.
- **Fleet** = one org, a hierarchy of operators each scoped to a subset of
  properties, credentials issued per-lease/booking.

## 2. The load-bearing principle

**The organization owns the keys; operators are revocable, delegated clients —
never key owners.** An operator's phone is a *leaf credential authorized
per-property by OZKEYSERV*, not the holder of a lock's root key. Get this right
and every enable/disable and revocation scenario is a server-side row change
instead of a truck roll.

### 2.1 Root authority — the one decision that shapes the tree

| Model | Root holder | Dispute behaviour | Use when |
|---|---|---|---|
| **Company-root** | the management company / hotel | operators depend on the company to hand back control | single org, hotel, company is trusted infra |
| **Owner-root delegation** (recommended for multi-party) | each property **owner** | owner revokes the company's delegation and reclaims doors unilaterally; company can't be held hostage either | management company running many owners' properties |

**Recommendation: owner-root delegation** for the property-management case — it
protects both sides in a service-fee dispute and is the sovereignty story that
differentiates from Tuya/TTLock (where the platform holds everyone hostage). A
**hotel** is naturally **company-root** (the hotel owns its own doors). The
server must support both; the difference is where the delegation tree is rooted.

## 3. Identity, tenancy & RBAC

```
Org (root tenant)                         ← hotel, or management company
 └─ Region / Group           (optional)
     └─ Operator             (front-desk clerk | property manager) = role + scope
          └─ Property = Lock  (owned by the org; = a hotel "room")
               └─ Tenant      (guest | renter) = credential holder (PIN/RFID or revocable app key)
```

- **Principal = role + scope.** An operator's `scope` = the set of properties
  they may operate (a manager's ~100 units; a front-desk clerk's whole site).
- **Locks belong to the org**, not the operator. Operators are granted scoped
  operate/issue rights; tenants are credential holders, never account holders.
- **Hotel = degenerate case:** one org, one site, front-desk operators, rooms as
  properties — so the hotel build (§4) needs only a thin slice of the full RBAC.
- Schema builds on the ozlockserv/ozkeyserv tables: `orgs`, `sites`, `operators`
  (+ roles), `properties`/`rooms` (mirror), `credentials`, `audit_log`
  (app-attributed, per ozkey-05 console work), `revocations`.

## 4. Property / room roster sync — `POST /pms/rooms` (the near-term build)

The concrete piece being built now for the hotel (XF-43 §7.0/§11). MAOI (or the
OZKEY app) is the **sole writer for property definitions**; OZKEYSERV is a
**read-only mirror** for pairing + command routing (XF-27 single-writer). This
also **retires OZKEYSERV's room auto-seed** (the ozkey-02 lab seeded 101–110;
the PMS is now the source of truth).

### 4.1 Contract

Base: `http://<onprem-host>:3200/ozkeyserv/api`. Idempotent; one endpoint, two modes.

**Request** `POST /pms/rooms`
```json
{
  "site_id": "default",
  "mode": "upsert",              // "upsert" (partial, default) | "reconcile" (full roster)
  "rooms": [
    { "id": "rm_a1b2",           // PMS stable row id — the join key (§4.3)
      "room_no": "101",          // display label; renameable without orphaning the lock
      "name": "Phòng Đôi 101",
      "type": "Đôi",             // room-type LABEL only (kits/rates stay in the PMS)
      "floor": 1,
      "capacity": 2,
      "lock_device_id": "ozk-…"  // optional; the binding, carried in-band (§6)
    }
  ]
}
```

**Response**
```json
{
  "ok": true,
  "upserted": 12,
  "deactivated": 1,              // reconcile only
  "adopted": [                   // pre-PMS rows claimed by room_no (§4.2)
    { "room_no": "101", "mac_address": "AA:…", "lock_device_id": "ozk-…" }
  ],
  "conflicts": [
    { "room_no": "103", "issue": "removed room has a bound lock + 1 live guest PIN",
      "lock_device_id": "ozk-…", "action": "kept inactive — resolve at the room" }
  ]
}
```

### 4.2 Semantics
- **`upsert`** (auto-on-change push): insert/update the listed rooms only; rooms
  absent from the payload are untouched.
- **`reconcile`** (manual full-roster button): payload is the *complete* roster;
  mirror rooms absent from it are marked **`active = 0`** — **never hard-deleted**.
- **Adoption (added 2026-07-12).** A pushed room whose `room_no` matches a
  **pre-PMS row** (`maoi_id` NULL — old lab seed or cockpit-created) **claims
  that row** instead of erroring: the PMS `id` and labels are written onto it
  and its pairing state (`mac_address`, `mac_token`, `lock_device_id`) and
  credentials are **preserved, never overwritten**. Reported in `adopted[]`.
  A `room_no` held by a row **owned by a different PMS id** — or renaming an
  owned row onto a pre-PMS row's number (a two-row merge) — is still a
  `conflicts[]` refusal.
- **`lock_device_id` tri-state (added 2026-07-12).** Key **absent** → keep the
  stored binding (so pushes from an app that doesn't know the binding never
  clear it). Key present with **null/empty** → **guarded clear**: applied, but
  if the room has live credentials a `conflicts[]` warning names them (a paired
  room self-heals onto its MAC-derived §5 route). Key present with a **value**
  → bind — **unless the room has a paired lock and the value disagrees with the
  binding of record**, in which case the binding is refused with a `conflicts[]`
  entry (roster fields still apply): the lock adopted its id at pair time and
  drops legacy room-topic copies (§10), so overwriting would silently strand
  command delivery.
  *Reconciled with the app (XF-43 §7.4):* MAOI's bulk push **omits the key when
  unbound** — it never emits the null. A bulk null would clear bindings MAOI
  doesn't locally know (cockpit-paired rooms, a re-adopting replacement
  tablet). Deliberate unbind is the per-room `POST /locks/unpair`, not a push.
- **Removal is guarded, not silent.** Deactivating a room with a bound lock or
  live (unexpired) credentials does **not** drop the lock/queue/credentials — it
  returns a `conflicts[]` entry so the app warns the operator. Opposite of the
  lab's destructive `DELETE /locks`.
- **`POST /pms/reset` (added 2026-07-12)** — factory-reset the mirror for
  first commissioning: wipes rooms, credentials, pending queue, door logs and
  guest users. Never automatic: requires body `{"confirm":"ERASE"}` behind the
  §4.4 secret gate; the cockpit `:3300` exposes it as a type-to-confirm
  **RESET MIRROR** button so the wipe is always an operator decision.
- `GET /rooms` (unchanged) feeds the view-only cockpit; `GET /pms/rooms/status`
  → `{last_synced_at, room_count, active_count, bound_count}` for the sync
  button's status line.

### 4.3 Join key — RESOLVED (FtposPM, 2026-07-11): `id`
Store the PMS row **`id`** as the mirror PK; treat `room_no` as a mutable label.
Renaming 101 → "Suite A" updates the label without orphaning the bound lock or
in-flight credentials. FtposPM agreed strongly (XF-43 §11.5): `rooms.id` is
already a stable UUID v4 in MAOI and `room_no` is a display label operators *will*
rename — no MAOI change needed, it emits both today.

### 4.4 Write auth — required header
All `/pms/*` writes require a shared-secret header:
```
X-OZKEY-Secret: <configured secret>
```
OZKEYSERV rejects writes without a matching secret (`401`). FtposPM reserves the
host + secret in MAOI's "OZKEYSERV :3200 config" surface (XF-43 §7.2) and sends
it on every push. This is the v1 gate for the on-prem LAN deployment; the fuller
principal-auth/RBAC story is §9/§11. (Reads — `GET /rooms`, cockpit — stay open
on the LAN as today.)

**Scope widened 2026-07-12 (XF-43 §7.6 rule 4):** the gate covers **every
door-fact write**, not just `/pms/*` — `POST /locks/pair` and
`POST /locks/unpair` included. Every writer authenticates: MAOI holds the
secret for normal ops; the cockpit supplies it only under a declared emergency
takeover (§6.2). Unset secret = lab-open, unchanged.

## 5. `room_no` → `device_id` resolution (keeps the app simple)

The app references properties by **`room_no`** everywhere — roster, issue,
revoke, audit — *except* the one binding moment (§6). OZKEYSERV internally
resolves `room_no → bound lock_device_id` and routes to the lock. Consequence:
the device-scoped topic refactor (§10) is **invisible to MAOI / the OZKEY app**,
so their build never waits on it and never handles raw `device_id`s in the
credential path.

## 6. Binding — carried in-band

The property↔lock binding writes `lock_device_id` in the app's room editor (via
`ozkey_commissioner`, XF-42) and **rides the §4 roster push** — so definition and
binding stay in sync through one channel, no racing second write. OZKEYSERV
stores it on the mirror row and uses it for §5 resolution.

### 6.1 Doorlock replacement procedure (added 2026-07-12)

The room row is the stable thing; the lock is the replaceable part. Per the
§6.2 authority model the **normal actor is MAOI** ("Gỡ khoá" = unpair, "Ghép
khoá vật lý" = discovery+pair); the cockpit `:3300` exposes the same halves
(PAIR / UNPAIR in the pairing row) for the lab and for emergency takeover. The
order matters because **unpair is a server-side row change only — nothing is
sent to the old lock, and credentials burned into it keep opening the door
until they are revoked, expire, or the lock is factory-reset.**

1. **Revoke live credentials while the old lock still heartbeats** —
   `POST /pms/revoke-key` per credential (cockpit ROOM KEYS tab → REVOKE). The
   DPID delete frames queue and flush on the old lock's next heartbeat. If the
   lock is dead/being retired anyway, skip this and rely on factory-resetting
   the hardware instead.
2. **Unpair the room** — cockpit UNPAIR or `POST /locks/unpair {room_no}`.
   Clears `mac_address` + `lock_device_id` on the row (the `mac_token` is kept
   and reused at the next pair); the MAC returns to the discovery pool. The
   room keeps its roster identity, credential history and door logs.
3. **Swap the hardware; factory-reset the old lock** (LockSim: REGISTER wipes
   its provisioning). An un-reset old lock keeps heartbeating its stale
   identity — harmless to routing (the row no longer points at it) but noisy,
   and its on-device credentials still open it if step 1 was skipped.
4. **Pair the new lock to the same `room_no`** — it announces on the unpaired
   channel, appears in cockpit discovery, PAIR binds it. It gets a **new**
   `lock_device_id` (derived from the new MAC, or a PMS-pushed binding if MAOI
   re-pushed one after the unpair) and the §10 handshake moves it straight to
   device-scoped topics.
5. **Resync credentials — `POST /locks/resync {room_no}`** (gap closed
   2026-07-14): re-queues the DPID issue frame for every live, unexpired
   credential on the room, so the replacement lock receives the full set on
   its first heartbeat. Idempotent (skips already-queued), excludes
   expired/revoking rows. Still-`queued` jobs survive the swap anyway.
6. **MAOI binding — already handled by steps 2 + 4** (reconciled, XF-43 §7.4):
   "Gỡ khoá" (step 2) clears the server row AND the stale local id — so the
   mismatch guard won't refuse the next sync — and "Ghép khoá vật lý" (step 4)
   reads the server-assigned new `device_id` back and mirrors it locally. No
   separate re-bind step. (Only when the swap was done from the cockpit during
   an emergency does MAOI need "Khôi phục khoá từ máy chủ" to re-learn, §6.2.)

### 6.2 Authority model — adopted from XF-43 §7.6 (2026-07-12)

**MAOI originates · OZKEYSERV records · cockpit observes.** The full rule-set
and emergency-takeover procedure live in `XFtposDecisions-43.md` §7.6; this is
the server-side view of the three roles:

| System | Role | Writes in normal operation |
|---|---|---|
| **MAOI** (tablet) | Authority for operations | room defs · bindings (pair/unpair) · credential issuance · housekeeping |
| **OZKEYSERV `:3200`** | System of record + router | only server-minted facts (`device_id`, `mac_token`, `credential_id`, door logs) — MAOI reads them back |
| **Cockpit `:3300`** | Assisting tool | **nothing normally** — monitor/audit/sync-status; fallback writer only in a declared emergency |

Server-side consequences (all shipped):
- **Every door-fact write authenticates** — `X-OZKEY-Secret` gates `/pms/*`
  AND `/locks/pair` + `/locks/unpair` (§4.4). Single active writer per fact is
  an operating rule, not a lock: the gate + event log make the writer visible.
  Verified (2026-07-12, secret-enabled instance): pair/unpair 401 without the
  header, pass with it; reads stay open.
- **Declared takeover is a cockpit feature (BUILT 2026-07-12).** The cockpit
  header has **DECLARE EMERGENCY**: the operator enters the shared secret,
  which arms `X-OZKEY-Secret` on every cockpit write (pair/unpair/issue/
  revoke/reset) and shows a persistent **⚠ EMERGENCY WRITER** badge until the
  operator ends the takeover (secret cleared, cockpit reverts to monitoring;
  the end-of-takeover log line reminds the operator to run "Khôi phục khoá từ
  máy chủ" on the returning MAOI). Survives a page reload (localStorage).
  With no server secret configured (lab) cockpit writes remain open and the
  declaration is a no-op ceremony.
- **Bulk sync never clears** — the §4.2 tri-state (absent = keep) is what
  makes rule 3 safe; deliberate unbind is the per-room `POST /locks/unpair`.
- **Re-adoption on MAOI return** — a replacement tablet learns state back via
  `GET /rooms` (app: `serverBindings()` / "Khôi phục khoá từ máy chủ") and the
  push response's `adopted[]`; the server is the merge point for anything the
  cockpit did during the gap. It never blindly clears what it doesn't
  recognize — enforced server-side by tri-state + the mismatch guard.
- **RESET MIRROR is cockpit-only** (§4.2) — the nuclear wipe never lives on
  the tablet.
- **Physical lock = last resort** — with MAOI *and* server down, burned-in
  credentials still open the door; factory-reset + BLE re-commissioning
  (XF-43 §7.5) recovers the lock with no cloud dependency.

## 7. Credential lifecycle — mostly ALREADY BUILT

§7.1 hotel auto-issuance and the fleet's per-lease grants reuse verified endpoints
(ozkey-02 + gap #8):

| Action | Endpoint (exists) | App passes |
|---|---|---|
| Issue PIN/RFID (check-in / lease start) | `POST /pms/issue-key` | `room_no`, `guest_name`, `type`, `raw_value`, `slot_number?`, `date_from`, `date_to` |
| Revoke (checkout / no-show / lease end) | `POST /pms/revoke-key` | `credential_id` |
| Master / staff / housekeeping PINs | `POST /pms/issue-key` | staff validity window |
| Per-property access audit | `GET /locks/log?room_no=` | `room_no` |

Server builds DPID 21/22 frames, queues, flushes on heartbeat — done. Fingerprint
held (422). So the credential path needs **zero new frame/queue work** — only §5
resolution + the auth gate (§9).

## 8. Enable / disable — the core commercial ask

- **Disable an operator (an app/user):** flip the principal's `active` flag →
  session/JWT invalidated, per-device broker credentials (ACL) revoked, and a
  **revocation pushed to that operator's in-scope locks on next heartbeat** (the
  ozkey-06 §6 revocation-list pattern). Server authority dies immediately; any
  cached at-the-door BLE delegation on the phone dies within **one heartbeat
  cycle even fully offline.**
  *BUILT v1 (2026-07-14):* `POST /fleet/operators/:id/disable` — token refused
  instantly (403) and every live credential they issued auto-revokes via DPID
  delete frames on next heartbeat. Broker ACLs + BLE-delegation revocation
  lists are stage 2 (PIN/RFID revocation is fully covered).
- **Disable a door (a lock):** `suspended` flag → server refuses to queue new
  credentials; optional lockdown command purges/disables on-device credentials.
  *BUILT v1 (2026-07-14):* `POST /locks/suspend|resume` — issue refuses 409
  while suspended; the lockdown purge command is stage 2.

## 9. The two revocation scenarios, resolved

- **Operator leaves, keeps their phone:** disable their account → all in-scope
  authority revoked server-side instantly; **reassign their properties to a
  replacement via a scope change** (one DB op) — zero site visits. Their phone's
  cached credentials expire on next lock heartbeat. (The case OZLOCK can't do.)
- **Owner ↔ management-company service-fee dispute:** under **owner-root** (§2.1),
  the owner revokes the company's delegation and reclaims their doors
  unilaterally — no cooperation, no lawsuit over a locked database. Under
  company-root the owner depends on the company; hence the owner-root
  recommendation for multi-party.

## 10. Security & the device-scoped refactor

- **Keep the ozkey-06 e2e envelope**, but with **org/owner-escrowed keys in a
  KMS, not individual phones** — so a breach of the on-prem relay/DB doesn't leak
  PINs, while revocation stays central. Per-device broker credentials + ACLs fence
  each lock; signed command frames + monotonic counter; TLS off-LAN.
- **Device-scoped topic refactor (ozkey-04 §9):** do it as part of this work.
  Property/room becomes a *label*; routing is by `device_id`; renaming/rebinding
  never touches topic strings or live credentials. Internal (§5) — invisible to
  the apps.

## 11. Build status & sequence

| Piece | Status |
|---|---|
| issue-key / revoke-key / queue / DPID frames / lock_logs | **DONE** (ozkey-02 + gap #8, verified) |
| `POST /pms/rooms` (upsert/reconcile, conflict guard, in-band binding) | **DONE** — this doc §4 |
| `GET /pms/rooms/status`; retire auto-seed; cockpit read view-only | **DONE** |
| `room_no → device_id` resolution in issue/revoke/log | **DONE** (§5, 2026-07-11) |
| Write-auth gate on `/pms/*` (shared secret, LAN) | **DONE** — §9 |
| Device-scoped topic refactor | **DONE** (§10, 2026-07-11, bundled not fast-followed) |
| Fleet slice v1: operators (token auth) + scopes + enable/disable w/ auto-revoke + door suspend + persistent audit + `root_mode` recorded per org + `/locks/resync` | **DONE (2026-07-14, lab-verified)** |
| Fleet stage 2: owner-root reclaim ceremony, org hierarchy/regions, per-device broker ACLs, revocation lists for app-key (BLE) credentials | **NEW — remaining fleet work (§2.1/§8)** |
| e2e envelope with org-escrowed KMS | **ozkey-06 implementation + KMS** |
| BLE bootstrap firmware (XF-43 §7.5 asks): `ozkey_commissioner` ProvisionPayload for OZKEY (broker/site_id/mode), factory trust anchor (QR pubkey), Matter-takeover semantics, ESP32-C6 reference peripheral | **CONTRACT DRAFTED — ozkey-08 (2026-07-13)**: GATT profile v1, payload schema, trust anchor, phased build plan on the Waveshare ESP32-C6 Touch 1.47″; firmware starts after operator board bring-up. **mDNS `_ozkey._tcp` addressing SHIPPED + verified** in ozkeyserv |

**Sequence:** ship the **hotel slice first** (`/pms/rooms` + status + resolution
+ the §4.4 shared-secret gate + auto-issue reuse) — that unblocks XF-43 P7 and is
contained. Per FtposPM (XF-43 §11.5b), **`/pms/rooms` must NOT be gated on the
device-scoped refactor** — it's the only thing MAOI waits on. So the refactor
(§10) ships either bundled *or* as a fast-follow if bundling risks slipping the
endpoint; MAOI is neutral either way (§5). The **full managed-fleet RBAC +
owner-root delegation + enable/disable + revocation lists** is the larger second
stage for the property-management case.

## 12. Decisions

1. **Root authority** — company-root vs **owner-root**. **OPEN — operator's
   call** (the big one for the property-management case; recommend owner-root;
   hotel is naturally company-root). Shapes the whole delegation tree.
2. **Join key** — ✅ **RESOLVED (FtposPM): PMS `id`**, `room_no` a mutable label
   (§4.3).
3. **Device-scoped refactor now vs later** — ✅ **ozkey's call, non-gating**
   (FtposPM neutral). Do it now if it doesn't slip `/pms/rooms`; else fast-follow
   (§11).
4. **Write auth** — ✅ **RESOLVED: `X-OZKEY-Secret` header** on `/pms/*`, secret
   held in MAOI's config surface (§4.4).
5. **RBAC depth for v1** — ship the hotel single-org slice first, generalize to
   multi-operator fleet after (recommendation).

## 13. Conformance (hotel slice)

1. MAOI creates rooms → "Đồng bộ phòng" → `GET /rooms` on `:3200` mirrors them;
   the auto-seed is gone (no phantom 101–110).
2. Rename a room in MAOI → re-sync → label updates, bound lock + any live PIN
   survive (join-by-`id`).
3. `reconcile` with a room removed → it goes `active=0`; if it had a live guest
   PIN, the response `conflicts[]` names it and the lock is **not** dropped.
4. Check-in issues a PIN via `/pms/issue-key` referencing `room_no`; server
   resolves to the bound lock and flushes on heartbeat; checkout auto-revokes.
5. A write to `/pms/rooms` without the shared secret is rejected.
6. With a secret configured, `POST /locks/pair` / `POST /locks/unpair` without
   the header are rejected (401); the cockpit can only perform them after
   DECLARE EMERGENCY arms the secret, and reads stay open throughout.
