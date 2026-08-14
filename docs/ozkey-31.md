# ozkey-31 — Commissioning identity: what the lock declares, what the app states, and what the server must stop assuming

**Author:** firmware, 2026-08-13
**Status:** §2 and §3 — server half CLOSED, see §9/§10 below. §5–§7 remain
🔴 OPEN, firmware/contract work.
**App half:** `XFtposDecisions-100.md` (raised same day).
**Corrects:** `ozkey-30 §6` — see §4.

---

## 1. The one sentence

Across provisioning, grant delivery and mesh identity, **one side states an
intent, the other infers something from the shape of what arrived, and nothing
ever compares the two.** Every finding below is that pattern.

---

## 2. 🔴 SERVER — `sync_status:"synced"` is asserted with no evidence

**Start now. Independent of everything else in this doc.**

`ozlockserv` marks a grant delivered when it has *published* it. It has never
had proof the lock stored it. Today we proved the cost: **every remote sealed
command to this lock had been failing since the path existed** — 
`ozControlOpen()` rejected every bare envelope on the first line, before any
crypto. All four failures were recorded `synced`. The server's record and
reality had been disconnected for weeks, and the server was the more confident
of the two.

**The lock can now tell you the truth.** Since `doorlock-1.63`/`1.64` a refusal
uplinks a sealed outcome, and — critically — the outcome **code** rides the
*plaintext* wrapper, so you can read it without opening the envelope. `1.61`
made `UNLOCK_OK` mean the MCU acknowledged storing the credential, not "we
wrote bytes at a UART".

**Asks:**
1. Consume the outcome code on the uplink wrapper.
2. `sync_status` becomes tri-state: `pending` → `delivered` (lock said so) /
   `failed` (lock said so, with its code). **Never** `synced` because a publish
   returned.
3. Surface `failed` somewhere a human sees it. A grant that silently never
   landed is the failure mode that cost us this week.

This is the same class as `likely_delivered` from ozkey-24 and needs the same
treatment: say what you know, not what you hope.

---

## 3. 🔴 SERVER — who durably holds the Thread dataset?

The Thread dataset (`network_name` / `ext_pan_id` / `network_key` / `pan_id` /
`channel`) **is the mesh's identity**. A replacement bridge given the same five
fields restores the whole house; a bridge that self-forms orphans every lock and
forces a BLE re-pair at each door. bridge32 has accepted a restore dataset since
2026-07-28 (`bridge32.ino:291`); the app has never sent one. That is XF-100 §2.

**The open question is ours, not the app's: if the phone is lost, what holds the
dataset?** Today the answer appears to be "only the phone". If so, a lost or
wiped handset is unrecoverable orphaning of every lock in the house — with the
hardware working perfectly.

**Asks:**
1. State whether `ozlockserv` currently stores the dataset for any lock/bridge.
   I could not find it and did not go looking through your schema.
2. If not, decide whether it should — and note the tension explicitly: this is
   **the key material for the customer's mesh**, and we sell sovereignty. An
   escrow that we can read is a different product than one we cannot. A
   client-side-encrypted blob we merely store is the obvious middle path.
3. For OZLODGE/hotel, the same question with a different answer likely — there
   is no consumer phone to be the system of record.

**This is a decision, not a task.** Operator's call if it comes to that; I am
flagging that today the answer is "nobody", and nobody is not a plan.

---

## 4. Correction I owe: `ozkey-29` is NOT fully implemented

`ozkey-30 §6` states ozkey-29 is fully implemented. **It is not**, and the wrong
claim is mine to correct.

- **Done:** `seq`, `dropped_before_seq`, `query_events`.
- **NOT done:** §5.1 — sealing events *at rest*.
- **NOT done:** records are still the legacy `{result, detail}` shape, not the
  `ozkey-28` `event.*` verbs.

Anyone building against ozkey-29's event log should read this section first.

---

## 5. Transport is inferred from payload shape (firmware)

`applyProvision()` decides what kind of lock it is from which fields are
present (`ozdoorlock_core.h:3380`): `network_key` → Thread, else `ssid` → Wi-Fi,
else fail. Documented at `:365` as the design.

Consequence: **`network_key` missing or empty while `ssid` is present makes the
lock permanently Wi-Fi**, `xport=wifi` in NVS. It then dials MQTT forever, and
looks like a Thread lock to everyone including the bench.

Firmware will accept an explicit `transport` field and refuse payloads whose
shape contradicts it, keeping inference as a fallback for existing app builds.

---

## 6. The peephole needs Wi-Fi **and** Thread — the contract cannot express it

Operator, 2026-08-13: the peephole doorlock uses **Wi-Fi for video** and
**Thread for short control packets**. Both at once.

Today the Thread branch `return`s before `ssid` is read (`:3446`), so sending
both silently discards the Wi-Fi credentials. `cfgTransport` is a scalar enum
and `isThread()` has **22 call sites** conflating two distinct questions — "is
the control plane Thread?" and "is there no Wi-Fi radio in use?". The peephole
needs the first true and the second false.

Firmware work: split control-transport from data-path, then audit all 22 sites.
**Server impact:** a lock may report more than one transport. Anything keying
off a single `transport` value needs to tolerate a set.

Product context (operator, 2026-08-13): the current firmware is a **co-processor
alongside a mass-market Tuya DL MCU** — the play against Tuya/TTLock. The
standalone keypad/panel features are staged for a **future single-chip lock
where the C6 does the MCU's job too**. So `hw_profile` must distinguish those
two, not just panel size.

---

## 7. Nothing confirms a lock joined the mesh it was told to

`ext_pan_id` is consumed at `:3413` and reported nowhere — no uplink, no
heartbeat, no status. Firmware will add `ext_pan_id`, live Thread role, and a
`hw_profile` to the identity block.

**Server ask (after firmware ships it):** store them, and flag a lock whose
reported `ext_pan_id` differs from the dataset it was provisioned with. That is
the only way an orphaned or mis-joined lock becomes visible before a user
reports a dead door.

---

## 8. What each side can start now, in parallel

| Who | Do | Blocked by |
|---|---|---|
| **Server** | §2 outcome-driven `sync_status` | nothing |
| **Server** | §3 answer the dataset-escrow question | nothing |
| **App** | XF-100 §2 send stored dataset on bridge replace | nothing (firmware ready) |
| **App** | XF-100 §5 verify `broker_tcp_port` spelling | nothing |
| **Firmware** | §5/§6 transport + capability split | §6 peephole shape answer |
| **Firmware** | §7 report `ext_pan_id` / role / `hw_profile` | nothing |

Independently of all of the above, `doorlock-1.66` bounds the blocking MQTT
dial (~18 s worst case → ~4 s, exponential backoff). That was today's
unresponsive-panel bug and is fixed, though **not yet verified on hardware** —
verification needs a Wi-Fi lock pointed at a blackholed broker.

---

## 9. SERVER — §2 implemented + live-verified, 2026-08-14

Read `ozReportOutcome()`/`ozUplinkSend()` in `ozdoorlock_core.h` directly
before writing anything server-side, to get the actual wire shape rather than
assuming one. Confirmed: as of `doorlock-1.65` the plaintext `code` field on
the uplink wrapper is **refusal-only** — `ENVELOPE_BAD_HEX`,
`ENVELOPE_NOT_OPENED`, `MCU_TIMEOUT`, `COUNTER_REPLAY`. `UNLOCK_OK` never
rides this channel; it only ever goes to `notifyStatus()`, which is
BLE-`chrStatus`-only and the server never sees it under any transport. So
there is no wire evidence available today for a positive "lock confirmed"
signal — `delivered` is implemented as a real state grants can reach, but
nothing sets it yet, honestly, because nothing on the wire earns it. If a
positive confirmation code is ever added to this same field, `ozlockserv`
needs a matching change — flagging that now so it isn't a second silent gap
later.

**Built in `ozlockserv/server.js`:**
- `flushQueueForDevice()` no longer writes `sync_status = 'synced'` the
  instant a `grant-key` job publishes. It stays `pending` and the server
  records which grant this device's next uplink outcome code should be
  attributed to (`lastGrantSentByDevice`, in-memory — correlation only, not a
  durability claim).
- New `handleUplinkOutcome()`, wired into the existing `SUB_UPLINK` handler
  alongside `logUplinkMetadata()`. Treats **any** non-empty `code` as a
  failure rather than matching a fixed whitelist of the four strings above —
  a whitelist is exactly one more place a refusal code you add later walks
  past unnoticed, which is the class of bug this whole doc is about. Marks
  the correlated grant `sync_status = 'failed'`, logs a `warn`-level event
  (visible on `/events`, i.e. surfaced to a human, not just a log line), and
  writes an `audit_log` row (`grant_failed`, device + grant id + code — no
  PIN/RFID content, matching `ozkey-29`'s zero-plaintext posture).
- `revoke-key` jobs are untouched — still flip to `revoked` on publish. Same
  underlying "publish ≠ confirmation" gap exists there too, but revoke wasn't
  in this ask and `grants.sync_status === 'revoked'` already gates a 409 on
  re-revoke elsewhere in the code (`api.post('/locks/:id/grants/:gid/revoke'`
  area) — changing that needs its own look, not a drive-by inside this fix.
  Flagging it here rather than silently leaving it for someone to
  rediscover the hard way.

**Live-verified against the real broker/DB** (synthetic lock
`ozk-synthtest01`, cleaned up after): a grant published normally now stays
`pending`, not `synced`. A synthetic `MCU_TIMEOUT` uplink flipped it to
`failed`, produced a `warn` event on `/events`, and an `audit_log` row
readable via the grants list API. A second outcome code with no pending
grant on record correctly warned ("no pending grant on record to attribute
it to") instead of misattributing to a stale one. Server restarted, PID
7114, bound `:4200`, real bridge/lock traffic (`ozb-98a316a7e638`,
`ozk-acebe639f8c4`) continued unaffected through the restart.

---

## 10. SERVER — §3 answered: no server-side dataset escrow

**Operator's decision:** do **not** store the Thread mesh dataset
(`network_key` etc.) on `ozlockserv`. The app is the durable store. This
closes §3's open question in favor of the sovereignty side of the tension you
flagged, not the recovery side — a lost/wiped phone with no other admin
device in the house does mean re-pairing every lock, and that is an accepted
tradeoff, not an oversight. `ozlockserv` stores no dataset for any
lock/bridge today (confirmed — nothing in the schema holds it), and nothing
changes that. If this needs revisiting later (e.g. a client-side-encrypted
escrow blob, the middle path you floated), that is a fresh decision, not a
default to drift into.
