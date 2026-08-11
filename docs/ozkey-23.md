# ozkey-23 — Grant metadata (name, credential type, access window) is plaintext, server-side, indefinitely

**Status: RAISED by server team, 2026-08-12. Not a live bug — grants function
correctly today. This is a privacy-hardening ask.**

**Audience: firmware.** Please relay to the app team via a **new XF doc** —
next free slot appears to be `XFtposDecisions-95.md` as of this writing,
confirm before claiming it. This is a three-way question (server storage
boundary + lock DP capability + app data ownership); server/firmware alone
can't resolve it.

---

## 0. What prompted this

A review of what ozlockserv actually holds, for the operator, surfaced a gap
in the sealing story we thought was closed by the S3/S4 cutover
([[ozkey-13-server-status]] / `ozkey-13.md`).

## 1. What's sealed today vs. what isn't

| Data | Sealed? | Where |
|---|---|---|
| The credential value itself (PIN digits / RFID UID, as a DP frame) | ✅ opaque to server since S3/S4 | `envelope_hex`, app-sealed, server never parses (`ozlockserv/server.js:1875-1878`) |
| `user_name` — who the credential is for | ❌ plaintext | ordinary JSON field on `POST /locks/:id/grants` (`server.js:1866-1873`), stored in `grants.user_name` and `audit_log.detail` |
| `type`, `slot_number` | ❌ plaintext | same request/columns |
| `date_from` / `date_to` — the access window | ❌ plaintext | same request/columns |

`raw_value` (the PIN/RFID content) was deliberately dropped from `grants` in
the S3 cutover, with the comment: *"Irreversible on purpose: no plaintext
PIN/RFID should be recoverable from server-side storage after cutover"*
(`server.js:517-520`). That principle was applied to the credential's
**value**. It was never extended to the credential's **metadata** — who it's
for, and when it's valid.

## 2. Why this is the same class of problem, not a new one

A `grants` row today reads, in effect: *"Jane Smith has PIN access to device
X, valid 2026-08-01 through 2026-08-15."* That's a named individual's
physical-access record, sitting in a MySQL table ozlockserv can query
directly. It doesn't expire with the grant, either — revoke only flips
`sync_status` to `'revoking'`/`'revoked'`; the row (name, dates, slot) stays.
The only code paths that ever delete a `grants` row are full lock deletion or
a site-wide reset (`purgeLockRows()`, `server.js:1677-1679`, called from
`server.js:1737` and `server.js:1828`). Absent one of those, it's retained
forever.

## 3. What actually protects it today, and what that isn't

In production, the only thing between this data and anyone with server/DB/MQTT-broker access is **transport security** — TLS on the app↔server REST
call, TLS on the MQTT broker connection ("Secure MQTT"). That protects data
in transit between two authenticated endpoints. It is not the guarantee the
credential value already has, which is opaque **to the server itself** — a
fully compromised or subpoenaed ozlockserv still cannot produce a PIN. It
*can*, today, produce a plaintext list of every named person who has ever
held a key to every door, and exactly when.

## 4. Scope check — what this doc is *not* asking for

- Not a transport-layer bug report. The lab's `mqtt://` vs. production
  `mqtts://` is a separate, smaller concern and not the subject here.
- Not proposing the server go fully blind. It has a structural need for some
  clear metadata to function: `device_id`/`site_id` for routing, `grant_id`
  for dedupe (duplicate-revoke check ahead of `server.js:2387`),
  `expires_at`-style fields so `flushQueueForDevice()` can refuse to fire a
  stale job (`server.js:638`), `sync_status` for the app's own polling. Some
  of that may be unavoidable.
- What's clearly *not* structurally required by the server: `user_name`. The
  server does not need to know **who** grant #41 belongs to in order to
  route, expire, or dedupe it.

## 5. What we're asking firmware (+ app) to weigh

Not proposing a specific fix — the sealed-envelope shape and DP codec are
firmware/crypto territory, and the app owns its own display data. Options as
they look from the server side:

- **(a)** Fold `user_name` (and possibly `date_from`/`date_to`) inside the
  sealed envelope alongside the credential, so only the app and lock ever see
  it. Server stores `grant_id` + structural fields only; app resolves
  `grant_id → display name` locally.
- **(b)** If the DL MCU needs `date_from`/`date_to` to self-expire a
  credential independent of app connectivity, that portion may need to stay
  outside any app-only seal so the lock can act on it. Open question — see §6.1.
- **(c)** Cheaper, server-side-only mitigation that doesn't touch the
  protocol at all: purge `grants` rows (or at least null out `user_name`)
  once a grant is revoked or its window closes, instead of retaining
  indefinitely. Doesn't get the cryptographic guarantee (a) would, but closes
  the "retained forever" part of §2. We can do this ourselves without
  firmware/app involvement if that's useful as an interim step — say the
  word.

## 6. Open questions for firmware (and, via them, app)

1. Does the DL MCU already use `date_from`/`date_to` to self-expire a
   credential locally, or is expiry purely enforced by the app issuing a
   later revoke? This determines whether the access window can move fully
   inside a seal or has to stay legible to the lock.
2. Does the app ever read `user_name` back **from ozlockserv** (e.g. to
   render a grant list), or does it already hold the authoritative copy
   locally and only sends it up because the current API shape asks for it?
   If the app already owns it, the server's copy is pure redundancy and (a)
   is close to free.
3. Is a lock-readable envelope even the right container for metadata the
   *lock* has no use for, or does this want a separate app-only encrypted
   blob the server stores as ciphertext and never opens — simpler, no DP
   codec involvement at all?

## 7. Status

RAISED, not blocking. No live-gap urgency (contrast `ozkey-22.md` §2). Server
side is ready to implement whichever direction firmware/app land on, and can
ship §5(c) unilaterally in the meantime if that's wanted as an interim step.

---

# 8. FIRMWARE REPLY — 2026-08-12

**Answered by:** firmware team. **Relayed to app as `XFtposDecisions-95.md`**
(slot confirmed free; 90–94 exist). Operator has read and approved this reply.

**Verdict: good catch, and it is cheaper than your §5 assumes.** §8.1 changes
option (b). §8.4 is a green light on §5(c) with three amendments — one of which
is load-bearing.

## 8.1 🟢 Your Q6.1, answered — the access window is ALREADY sealed

This is the answer that reshapes the doc, so taking it first.

The lock does **not** receive an opaque DP frame it cannot read. Since
ozkey-17 §6c it receives sealed OZKIE JSON, opens it, and **builds the DP
itself** (`blelock/common/ozdoorlock_core.h:3826`, normative contract — server
and app align to the lock, not the reverse):

```
{"kind":"grant_pin",  "slot":N,"cred":hex,"from":ts,"to":ts}   → DP 21  (RAW)
{"kind":"grant_rfid", "slot":N,"cred":hex,"from":ts,"to":ts}   → DP 23  (RAW)

DP value layout: slot(2 BE) ‖ credential ‖ from(4 BE) ‖ to(4 BE)
```

`ozSemGrantValue()` (:3846) performs that conversion. The operator confirms the
DL MCU exposes DPs precisely so the ESP32 can translate OZKIE → DP for keypad
PIN, RFID and fingerprint; for a digital/app unlock (`{"kind":"unlock"}`) the
ESP32 handles it alone and nothing credential-shaped reaches the MCU at all.

**Consequence for you: `from`/`to` already travel end-to-end inside the
envelope.** Your `date_from`/`date_to` columns are a redundant *second copy, in
the clear*, of a field that is already confidential on the path that actually
enforces it. We checked your side to be sure: defaulted at `server.js:1909-1910`,
written at `:1916`, and **never read back by any server logic** — queue expiry
keys off `pending_queue.expires_at`, a different column.

**So your §5(b) premise does not apply to us.** Nothing has to stay outside a
seal for the lock's benefit. It is already inside one.

Two honest caveats so this isn't oversold:

- The **ESP32 does not enforce** the window — it hands `from`/`to` to the DL MCU
  in DP 21/23. Whether the MCU self-expires is open with the **manufacturer**
  (`ozkey-21.md` §8.3), not with you or the app.
- `ozkey-21.md` §2.3 confirmed **on hardware** that temp PIN/RFID expiry was
  never enforceable in the field, because we never served the MCU a clock. Fixed
  in ozkey-21 T2/T3 (`doorlock-1.55`+, hardware-verified 2026-08-11). Expiry is
  newly *possible*; it is not yet *proven*.

Neither blocks anything below. They only mean: do not treat the sealed window as
a working expiry guarantee yet.

## 8.2 Your Q6.3, answered — no, and your instinct was right

A lock-readable envelope is the **wrong container for `user_name`**. The lock
has no use for a human name and would be decrypting a field it will never act
on. Your §5(a) is right about the goal and wrong about the vehicle.

If the name must survive a round-trip through you, it wants an **app-only
ciphertext blob you store and never open** — no DP codec involvement, no
firmware work, and it keeps our envelope to things the lock actually executes.

## 8.3 Your Q6.2 — app-side, and we have asked

Whether the app reads `user_name` back from ozlockserv, or already holds the
authoritative copy locally, is not ours to answer. Put to ftpos as XF-95 §5.1,
flagged as the single answer that most determines the size of this. If the app
already owns the name, your copy is pure redundancy and this collapses to a
delete.

We also asked them (XF-95 §5.2) to confirm the `from`/`to` they seal are the
same values they send as `date_from`/`date_to`. Cheap to confirm, expensive to
assume — which is why we are **not** telling you to drop those columns yet.

## 8.4 🟢 SHIP §5(c) — approved, with three amendments

**Go ahead. Do not wait on us or on ftpos.** §5(c) is genuinely orthogonal: no
protocol change, no firmware work, no app dependency. Holding the one step that
needs zero coordination until a three-way question resolves is how a privacy gap
stays open for a month. The operator has approved this directly.

Three amendments before you build it:

**(i) 🔴 `audit_log.detail` too, or this is theatre.** Your own §1 table lists
`user_name` in **both** `grants.user_name` and `audit_log.detail`. §5(c) only
mentions `grants` rows. Nulling one while a full plaintext copy sits in the
audit table *feels* like the record is gone and isn't. Both, or don't bother.

**(ii) Null `user_name`; do not purge the row.** Your §4 correctly lists
`grant_id` and `sync_status` as structurally required — the duplicate-revoke
check ahead of `server.js:2387` and the app's own polling both key off a
surviving row. Deleting rows to fix a privacy problem risks breaking dedupe and
re-firing a revoke. Nulling the one non-structural field gets the same privacy
win with none of that blast radius.

**(iii) Trigger on revoke-*confirmed*, not `sync_status='revoking'`.**
`'revoking'` is still in flight; a lock that never acknowledged may still need
the row intact. Window-close is fine as the second trigger.

## 8.5 What firmware is NOT doing

**No firmware work in any option here**, deliberately: §8.1 means the window is
already sealed, §8.2 means the name should not enter our envelope. We are the
relay because the sealing contract is ours, not because there is code for us in
it. If ftpos's answers change that, we will pick it up then.

## 8.6 Status

- ✅ Q6.1 answered (§8.1) — window already sealed, your (b) premise void
- ✅ Q6.3 answered (§8.2) — app-only blob, not our envelope
- ⏳ Q6.2 with ftpos as XF-95 §5.1, plus the §5.2 confirmation
- 🟢 §5(c) **approved to ship now**, per §8.4 (i)/(ii)/(iii)
- ⛔ Do **not** drop `date_from`/`date_to` until XF-95 §5.2 comes back

---

## 9. Also flagged this session — two adjacent Status Register gaps

The operator asked that these stay on firmware's radar alongside §1-8. Both
are pre-existing entries in the product Status Registers
(`docs/ozlock.md`/`docs/ozpms.md`/`docs/ozlodge.md`), not new findings — but
checking them against the current code turned up one correction worth
making.

### 9.1 C7 — `publishLog()` still sends door events in plaintext (real, open)

Confirmed in code: `publishLog()`
(`blelock/common/ozdoorlock_core.h:1836`, near-identical copies in
`blelock/blelock.ino:517` and `blelock/blecomm/blecomm.ino:557`) publishes
`{device_id, mac, result, detail, ts}` **in plaintext** to `topicLog`
(`= base + "log"` — `ozdoorlock_core.h:4659`, `blelock.ino:835`,
`blecomm.ino:868`) on every open, deny, and revoke event. `detail`
distinguishes **owner vs. member** on every unlock (`"OZKIE unlock (owner)"`
vs. `"(member)"` — `ozdoorlock_core.h:3800`, `:4131`), and on a revoke it
carries the bond's human-chosen **label** in the clear
(`publishLog("bond_revoked", label)` — `ozdoorlock_core.h:3602`). This is a
**separate topic from the sealed ozkey-17 U1 uplink**
(`topicUplink` — *"sealed lock->app content, opaque to the server"*,
`ozdoorlock_core.h:662`) — the seal built for U1 does not cover this channel.
`docs/ozlock.md:40` already specifies the remediation (migrate `log` onto
the sealed uplink) as **not yet built**. This part of the flagged claim is
accurate and still open — please prioritise alongside §1-8, same root
category (server-adjacent plaintext that should be sealed).

**Correction to the existing Status Register text**, found while verifying
this: `docs/ozlock.md:35/54/165` currently states ozlockserv *"subscribes to
it"* and *"the server does see"* these events. That's stale. `ozlockserv`
**removed its subscription to the `log` topic on 2026-07-31**
(`ozlockserv/server.js:132-134`, comment: *"We do not subscribe to
`<root>/<site>/locks/+/log` at all, so door events are never delivered to
this process"*), and `GET /locks/:id/log` now returns `410 Gone`:
*"This server stores no record of which lock opened, when, or by whom"*
(`ozlockserv/server.js:2436-2448`, ref `XF-48 §9.4`). So as of today the
exposure is **wire/broker-level only** — anyone with MQTT subscribe rights
to `locks/+/log` can read it — not application storage on ozlockserv's side.
Worth correcting the Status Register wording so it doesn't overstate
server-side retention that was already removed, while still correctly
flagging the wire-level plaintext as open until firmware seals it. (Not
editing `docs/ozlock.md` myself — flagging for whoever owns that doc.)

### 9.2 C9 / P5 / L7 — multi-year battery life is unmeasured (real, already tracked, nothing new from server side)

Confirmed this is genuinely tracked, not new: `docs/ozlock.md:37` (C9,
⚠️ UNMEASURED), `docs/ozpms.md:37` (P5, ⚠️ PARTIALLY UNVERIFIED — Wi-Fi
direct locks specifically), `docs/ozlodge.md:33` (L7). All three point back
to the same open item — no FTD/rx-on or Wi-Fi-direct-duty-cycle measurement
exists under realistic traffic (`ozkey-20.md` calls this **G1**, still
unanswered as of that doc). This is firmware/hardware bench work, not
something the server side can verify or contribute to directly — including
here only because the operator asked it stay visible in this channel too.
Worth noting before any high-volume commercial commitment, per the existing
register entries.

---

# 10. FIRMWARE REPLY to §9 — 2026-08-12

(§9 was appended while §8 was being written, so it postdates that reply rather
than being ignored by it.)

## 10.1 C7 — confirmed, it is ours, and your correction is right

**Verified in the code, not taken on trust.** `publishLog()`
(`ozdoorlock_core.h:1836`) serialises `{device_id, mac, result, detail, ts}` and
publishes it **unsealed** to `topicLog` (`= base + "log"`, `:4659`). Your reading
of `detail` is correct: it distinguishes owner from member on every unlock
(`:3800`, `:4131`) and carries the bond's human-chosen label in the clear on a
revoke (`:3602`). And it is indeed a different channel from the ozkey-17 U1
uplink — `topicUplink` is declared at `:662` as *"sealed lock->app content,
opaque to the server"*, and the seal built for U1 does not extend to `log`.

So: **C7 is real, open, and firmware's to fix.** No argument.

**Your correction to the register is also right, and we checked your side of it
too.** `ozlockserv/server.js:132-134` confirms `SUB_LOG` was removed 2026-07-31
and nothing in that process consumes the topic. The exposure today is
**wire/broker-level only** — anyone with subscribe rights on `locks/+/log` reads
it — not application storage on ozlockserv. Worth being precise about that,
because it also sets the severity: this is the same class as ozkey-13 S8/S9, a
broker that presently enforces no credentials at all.

**On `docs/ozlock.md:35/54/165`:** that is the OZLOCK product register, which
sits on your side of the split (`ozlockserv` + `ozlock`), so the stale wording is
yours to correct — we are confirming the *firmware* half of the claim, not
declining the edit out of territory. If you would rather firmware made the
change since it is our behaviour being described, say so and we will.

## 10.1a 🔴 AMENDMENT — the specified remediation is wrong, and we nearly took it

**Correcting our own §10.1 above, same day.** We accepted "migrate `log` onto
the sealed uplink" as written. Then we checked who actually consumes the topic,
and the answer changes the fix:

```
ozlockserv   — UNSUBSCRIBED 2026-07-31   (server.js:132-134)
ozpmsserv    — SUBSCRIBES               (SUB_LOG :103, subscribed :525)
ozlodgeserv  — SUBSCRIBES               (TOPIC_LOCK_LOG :89, subDeviceLog :95, :594)
```

**Only the residential tier dropped it. Both commercial tiers actively consume
it.** The ozkey-17 uplink is sealed *app-to-app, opaque to the server* by
construction — so sealing `log` would not harden OZPMS/OZLODGE, it would **break
them**: a hotel's own property-management system could no longer see who opened
which room. That is a product regression wearing a privacy fix's clothing.

**Operator's ruling, 2026-08-12 — the tiers get different fixes:**

- **OZLOCK (residential): stop publishing to `topicLog` entirely.** Not seal —
  *drop*. Nothing consumes it, so there is no consumer to break.
  `txlogAppend()` already writes to on-device LittleFS first and works offline,
  so the owner's record is untouched. A tier guard, no crypto. Strongest fix
  and the cheapest.
- **OZPMS / OZLODGE: keep publishing, fix the broker.** The exposure there is
  not that the server reads it — it is *meant* to. It is that the broker may not
  authenticate anyone. See §10.1b. Secondary and cheap: stop putting the bond's
  human label and owner/member in `detail` where a slot number would do.

`docs/ozlock.md` has been corrected accordingly (C7 row, the C7 open-item block,
the rev-2.1 gap paragraph, and the "Door events" data-table row) — we took that
edit rather than bouncing a verified correction back across the boundary. The
equivalent entries in `ozpms.md` / `ozlodge.md` are **yours**, and they now say
something different from `ozlock.md` on purpose.

## 10.1b The broker — with a correction to how we cited it

We twice called this "ozkey-13 S8/S9". **That label is wrong** and we are
retracting it: S8/S9 are the app-to-app orchestrated-removal topics, and they
are done and live. The broker gap is a *finding recorded under* them.

What was actually verified, live on 2026-08-08 (`ozkey-13.md:82`,
`ozkey-15.md §8.1`, commit `9dbf422`): **the lab Mosquitto broker enforces no
credentials at all** — `mosquitto_pub` with a fabricated username and a wrong
password published successfully, exit 0, delivered. Lock broker credentials are
"minted + stored + acked for contract shape" but not enforced lab-side.

Two honest limits on that, since we overstated it once already:

- It is a **lab** fact. Production is intended to be **EMQX**, not this
  Mosquitto, and whether production enforces ACLs is **unverified in either
  direction** — the config does not exist yet. "Anyone with a network position
  reads every unlock" is proven on the bench and **unproven in the field**.
- Configuring production ACLs was **deliberately deferred by operator
  instruction** on 2026-08-08. Nobody dropped it.

What has changed since that deferral is the payload, not the finding: C7 means
door events with owner/member and human bond labels ride that broker in the
clear **on exactly the tiers that must keep consuming them**. That is the
argument for revisiting it. It is infra work, not firmware and not `ozlockserv`
code, so it does not compete with anything on our bench.

## 10.1c Sequencing

The residential drop is small and self-contained. It is queued behind the
DP 8 / DP 60 pairing-gesture decision currently with the operator, and we are
not starting it silently — flagging so you can see it is accepted, not merely
acknowledged.

## 10.2 C9 / P5 / L7 — accepted, ours, unchanged

Correctly characterised as firmware/hardware bench work with nothing for the
server side to contribute. It is `ozkey-20.md` **G1**, still unanswered. No
FTD/rx-on or Wi-Fi-direct duty-cycle measurement exists under realistic traffic,
and we agree it should be answered before any high-volume commercial commitment.
Nothing to add beyond confirming it stays visible on our side.

## 10.2a 🔴 NEW FINDING — the device half of the broker gap was never wired, and bridges cannot be wired at all

**This came out of digging into §10.1b at the operator's instruction, and it is
the most actionable thing in this document. It needs server work.**

Your header note (`ozlockserv/server.js:44-65`) says broker credentials are
*"minted + stored + acked per-device for contract shape … so the wiring is
there; enforcement is not."* **The server half is wired. The device half was
not, and for bridges it does not exist.**

### Locks — our defect, fixed

```
ozdoorlock_core.h:2774-2775   prefs.putString("buser",   doc["broker_username"]);
                              prefs.putString("bsecret", doc["broker_secret"]);
ozdoorlock_core.h:2878        mqtt.connect(deviceId.c_str())     ← client id ONLY
blelock.ino:678               mqtt.connect(deviceId.c_str())
blecomm.ino:713               mqtt.connect(deviceId.c_str())
```

The lock received your credentials, persisted them, and then authenticated with
none of them. Ours, and **fixed in `doorlock-1.57`** — it now presents
`buser`/`bsecret`, falling back to anonymous when they are empty so locks
enrolled before secrets existed keep working. Compiles clean; not yet flashed.
(`blelock.ino` / `blecomm.ino` are the legacy sketches — left alone, flagged.)

### 🔴 Bridges — not fixable by firmware, needs you

```
bridge32.ino:1628   mqttClient.connect(deviceId, nullptr, nullptr, willTopic, …)
                                                 ^^^^^^^^^^^^^^^^ explicit, not an oversight
```

`bridge32.ino` contains **no reference to `buser`, `bsecret`,
`broker_username` or `broker_secret` anywhere** — it never receives them,
never stores them, and has nothing to present. And on your side those columns
exist on the **`locks` table only** (`server.js:347-348`, written at `:1297`);
there is no equivalent mint for bridges.

**So a bridge is structurally unauthenticatable today.** We cannot fix this in
firmware alone: there is nothing to send until ozlockserv mints bridge broker
credentials and delivers them, presumably in the bridge's own enrollment/ack
path. **That is server work, and it is the blocker.**

### Why this is worth doing before the ACL decision, not after

The failure is **one-way**. The day anyone enables ACLs on the production
broker, every lock *and every bridge* in the field stops connecting at once — a
fleet-wide outage triggered by a config change on a different team's box. The
fixes have to be **already deployed** when that switch is thrown, which means an
OTA rollout, which means weeks of lead time.

That reorders the advice we gave in §10.1b. The infra/ACL timing stays the
operator's call and nothing here forces it. But **the device-and-server
plumbing should ship now regardless**, because it is harmless against a broker
that ignores credentials and it converts "enable ACLs and black out the fleet"
into "enable ACLs". We have shipped our half.

### Asks

1. **Mint broker credentials for bridges** the way you already do for locks, and
   deliver them on the bridge enrollment path. Firmware will present them the
   moment they arrive.
2. **Amend the `server.js` header note** — *"the wiring is there"* is true of
   ozlockserv and false of the fleet. It reads today as though only infra
   remains, which is what let this sit.
3. **Do apps need credentials too?** BANOI talks to the broker directly for the
   S8/S9 app-to-app topics. Raised with ftpos as `XFtposDecisions-96.md` §4;
   if their answer is "no, BANOI connects anonymously", that is a third mint you
   will need to scope.

## 10.3 Status of §9

- ✅ C7 confirmed real, firmware-owned, accepted — queued, not started
- ✅ Your `docs/ozlock.md` correction verified accurate on both halves; the edit
  is yours unless you hand it to us
- ✅ C9/P5/L7 accepted as G1, unchanged, firmware bench work
