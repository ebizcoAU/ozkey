# ozkey-57: ozlock transit-only (retention)

- **Status:** design, 2026-11-19, ozkey-srv (session ozkey-cf). Branch `server/ozlock-transit-only` from main 801e03e. Built red-first, slice by slice (§7).
- **Rulings:** operator via PM nexus-46, 2026-11-18 (R2-R6, grants, revoke_state, grantee_id XF, per-profile). Audit evidence: nexus-lab-backups `ozkey_notes/2026-11-18_server_status_PM.md` §2 + R6.

## 1. Rule

ozlock is a relay.
- It keeps no record of door use.
- It keeps only what it needs to route, plus a 30-day security log.
- Every other byte lives in process memory for the length of one exchange.

## 2. What changes

| # | Ruled | Change | Red-first row (fails on main today) |
|---|---|---|---|
| R2 | queue rows deleted at their terminal state | Delete the `pending_queue` row on: sent (no confirm needed), lock refusal, expiry, or lock delete. Boot sweep: delete every non-`queued` row and every `queued` row past `expires_at`. Migration: delete existing `sent`/`expired` rows (lab: 70). | after a grant/revoke/factory-reset/assisted flush, no row of that job is left; after boot, no `sent`/`expired` row exists |
| R3 | no per-use audit rows; security events 30 d | Every `recordAudit()` call names a **kind**: `security` or `use`. ozlock's profile keeps `security` only; an untagged call counts as `use` there, so a new per-use row cannot leak by default. `use` = remote unlock sent / not sent (transport down), assisted unlock queued, uplink, caps_update. `security` = everything else: pair / unpair, grant, revoke, grant_failed / revoke_failed, reset, bridge-reset / reconcile(_rejected) / failover, bridge bond / unbond / key conflict, bond-revoke / invite-cancel / settings, `ozkie_edhoc`, **a refused unlock** (failed access). Console actions (own INSERT, with actor) are kept unchanged. `AUDIT_LOG_RETENTION_MS` 90 d → 30 d on ozlock. Migration (ozlock only): delete `unlock` rows that are not refusals, plus `uplink` / `caps_update`. | an unlock / uplink / caps report writes no audit row on ozlock; a refused unlock does; a 31-day-old row is purged; a static check: every `recordAudit(` call in the core passes a kind |
| R4 | drop `locks.last_mech_*` | Stop writing both columns, then DROP COLUMN (behind the existing `DROP_*` flag pattern). The result goes to the app in transit only. | after an unlock result, the locks row has no mech result/time |
| G | grants minimal: lock id + grantee id + expiry | Columns: `id, device_id, grantee_id, expires_at, revoke_state ENUM(NULL,'pending','failed')`. Drop `type, slot_number, sync_status, issued_by, site_id`. See §3. | see §3 |
| R6a | no per-use stdout lines outside debug; capped rotating log | Per-use `logEvent` calls (unlock sent / refused, uplink, caps, v2 handshake) go to level `debug`, which is off by default. Log file rotation with size + age caps (values in the PR, for the PM). | at default level, an unlock + a v2 relay produce no stdout line containing the lock id |
| R6b | production broker: log_type error/warning/notice, persistence off, rotation | Ship `ozlock/deploy/mosquitto.conf` for ozlock's own broker. A test parses it and asserts the three settings. | the conf test fails until the file exists with those settings |
| R6c | the lab broker conf is Nexus's | Not ours. Logged for its owner by the PM. | — |

## 3. Grants (G)

**Lifecycle.** A grant row exists only while the grant can open a door, or while its revoke is not yet confirmed:
- `expires_at` NOT NULL (time-limited): purged at expiry, by a minute sweep plus at boot.
- `expires_at` NULL (owner / family permanent key): allowed, no cap. Lives until revoked.
- Revoke call: `revoke_state = 'pending'`. The sealed revoke is relayed as today.
- **Lock confirms → the row is purged at once.**
- Lock refuses, or the queue expires: `revoke_state = 'failed'`. The row stays visible to the owner until a retry succeeds. Nothing is purged on the CALL alone, so an undelivered revoke can never look done.
- A second revoke while `pending`: 409 `revoke_in_progress`. While `failed`: allowed (the retry).

**Columns the relay does NOT need** (from the code on main 74331a6):
- type, slot_number: labels the app sends unverified. They were used only in log text and GET /locks/:id/grants. The sealed `envelope_hex` carries the real content.
- issued_by: an enrol default only.

**Red-first rows:**
- G1: the grants table has no type/slot/sync_status/issued_by column after boot.
- G2: a revoke the lock confirms leaves no row.
- G3: a revoke the lock refuses leaves `revoke_state = failed`, the row still listed; the retry, once confirmed, leaves no row.
- G4: a revoke with no answer until queue expiry → `failed`, never purged.
- G5: an expired timed grant is purged; a NULL-expiry grant is not.
- G6: compatibility, see §4: a grant without grantee_id is accepted before the cut-over date and refused after.

**Migration of existing rows:**
- revoked → delete;
- revoking → `pending`;
- revoke_failed → `failed`;
- the rest → NULL;
- `grantee_id` NULL (legacy);
- `expires_at` NULL (unknown, so treated as permanent until revoked).
- Lab: 1 row.

## 4. XF item for the app teams (TalkKim / TalkPOS owners)

**XF draft: ozlock grants API change (grantee_id; no labels on the server)**

1. `POST /locks/:id/grants` takes a new field `grantee_id` (string, ≤ 64, opaque: the app's own id for the person) and an optional `expires_at` (ISO 8601 UTC; absent = permanent until revoked).
2. `type` and `slot_number` are still accepted but **not stored**. The app keeps its own labels (PIN / RFID / slot) locally.
3. `GET /locks/:id/grants` returns `{id, grantee_id, expires_at, revoke_state}` only. type/slot are no longer returned.
4. Revoke: `revoke_state` = `pending` while in flight, `failed` if the lock refused or never answered. **The grant disappears from the list when the lock confirms.** Show a `failed` revoke to the owner with a retry.
5. **Compatibility:** until the cut-over date, a grant without `grantee_id` is accepted (stored NULL). From that date it gets 400 `grantee_id_required`. **Date: set by the PM after the apps ship (TBD).**

The server keeps an env/config switch `GRANTEE_ID_REQUIRED_FROM` (date). Row G6 tests both sides of it.

## 5. Public claim: proposed wording (the doc owner applies it to ozlock_v4.0 §2.2 + the whitepaper §4.1)

> ozlock is a relay. It does not record when, how or by whom your doors are used. To deliver
> messages it keeps each lock's registration (its id, its bridge and its connection login) and its
> latest health status (online, battery) while the lock is enrolled. It keeps each access grant
> (which lock, which person, until when) only until the grant expires or the lock confirms it was
> revoked. It keeps a security log of pairing changes, revocations, bridge resets and failovers,
> failed or tampered access attempts and staff actions for 30 days, so abuse can be investigated,
> and then deletes it.

- Every clause maps to a §2/§3 row. The doc owner should not apply the wording before those rows are green on main.
- §2.2 "the system's memory" goes.

## 6. Out of scope / open

- **Finding (2026-11-19):** ozlodge does not use ozserver-core at all (standalone `ozlodge/server/server.js`, its own tables). The core changes cannot reach it. Row P2 is therefore structural: ozlodge's server requires nothing from ozserver-core, and its suite stays green.
- **RULED (PM, 2026-11-18): per PROFILE.** ozlock = transit-only (this note). ozpms + ozlodge stay unchanged until ruled; ozpms keeps the door log by design (property managers need it).
  - The audit allow-list, the audit window, R2 queue deletion, the grants schema/purge and the per-use log level become **profile config** in the shared core (`createCore` CONFIG). ozlock sets the transit-only values; ozpms/ozlodge keep today's defaults.
  - Red-first rows P1 (ozpms) and P2 (ozlodge): their audit actions, 90 d window, queue rows, grants columns and stdout lines are unchanged after the change. Each is shown to catch a mutant that applies the ozlock profile to it.
- **Retention values still to set in the PR:** log rotation size/age.

## 7. Build order (each slice: red rows, then the fix, gate, one commit)

1. R3: audit kinds + ozlock 30 d + the static check; P1 (ozpms unchanged) + P2 (ozlodge structural).
2. R2: queue rows deleted at their terminal state (ozlock profile).
3. R4: `last_mech_*` not written, then dropped (ozlock profile).
4. G: grants minimal + revoke_state + purge on lock confirm + `GRANTEE_ID_REQUIRED_FROM`.
5. R6a: per-use logEvent → debug on ozlock + rotating capped log. R6b: `ozlock/deploy/mosquitto.conf` + its test.

New profile CONFIG keys have defaults equal to today's behaviour, so ozpms keeps working unchanged without a change of its own.
