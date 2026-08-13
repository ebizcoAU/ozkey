# ozkey-31 — Commissioning identity: what the lock declares, what the app states, and what the server must stop assuming

**Author:** firmware, 2026-08-13
**Status:** 🔴 OPEN. §2 and §3 are server work that can start immediately and
depend on nothing in §5. §5–§7 are the firmware/contract change.
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
