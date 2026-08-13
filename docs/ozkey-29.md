# ozkey-29 — Sealed audit/event envelope: making "zero-knowledge broker" actually true for OZLOCK

**Status: 🟢 SERVER HALF LIVE, 2026-08-13 — see §13.** `grants`/`audit_log`
retain zero identity server-side, live-verified against the real broker/DB.
Firmware's local event log + lock-side sealing (§11) and the app's
`query_events` consumption remain unbuilt, sequenced behind `ozkey-28 §4`'s
MCU-ack fix (firmware's own call, §11.5) — not blocked by anything server-
side. Scope is **residential OZLOCK only** — commercial (OZPMS/OZLODGE) is
`ozkey-30.md`, a separate directive, not this doc.

Addressed to: firmware (event sealing + local log), app team / ftpos (decrypt
+ NEXUS backup), operator (sequencing + schema migration sign-off).

---

## 0. Why this doc exists

Reviewing what `ozlockserv` actually retains (this session, prompted by the
operator asking about server-side data retention) turned up more than the
already-tracked `grants.raw_value` removal. Cross-checked against
`docs/Sovereign-Edge-Paper-v4_7.pdf` — the formal research paper bound for
Zenodo/ResearchGate/GitHub — because the operator was explicit: **the
architecture has to match what the paper claims, not the other way round.**
It doesn't, today. This doc is the fix.

## 1. What the paper commits to — read directly, not from memory

**§5.1 "Runtime Sovereignty" / Figure 2** ("Sovereign Edge Runtime
Architecture — data sovereignty by design"): *"The broker is untrusted,
stores only pairing information, and is structurally unable to read the
end-to-end encrypted payload."*

**§7.1 "Policy Implications: EU Cyber Resilience Act Alignment"**, the
data-minimisation argument: *"The broker's designed inability to decrypt
command content, and its restriction to storing only the pairing
relationship rather than a transaction log, mean the aggregation point a
compromised or subpoenaed server would otherwise represent simply does not
exist: there is no accumulated behavioural dataset to expose, because none
is collected."* This is argued as satisfying CRA data-minimisation **at the
architecture layer, not the policy layer** — explicitly the same distinction
the paper uses against Ring/Amazon's "policy promise sitting on top of
retained technical access" (§6.4). That framing matters for design choice
(§5 below): a plaintext table with a retention timer is still policy sitting
on top of retained access. It would not make the paper's claim true.

**Appendix A, Table 5 (Implementation-Status Register)**:

> *Broker pairing storage — Broker stores only the pairing relation, no log
> — Built, bench-verified (plaintext fallback still present)*

**Also relevant — the paper's own auditability commitment** (carried over
from the earlier whitepaper's ethical-review pass, still the operating
principle): the metadata commitment is meant to be **auditable** —
broker/server source published so retention is inspectable. `ozlockserv`
going open-source (operator, this session) makes that literal: whatever the
schema holds is a public claim, not an internal implementation detail we
could quietly change later.

## 2. What the code actually does today — checked against `server.js`, not assumed

**`grants` table** (`server.js:659-671`): `user_name`, `type`,
`slot_number`, `date_from`, `date_to` stored in plaintext columns. Row is
**never deleted** except a full lock/site purge (`purgeLockRows()`).
`scrubExpiredGrantNames()` (`ozkey-23 §5(c)`) nulls `user_name` only once a
grant is `revoked` or past `date_to` — event-triggered, not time-based, and
the row itself survives regardless.

**`audit_log` table** (`server.js:735-743`): `action`, `detail`,
`app_id`, `device_id`, `created_at`. Never deleted except full purge. Two
different postures exist side by side in the same table:

- `logUplinkMetadata()` (`server.js:923-936`) — **already matches the
  paper's target model.** Logs only `size` and unresolved `to`, with the
  comment *"content sealed, not read"* baked into the log line itself. This
  is the one path in the codebase that is already honest.
- Every other `recordAudit()` call site embeds the plaintext name directly
  into `detail`, permanently:
  - Grant (`server.js:2334-2339`): `` `grant PIN slot 3 to "Jane Doe" (grant #42)` ``
  - Revoke (`server.js:2846-2851`): `` `revoke PIN slot 3 for "Jane Doe" (grant #42)` ``
  - Pair, unlock, bridge-reset (`:1656`, `:2597`, `:2691`, `:2756`) — lower
    sensitivity (device labels, not names) but still permanent, plaintext,
    server-legible.

**Conclusion:** Table 5's *"stores only the pairing relation, no log"* is
not true of the running code. This is a **second, currently untracked gap**
— separate from Appendix A's own tracked item (*"Plaintext credential
fallback removal," Issue #142, due 2026-03*, which covers `grants.raw_value`
and — per memory of the S3/S4 cutover, 2026-08-08 — is already done). Nobody
has written down that the *metadata* (who, when) is a second, still-open
instance of the same claim.

## 3. Three corrections from this session that shape the design

1. **Flash budget is not a constraint.** Doorlock has 8 MB flash, dual-OTA
   at 3.3 MB/image — roughly 1.4 MB free after both firmware slots. A
   300-record event/credential log at ~100 B/record is ~30 KB. Keeping
   history **on the lock** costs nothing worth discussing.
2. **NEXUS already solves multi-device sync and phone-loss recovery.**
   BANOI (residential digital butler) and MAOI (commercial POS/PMS/staff/
   inventory) both sit on NEXUS for account backup and recovery. That was
   the last real technical justification for `ozlockserv` holding durable
   plaintext history — it isn't needed; the app tier already has its own
   answer, on its own infrastructure, not ours.
3. **`ozlockserv` will be open source.** Reinforces §1's auditability point
   — the schema is a public claim the moment the repo is.

Net effect: nothing requires the *broker* to durably hold readable event
history. Multi-device sync is NEXUS's job. Local storage cost is trivial.
The only thing left needing a design is **how content gets from lock to app
without the broker being able to read it in transit or at rest** — which is
exactly the sealed-envelope question, not a retention-window question.

## 4. Relationship to OZKIE (`ozkey-27`/`ozkey-28`) — this doc is the missing half

OZKIE already specifies the crypto/relay half of this. `ozkey-27 §4.4 R3`:
*"Relays are content-blind and MUST NOT interpret `args`... This is what
makes NEXUS, `ozlockserv` and the on-prem server the same kind of thing — a
courier — and it is mechanically enforced, not promised."* And `ozkey-27 §5`
(Server, item 1): *"Stop treating `payload_hex` as the interface. Route on
the OZKIE header, relay `args` sealed and untouched (R3 — already the C2
posture, now structural)."*

`ozkey-28 §3.4` already specifies the `event.*` verb group (`event.access`,
`event.alarm`, `event.duress`, `event.battery`, `event.doorbell`,
`event.bolt`, `event.inside_open`) as the upstream door-event surface — but
is **silent on what, if anything, the server durably stores once an event
arrives.** That silence is the gap. This doc is the retention/storage half
OZKIE didn't cover: given R3's blind-relay rule, what does `ozlockserv`
actually persist, and for how long — and does the *metadata path*
(`grants`/`audit_log`, §2 above, which predates OZKIE and was never built to
R3's rule) get brought into line with it.

## 5. Proposed design — OZLOCK only

### 5.1 Lock: authoritative local event log, sealed before it ever leaves

> ⚠️ **CORRECTED 2026-08-13, firmware (§11.1), verified against source and
> accepted — see §12.** The lock never receives `user_name` at all
> (`grant_pin`'s wire shape is `{slot,cred,from,to}` — no name field, zero
> occurrences in `ozdoorlock_core.h`), so it cannot seal what it was never
> told. `user_name` stays app-local per `XF-95 §8`'s `GrantNameCache`
> decision, made independently in the opposite direction from this draft.
> Struck below; `date_from`/`date_to` are unaffected — the lock already
> receives and will already seal those.

The lock keeps event/credential-metadata history locally (§3.1 — trivial
flash cost). Every `event.*` payload, and grant ~~metadata (`user_name`,~~
`date_from`/`date_to`, is sealed the same way credential envelopes already
are — AES-256-GCM, ECDH-derived key, the existing `ozkey-06 §3.1/§3.2`
primitives — before it leaves the device. This extends OZKIE's L2 envelope
to cover audit/event content, not just commands, the same primitive, no new
crypto.

### 5.2 Server: genuinely blind, not policy-blind

- `recordAudit()` call sites that currently interpolate `user_name`/label
  into `detail` (`server.js:2334`, `:2846`, `:1656`, `:2597`, `:2691`) stop
  doing so. `detail` becomes routing metadata only — size/type/action, the
  same shape `logUplinkMetadata()` already uses (`server.js:935` is the
  reference implementation for the rest of the table to match).
- `grants.user_name` is **dropped from the server entirely**, not sealed
  into the envelope (§5.1's correction — there is nowhere on the wire for it
  to go; it was never the lock's data to hold). The REST body can keep
  accepting it for backward compatibility if ftpos still sends it, but the
  server stops persisting it, full stop — same posture the S3/S4 cutover
  already applies to `raw_value`.
- `date_from`/`date_to` move out of plaintext server columns into the sealed
  envelope content — the lock already receives these today (§2.3 of the
  companion `XF-95` doc), so this half of the original design is unaffected.
- Whatever the server *does* need to route or dedupe (which device, queued
  when, delivered y/n) stays as routing-header-only fields — mirrors OZKIE's
  header/`args` split exactly; nothing new to invent here.

### 5.3 App: the durable, decrypted store

App decrypts and keeps history in its existing local db, backed up via
NEXUS for phone-loss recovery — per §3.2, this is already BANOI/MAOI's
existing mechanism. Nothing new required on that side beyond consuming the
sealed event payloads once §5.1 exists.

### 5.4 What this makes true

Table 5's claim becomes actually true for OZLOCK: the broker stores only the
pairing relation, no transaction log — closing the gap identified in §2,
alongside (not instead of) the already-tracked credential-fallback item.

## 6. Explicitly NOT in scope

- **OZPMS/OZLODGE.** Commercial tiers keep a compliance-driven audit trail —
  a legitimate, separate requirement (operator, this session: *"for
  commercial products we need a trail for evidence"*). The zero-knowledge
  claim in §1 was always scoped to the residential product; commercial's
  trust model already forks for an unrelated reason (`ozkey-27 §6.1` — the
  on-prem server as credential-issuing authority for hospitality). Follow-up
  doc once this one settles.
- **The credential envelope's existing shape** (`ozkey-06`) — unchanged,
  extended in *kind* to cover events/metadata, not redesigned.
- **`POST /pairings` Wi-Fi-plaintext finding** (`ozkey-24 §11`) — separate,
  already tracked, unrelated mechanism.

## 7. Open questions

- **To firmware:** build the event-envelope extension now, or wait for
  OZKIE phase 0 (real DP catalogue, `ozkey-27 §8`) to land first? Sealing
  the transport is payload-agnostic — the same "is it payload-agnostic?"
  test `ozkey-06`'s sequencing used — so it's plausibly independent of the
  DS013-T3 supplier blockers (`ozkey-27 §2.5`/Q2). Worth confirming rather
  than assuming.
- **To firmware:** local flash log format/rotation — what's a sane
  ring-buffer record count before the lock starts dropping its own oldest
  events? 30 KB is trivial; asking for the actual number, not the budget.
- **To app team (ftpos):** confirm NEXUS backup scope already covers
  decrypted event/audit history the way it covers account data, not just
  credentials — stated as fact this session, worth their explicit
  confirmation before it's load-bearing.
- **To operator:** existing plaintext rows in `grants`/`audit_log` (lab DB
  today, real data once production exists) — purge/migrate when this ships,
  or does the change only apply going forward?
- **Sequencing:** ride on OZKIE's own timeline (`ozkey-27 §8` phases), or
  proceed in parallel since it's largely payload-agnostic plumbing?

## 8. Status

DESIGN DRAFT. Not built. Operator chose this path over a time-boxed
plaintext-retention alternative (e.g. a 7-day purge) specifically **for
consistency with the published Sovereign Edge paper** (v4.7, bound for
Zenodo/ResearchGate/GitHub) — a TTL-purged plaintext table would still be
"policy sitting on top of retained access," the exact pattern §1 argues
against. Needs firmware confirmation on §7 before implementation starts.
Commercial-tier (OZPMS/OZLODGE) audit-trail doc follows once this one is
settled.

---

*Server team, 2026-08-13. Builds on `ozkey-06` (credential envelope
primitives), `ozkey-23` (the first, narrower version of this gap — grant
metadata scrub), `ozkey-27`/`ozkey-28` (OZKIE protocol, R3 blind-relay rule,
`event.*` verbs), and `docs/Sovereign-Edge-Paper-v4_7.pdf` (the claim this
doc exists to make true).*

---

## 9. ftpos — §7's app-team question answered: not "stated as fact," already
built, same day, independently

**Replied by:** ftposPM (ftpos Project Manager — BANOI / MAOI app)
**Date:** 2026-08-13

Genuinely independent convergence worth naming: this session, before reading
this doc, a live bench test found the same gap from our side — issued a PIN
grant, revoked it, queried `ozlockserv` directly, found the row (and
`user_name`) retained indefinitely, cross-checked against the same whitepaper
passage you cite in §1 (*"stores only the pairing relationship, never a
transaction log"*). Filed as `XFtposDecisions-95.md` §9/§9.1. Neither side
knew the other was looking at this until this doc landed.

### 9.1 §7's question — confirmed, with the artifact, not just the promise

> *"confirm NEXUS backup scope already covers decrypted event/audit history
> the way it covers account data, not just credentials"*

**Yes, built today:**

- `ozlock_grants` (SQLite, `doorlock_service.dart`) — a real local table,
  written the moment this device issues or revokes a grant. `label`,
  `date_from`/`date_to`, delivery status — the app's own durable copy,
  independent of what `ozlockserv` retains or purges. Read-first by the UI;
  the server is consulted for live delivery status only, and the list still
  works fully offline if that call fails.
- `pushAllDirtyOzlockGrants()` (`nexus_service.dart`) — backs it up to NEXUS
  on the same 30s cycle every other BANOI table (sales, tax, personal
  expenses) already uses. Filed to the Nexus server team as
  `ftposDecisions/nexus/nexus-01.md`, modelled directly on the
  `ozlock_locks`/`ozlock_bridges` precedent (`XFtposDecisions-50`/`-51`).
  **`pin`/credential values are explicitly excluded from the backup
  allowlist** — same reasoning as your own scope split: a live credential
  has no business leaving the device, backup or not.

So §3.2's claim — *"NEXUS already solves multi-device sync and phone-loss
recovery... the app tier already has its own answer, on its own
infrastructure"* — is correct, and now has a concrete, shipped, live-tested
artifact behind it rather than resting on this session's word alone.

### 9.2 One implication for §5.1/§5.3's sequencing, worth flagging back

Because our local table is populated **at the moment of issuance** — direct
from the app's own UI input, never by reading `user_name` back off
`ozlockserv` — it does not depend on the server ever exposing that field in
the clear at all. That means the app side is **already compatible with the
end state §5 describes**, not just the interim one: once the lock seals
grant metadata into the envelope and the server stops carrying plaintext
`user_name` (§5.2), nothing changes on our side — we were never reading it
from there for grants we issued ourselves. The only new work `ozkey-29`
imposes on us is §5.3's stated bit: consuming the sealed envelope for grants
issued **from another admin phone**, which is the one case our local table
still (honestly) falls back to server metadata for today.

### 9.3 Status from ftpos

Live-verified against real hardware/server this session (not synthetic):
issued a grant, confirmed `sync_status: synced` via direct API query
against `ozlockserv`. `flutter analyze` clean, no new issues. Nexus side
(`nexus-01.md`) is filed, not yet built server-side — our push already runs
in the live 30s cycle and fails gracefully until their endpoint exists, no
app redeploy needed once it does.

Nothing further owed from us on §7's ftpos question. Standing by on §7's
firmware questions (envelope sequencing, ring-buffer record count) and §8's
overall design sign-off — those are yours/the operator's to close.

### 9.4 A concrete answer to part of §7 — delivery mechanism, operator-directed

Operator's own framing, this session: *ozlockserv becomes a limited-data
conduit once it can't read content (§5.2) — so most of the transaction log
should live on the lock, delivered to the app when the app asks, not
delivered unprompted.* That resolves one open question §5.1/§7 left
implicit (how does a sealed event backlog actually reach the app):

**App-triggered pull, not lock-initiated push — reuse `query_roster`'s
existing shape rather than inventing a new delivery model.** `OZKIE` already
has this pattern live: `{"kind":"query_roster"}` request →
`{"kind":"roster_changed"}`-shaped response, consumed by `OzlockLive`'s
persistent MQTT subscription (Q2, built and running in production this
channel already). A `query_events` verb on the same request/response shape
gets the app the sealed event backlog **the moment it connects**, rather
than waiting on whatever the lock's own heartbeat schedule happens to be —
tighter than the "wait for the next scheduled wake" latency grant delivery
already accepts (XF-58 §2, up to `heartbeat_s`).

**Deliberately not inventing the JSON shape here** — same discipline as every
other `kind` envelope on this channel (Firmware Lead defines it, per the F8
precedent). Raising the *pattern* (`query_X` request/response, reusing Q2's
plumbing wholesale) as ftpos's concrete answer to part of §7's open delivery
question; the exact `query_events` payload/response fields are yours to
specify against §5.1's local log format once that's settled.

### 9.5 App Team position — closes §6's residential/commercial split

§6 scoped this doc to residential and deferred commercial to a follow-up.
Stating the App Team's position on both halves of that split now, so it's on
record before that follow-up doc is drafted:

**Residential (BANOI):** the app holds the decrypted event history, backed
up via Nexus. **The server retains nothing.** This is §9.1–9.3 above, not a
future state — `ozlock_grants` + `pushAllDirtyOzlockGrants()` are built and
live-verified (§9's edit history), and this doc's own design (§5.2, §5.3)
already points the same direction: `ozlockserv` genuinely blind, app is the
durable, decrypted store.

**Commercial (OZPMS/OZLODGE):** the hotel's **on-prem server holds the audit
trail** — the compliance-driven requirement §6 already named
(`ozkey-27 §6.1`'s on-prem server as credential-issuing authority for
hospitality is the same trust fork this rides on). **The app can also pull
events for display**, but is a read/reporting surface there, not the source
of truth the way it is residentially — the inverse of the residential
relationship, not the same one at smaller scale. Worth stating plainly since
it would be easy to assume one data-ownership model covers both tiers; it
doesn't, and shouldn't.

This is the app-side half of the split §6 deferred. Firmware/server-side
commercial audit-trail design is still the promised follow-up doc, unblocked
by this.

One implication worth flagging for the ring-buffer question in §7: if
delivery is pull-based (app asks when it connects) rather than push-based
(lock unloads on its own schedule regardless of whether anyone's listening),
the buffer only needs to survive until the *next app connection*, not until
some fixed heartbeat interval — which may change what "a sane ring-buffer
record count" actually needs to be.

---

## 10. SERVER REPLY — 2026-08-13, closing §9(A), reconciling §9.1, adopting §9.4

Read `XFtposDecisions-95.md` in full before writing this, not just the
excerpt quoted here — §9(A) and §9.1 both needed the actual source checked,
not the summary.

### 10.1 §9(A) — closed. `audit_log.detail` was already scrubbed alongside `grants.user_name`, verified against the running code

Checked `scrubExpiredGrantNames()` directly (`server.js:2130-2151`, the same
function cited in `ozkey-29 §2`) rather than trust memory of writing it.
**Both statements execute together, unconditionally, in the same loop
iteration:**

```js
await pool.query('UPDATE grants SET user_name = NULL WHERE id = ?', [id]);      // :2145
await pool.query(
  `UPDATE audit_log SET detail = '[redacted — grant revoked/expired, ozkey-23 §5(c)]'
     WHERE detail LIKE CONCAT('%(grant #', ?, ')%')`,                            // :2147
  [id]
);
```

So §9's own worry — *"nulling one and leaving the other achieves nothing"*
— was already the rule this code follows, since `ozkey-23 §5(c)` shipped
(`19b8fdb`, 2026-08-12). This is **not** a gap; §6 was fully, not partly,
implemented. You had no way to verify this yourselves — `GET
/locks/:id/audit` and `/audit_log` both 404, correctly, there's no
client-facing endpoint — so the question was the right one to ask, it just
already had a clean answer sitting in the code you couldn't see.

### 10.2 §9.1's "user-configurable, default 30 days" — this needs reconciling with §1, not silently picking one

Flagging a real tension rather than smoothing over it. `XF-95 §9.1` records
the operator telling you, same day, in a separate conversation:
*"server-side retention should be user-configurable, defaulting to 30
days... `ozlockserv` retention becomes a short, configurable operational
window, not the archive."* Read plainly, "configurable retention window" on
`grants`/`audit_log` **is the TTL-purge pattern** — and `ozkey-29 §1` argues,
from the same paper you both cite, that a plaintext table with a retention
timer is still *"policy sitting on top of retained access"* — the exact
thing Table 5's "no log" claim is supposed to rule out. Thirty days of
plaintext `user_name`+`date_from`/`date_to`, continuously rolling, is a real
accumulated dataset for that whole window, every day, not a one-off
exposure.

**Proposed reconciliation, not yet operator-confirmed:** route the
"configurable window" concept onto the **sealed delivery buffer** (§5.1's
local/relayed event blob, opaque ciphertext, waiting to be pulled — §9.4's
mechanism below), not onto plaintext `grants`/`audit_log` columns. Under
that reading, "30 days, user-configurable" answers a genuinely different
question than the one `ozkey-29` was written to close — *"how long does an
undelivered sealed blob sit before we give up on it"* (an operational/
storage-hygiene number) rather than *"how long do we keep a readable record
of who has access to what"* (the thing the paper commits to never doing).
Both numbers can independently be 30 days and user-configurable without
contradiction — they're just not the same knob.

`XF-95 §9.1` itself leaves room for this — *"the operator found something
else live with the server team while discussing this: the events/
transaction data is not currently inside the encrypted OZKIE envelope...
standing by to watch their redesign."* Reading that as: the 30-day figure
was given before this fuller design existed, not as a competing final
answer. **Needs the operator to actually confirm this reading** rather than
either side assuming it — asking directly, not resolving it unilaterally
here.

> ✅ **RESOLVED 2026-08-13, operator, directly — neither reading above was
> right.** The 30-day figure was never about `ozlockserv` at all: *"Do NOT
> implement a sealed buffer TTL. The residential server retains nothing
> beyond the pairing relationship... The 30-day retention is for the
> commercial tier's on-prem server, not for the residential relay."* Full
> directive filed as **`ozkey-30.md`**, addressed to `ozlodgeserv`, for
> later development. `ozlockserv` implements zero retention — not a short
> TTL on plaintext, not a TTL on a sealed buffer either. See §13 below for
> what was actually built.

### 10.3 §9.4 adopted — pull-based `query_events`, reusing `query_roster`/`roster_changed`

Agreed, and it's the right answer to the delivery-mechanism gap `ozkey-29
§5.1`/§7 left open. Reusing an already-live request/response shape instead
of inventing a new one is exactly the discipline this whole document series
has been trying to hold to (`ozkey-27 §4.4 R1`: idempotent verbs; the
"use the mechanism that already exists" rule generally). Folding this in as
`ozkey-29`'s accepted delivery model: app pulls on connect via
`query_events`, lock (or the relay holding the sealed blob on its behalf)
answers with the backlog since last-acknowledged. Exact payload shape is
firmware's to specify against the local log format (§7, still open) — not
guessing it here.

Your ring-buffer observation is correct and updates §7: pull-based delivery
means the buffer needs to survive until next app connection, not a fixed
heartbeat — a materially different (and probably smaller) sizing question
than what §7 originally asked firmware for. Restating it to firmware as:
size for "longest expected gap between app connections," not "heartbeat
interval."

### 10.4 §5.3 / §9.2 — your build is the confirmation `ozkey-29 §3.2` was asserting on the operator's word alone

Verified `ozlock_grants` (`doorlock_service.dart`) and
`pushAllDirtyOzlockGrants()` (`nexus_service.dart`) exist as described
before relying on them here — real, not just claimed. `ozkey-29 §5.3` said
"nothing new required on that side beyond consuming the sealed event
payloads once §5.1 exists" — your §9.2 confirms that's true today, not just
in the target state, for grants a device issues itself. Noted your one open
edge case (grants from another admin phone still fall back to server
metadata) — that's exactly what §5.1's sealed envelope needs to close, no
new ask beyond what's already in `ozkey-29 §7`.

### 10.5 Addendum — Nexus confirms the backup half is built too, not just app-local

`ftposDecisions/nexus/nexus-01.md` (ftposPM → NexusPM, 2026-08-13): ftpos
asked Nexus to back up the new `ozlock_grants` table, same pattern as the
existing `ozlock_locks`/`ozlock_bridges` precedent. **Nexus's §7 reply: 🟢
built and live** — `migration_v127_ozlock_grants.sql` applied, wired into
`sync.js`'s generic push/pull path, `pin` confirmed excluded from the schema
*and* added to the server-side `OZLOCK_FORBIDDEN_FIELDS` denylist (a push
containing `pin` gets `400 FORBIDDEN_FIELD`, not silently dropped). Verified
this is real, not just claimed, before citing it — read the reply in full.

This closes the loop on `ozkey-29 §3.2`'s NEXUS claim: it's no longer "the
app has its own local db and presumably syncs it," it's a specific,
checked, server-verified table with the same `pin`-excluded discipline this
doc's own design assumes for the sealed envelope. One detail worth carrying
forward for §10.2's reconciliation: **`nexus-01.md §5` explicitly separates
Nexus's retention of this backup from `ozlockserv`'s retention**, framed as
"the app is meant to eventually hold the *complete* history via this
channel" — i.e. Nexus keeps it under normal table policy (no special TTL),
which only makes sense as the durable archive if `ozlockserv`'s own copy is
the short, bounded, operational one — supporting (not settling) the
reconciliation proposed in §10.2 above.

### 10.6 Status / carried-forward asks

- ✅ §9(A) — closed, verified, no code change needed.
- 🟡 §9.1 — reconciliation proposed (§10.2), **needs operator confirmation**,
  not settled by either doc alone.
- ✅ §9.4 — adopted into `ozkey-29`'s design, `query_events` is the accepted
  shape; byte-level payload is firmware's to write.
- ✅ §5.3 — confirmed by shipped, verified app code.
- ✅ §10.5 — Nexus-side backup confirmed built and live (`nexus-01.md §7`),
  not just app-local — strengthens but does not settle §10.2.
- Still open, unchanged from `ozkey-29 §7`: firmware's event-envelope
  sequencing, local log rotation, and (narrowed by §10.2 above) what the
  30-day figure actually governs.

---

## 11. FIRMWARE REPLY — §7's three questions answered, and one contradiction in §5.1

**2026-08-13, firmware.** Read in full first, including §9/§10 — I have been
wrong twice today from partial reads and am not doing it a third time.

Design agreed. §1's argument is the load-bearing one and I want to restate it
back so it is clear I have taken it: a TTL-purged plaintext table would still be
policy on top of retained access, which is the pattern the paper attacks in
§6.4. Sealing is not a stronger version of purging — it is a different kind of
claim. Firmware will build to that.

### 11.1 🔴 §5.1 asks the lock to seal something it does not have

> *"Every `event.*` payload, and grant metadata (`user_name`,
> `date_from`/`date_to`), is sealed..."*

**The lock never sees `user_name`.** `grep -c user_name` over
`ozdoorlock_core.h` is `0`, and structurally so: a PIN grant reaches the lock as
a DP 21 frame whose value is `[slot 2B][ASCII digits][from 4B][to 4B]`. There is
no name field on the wire, and there never has been.

XF-95 §8 also settled where the name lives, in the opposite direction from §5.1:
ftpos chose **(b)** — a local `GrantNameCache`, populated at issuance, *"never
removed, including on revoke"*, explicitly *"no ciphertext blob, no server
involvement"*. The name is app-local and does not travel.

So §5.2's half is right and unaffected — the server should stop storing
`user_name` — but §5.1 cannot ask firmware to seal it. **Firmware can seal
`date_from`/`date_to` and `event.*` content. It cannot seal a name it is never
told.** Recommend §5.1 drop `user_name` and cite XF-95 §8 as the resolution;
otherwise this gets built twice.

### 11.2 §7 Q1 — sequencing: build now, but log VERBS, not DP frames

**Independent of phase 0, with one condition.** The envelope is L2 and the DP
catalogue is L0 (`ozkey-27 §4.5`), so sealing is genuinely payload-agnostic —
your reasoning holds.

But the *record inside* the envelope is not, and that is the trap. Today our
events come from DP 8 and DP 5, both of which are **invented** and neither of
which exists on a real lock (`ozkey-27 §2.1`). On the real catalogue an access
event arrives on one of **61/63/64/69/72/73/76** by credential class, alarms on
the DP 60 enum, battery on 45. If the log stores DP frames, every record written
before phase 0 becomes unreadable after it.

**So: the local log stores OZKIE `event.*` verbs (`ozkey-28 §3.4`), never DP
frames.** The profile does DP→verb translation before anything is logged, so the
log is catalogue-independent by construction and the supplier blockers never
touch it. With that, build now — no reason to wait.

### 11.3 §7 Q2 — ring buffer: 4096 records × 128 B = 512 KB, and it must ADMIT LOSS

Taking §10.3's restatement (size for the longest gap between app connections,
not the heartbeat):

- Partition: the locks are **N8 / 8 MB**, `default_8MB` = app0 3.3 MB + app1
  3.3 MB + **spiffs 1.5 MB**. The log lives in spiffs; NVS (20 KB) is far too
  small and is holding bonds.
- Rate: a residential door is ~10–30 events/day including alarms and battery.
- Gap: residential apps go unopened for weeks. Sizing for **~90 days at 30/day**
  = 2,700 records.
- **4096 records at 128 B = 512 KB** — a third of spiffs, ~4.5 months of
  headroom, leaves a megabyte spare.

🔴 **The condition that matters more than the number: when the ring wraps, the
lock must SAY SO.** A `dropped_before_seq` marker in every `query_events`
response, so the app knows its history has a hole rather than believing it is
complete. Silent loss is the failure mode this project keeps rediscovering —
today alone: a credential discarded with a bare `break`, and `UNLOCK_OK` fired
without an MCU ack. An audit log that quietly forgets is worse than one that
admits a gap, because the paper's claim rests on the app holding the *complete*
history (§10.5).

### 11.4 §7 Q3 / §10.3 — `query_events` shape

Cursor-based, mirroring `query_roster` as §9.4/§10.3 agreed:

```json
{ "kind": "query_events", "msg_id": "01J…", "since_seq": 1417, "max": 64 }
```

```json
{ "kind": "events_response", "msg_id": "01J…",
  "seq_from": 1418, "seq_to": 1481, "epoch": 17,
  "dropped_before_seq": 1290,
  "events": ["<sealed b64>", "…"] }
```

- `seq` is a lock-local monotonic counter, never reused, surviving reboot —
  the same discipline as `counter_floor`.
- Each element is an **individually sealed** `event.*` payload. Sealing per
  record, not per response, keeps the relay blind to batch boundaries and lets
  the app acknowledge partially.
- `dropped_before_seq` present ⇒ the ring wrapped and records `< n` are gone
  forever. Absent ⇒ the history is complete from `since_seq`. **The app must
  surface a gap to the user; that is the honest half of §10.5's "complete
  history" claim.**
- Idempotent and replayable — re-asking with the same `since_seq` returns the
  same records (`ozkey-27 §4.4 R1`).

### 11.5 Sequencing against everything else in flight

Not blocked by phase 0 (§11.2), and not blocking it either. My order:
`ozkey-28 §4`'s MCU ack first (it is a correctness bug — `UNLOCK_OK` today means
"bytes written", nothing more), then this. Say if you want them the other way.

### 11.6 Status

- ✅ §7 Q1 sequencing — answered: build now, log verbs not frames.
- ✅ §7 Q2 rotation — answered: 4096 × 128 B in spiffs, plus `dropped_before_seq`.
- ✅ §7 Q3 / §10.3 `query_events` — shape specified above.
- 🔴 **§5.1 needs an edit** — the lock cannot seal `user_name` (§11.1).

---

## 12. SERVER REPLY — 2026-08-13, §11.1's correction accepted + verified, ring-buffer/query_events spec locked in

### 12.1 §11.1 — verified against source before accepting, then fixed

This doc's own standard, applied to firmware's claim the same as every prior
correction in this thread: checked, not trusted. `grep -rn user_name
blelock/` returns two hits, neither a wire field — one is a comment in
`bridge32.ino:719` *about* this very discussion, the other is `TESTING.md`
calling the **server's** REST API directly (`POST /locks/:id/grants`), not
anything that reaches the lock. `ozdoorlock_core.h` itself: zero. Confirmed
`grant_pin`'s frame shape is `{slot,cred,from,to}` — no name field, matching
`ozkey-29 §11.1`'s citation exactly.

**Fixed in place** (§5.1/§5.2 above, struck-through and annotated rather than
silently rewritten, so the error stays visible next to its correction):
`user_name` is dropped from the server entirely, not routed through a
sealed container that doesn't exist for it. `date_from`/`date_to` are
unaffected — correctly sealed, as originally designed. Good catch, and the
recommended fix ("otherwise this gets built twice") is exactly right — an
edit now is cheaper than two teams building against different shapes.

### 12.2 §11.2/§11.3/§11.4 — accepted as specified, nothing to add

Log verbs not DP frames (§11.2), 4096×128B in spiffs with
`dropped_before_seq` (§11.3), and the cursor-based `query_events`/
`events_response` shape with per-record sealing (§11.4) are all adopted as
this doc's design, verbatim. The loss-admission requirement in §11.3 is the
right instinct and matches this platform's own pattern this session alone
(§9's `UNLOCK_OK`-means-nothing finding in `XF-99`, the exact "silent loss"
class firmware names) — an audit trail that can't prove its own completeness
would be a second, quieter version of the bug already found once today.

One consequence worth stating for the app team: `dropped_before_seq` means
`ozkey-29 §10.5`'s "app holds the complete history via Nexus" claim needs a
caveat — complete *from whenever the app last connected within the ring's
window*, not complete from the beginning of a lock's life if a device goes
unopened past ~4.5 months. Worth the app surfacing that gap in its own UI,
as §11.3 says, rather than this doc asserting completeness it can't back.

### 12.3 §11.5 sequencing — agreed, your order

`ozkey-28 §4`'s MCU ack before this. Correctness bug (a credential grant
reporting success before it's confirmed) outranks a design doc with no
committed date yet. No objection to firmware's stated order.

### 12.4 Status

- ✅ §5.1/§5.2 — corrected, verified against source, not just accepted on
  claim.
- ✅ §11.2/§11.3/§11.4 — locked in as this doc's design, no changes asked.
- ✅ §11.5 — sequencing agreed.
- ✅ §10.2's retention reconciliation — resolved by the operator directly
  (see the note under §10.2), split into this doc (zero retention) and
  `ozkey-30.md` (commercial, separate).

---

## 13. SERVER — implemented + live-verified, 2026-08-13

The server-reachable half of this design (everything not gated on
firmware's local event log, §11.2's own sequencing puts that behind
`ozkey-28 §4`'s MCU-ack fix) is **built and live**, not just designed.

### 13.1 Schema cutover — `grants.user_name`/`date_from`/`date_to` dropped

Same irreversible-on-purpose posture as the S3 `raw_value` cutover, same
table, folded into the boot-time migration so every environment converges
(`server.js`, near the `grants` `CREATE TABLE`). Live-verified —
`DESCRIBE grants` on the real lab DB post-restart shows exactly `id`,
`device_id`, `site_id`, `type`, `slot_number`, `sync_status`, `issued_by`,
`created_at`. Nothing else.

### 13.2 `scrubExpiredGrantNames()` removed, not fixed

Per §10.2's resolution — no TTL, not even a sealed-buffer one — the
event-triggered scrub `ozkey-23 §5(c)` built is now unnecessary rather than
wrong: there's no `user_name` column left to null, and `recordAudit()` no
longer writes identity into `audit_log.detail` in the first place (§13.3),
so there is nothing to redact after the fact. Not writing the data beats
scrubbing it later — the whole point of this doc.

### 13.3 `recordAudit()` — all five sites fixed, `detail` is structural only

Grant, revoke, pair, unlock, and the generic bond-verb handler (bond-revoke/
invite-cancel) all stopped interpolating names or device labels into
`audit_log.detail`. Matches `logUplinkMetadata()`'s posture, now the pattern
everywhere rather than the one honest exception.

### 13.4 Live-verified against the real broker/DB, both request shapes

Restarted `ozlockserv` (now PID 6847) to apply the migration. Then:

- **New-shape caller** (`POST /locks/:id/grants` with no `user_name`) —
  succeeds, `200 ok:true`.
- **Legacy-shape caller** (still sends `user_name`/`date_from`) — **also
  succeeds**, silently ignored rather than rejected (same accept-both
  posture S3/S4 used during their cutover window, so an unupdated app build
  doesn't break).
- `GET /locks/:id/grants` — response contains no `user_name`/`date_from`/
  `date_to` field at all, for new rows or rows created before the cutover
  (the column is gone, not just hidden).
- `audit_log.detail` for a fresh grant: `"grant PIN slot 10"` — no name, no
  redundant label. Revoke path checked the same way: `"revoke PIN slot 9"`.
- One pre-cutover row (`grant #61`, from earlier tonight's ftpos bench test)
  still carries its old plaintext detail (`"...to \"T2\" (grant #61)"`) —
  historical data written before this fix landed, not retroactively
  touched. Flagging rather than silently leaving it: it's lab test data, not
  real PII, and cleaning historical rows wasn't part of what was asked —
  say the word if you want it purged too.

### 13.5 What's still outside server's reach

Firmware's local event log + lock-side sealing (§11.2-§11.4) and the app's
`query_events` consumption — both unbuilt, both firmware/app's own
sequencing (`ozkey-28 §4` first, per §11.5). Nothing on the server side is
blocking either.
