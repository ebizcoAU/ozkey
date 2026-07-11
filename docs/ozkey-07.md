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
- **Removal is guarded, not silent.** Deactivating a room with a bound lock or
  live (unexpired) credentials does **not** drop the lock/queue/credentials — it
  returns a `conflicts[]` entry so the app warns the operator. Opposite of the
  lab's destructive `DELETE /locks`.
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
- **Disable a door (a lock):** `suspended` flag → server refuses to queue new
  credentials; optional lockdown command purges/disables on-device credentials.

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
| Full RBAC (orgs/roles/scopes), enable-disable, revocation lists, owner-root delegation | **NEW — the fleet build (§3/§8)** |
| e2e envelope with org-escrowed KMS | **ozkey-06 implementation + KMS** |

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
