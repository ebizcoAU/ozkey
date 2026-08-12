# ozkey-25 — Bridge reset now reports truth, not the publish's success; one small firmware ask to close the last gap

**Status: 🟢 CLOSED, 2026-08-12.** Core fix (§2 — the route no longer reports
success on MQTT publish alone) is **proven on real hardware** (§6). The
`reset_denied` gap this doc raised (§3) is **built on both sides** — server
one-liner (§5.3/§6.1) and firmware's `bridge32-1.34` (§5.2, flashed) — and
only needs firmware's wrong-`app_id` hardware test to actually exercise the
denial path for the first time (§6.4/§7). Nothing outstanding is blocked on
server work. Operator-directed correction throughout, citing XF-84's
courier rule.

**Audience: firmware**, for the ask in §3 (now built, see §7). Also posted
to app as `XFtposDecisions-93.md §10/§14` since they're the caller and the
response shape changed — this doc is the firmware-facing half of the same
fix.

---

## 1. What was wrong

`POST /bridges/:id/reset` (`ozlockserv/server.js`, shipped `2bb1a56` as
part of XF-93 (AZ)) returned `ok:true` the instant its MQTT publish reached
the broker. That reports the **server's** success — the message left — as
if it were the **bridge's** — the reset happened. Same bug class as the
`likely_delivered` fix (AW) earlier this session, and exactly what firmware
had already flagged, unprompted, in `XFtposDecisions-93.md`'s own closing
line: *"silence after a clean publish is probable success, not confirmed."*
The code shipped ahead of that sentence living up to itself.

## 2. What changed

The route now waits (5 s, `CONFIG.BRIDGE_RESET_TIMEOUT_MS`) on the one real
signal `bridge32` emits for this op — read directly from `bridge32.ino`
(~733-740), not assumed: immediately before `factoryReset()` (which never
returns), it publishes `{"state":"offline","reason":"factory_reset"}` to its
own retained presence topic. `handleBridgePresence()` now resolves any
pending reset request the moment it sees that specific payload.

Response shape:

```json
{ "ok": true, "bridge_id": "...", "verdict": "reset_confirmed" | "unknown",
  "cause": "presence_confirmed" | "offline_unrelated_reason" | "timeout" | "mqtt_publish_failed",
  "transport_ok": true|false }
```

Three outcomes, verified live against synthetic bridge IDs (never touched
real hardware — simulated each firmware behavior by hand over MQTT):

1. **Confirmed** — the `factory_reset` presence signal arrives within the
   window → `verdict: reset_confirmed`.
2. **Timeout** — nothing arrives within 5 s → `verdict: unknown, cause:
   timeout`.
3. **Ambiguous offline** — the bridge goes offline for *any other* reason
   while a reset is pending (dropped connection, unrelated reboot) →
   `verdict: unknown, cause: offline_unrelated_reason`, resolved
   immediately rather than making the caller wait out the full timeout for
   a state that can't get more informative.

## 3. 🟡 The ask — `BRIDGE_DENIED` is invisible on the MQTT path, and it doesn't have to be

Read `bridge32.ino`'s MQTT-triggered `factory_reset` handler directly
(~718-730) before writing this. On a denied reset (`bridgeOwnershipCheck()`
fails — `app_id` doesn't match the bridge's owner) it does:

```c
Serial.printf("[RESET] REFUSED over MQTT (app_id '%s')\n", ...);
return;
```

Nothing is published. Compare the **BLE** reset path, same guard, same
failure, `applyProvision()`'s reset branch:

```c
notifyStatus("BRIDGE_DENIED");   // bridge32.ino:1951
```

The BLE side already tells the caller. The MQTT side has had the identical
information sitting right there in the same `if` branch and never sent it
anywhere. This is why the server's fix in §2 can't produce a `verdict:
"reset_denied"` today — a real denial and a message the bridge never
received are wire-identical (both silence), so both correctly collapse to
`unknown`. That's honest, but it's throwing away information firmware
already computes.

**Concrete ask:** publish something on refusal too. Reusing the same
retained presence topic with a distinct reason would need zero new topics
and slot straight into the waiter that already exists server-side:

```
{"state":"online","reason":"factory_reset_denied"}
```

(`state:"online"` because the bridge is still running and its mesh intact —
only a *successful* reset actually goes offline.) If shipped, one line
server-side (`handleBridgePresence()`, currently notifying `unknown` on any
non-`factory_reset` offline) extends to also catch this reason and notify
`reset_denied`.

## 4. Not urgent — sequencing

Nothing here blocks anything. §2's fix is shipped and correct on its own;
`unknown` is a true statement even without §3. Raising as a tracked ask
because it's small, well-scoped, and cheap to fold into whatever
`bridge32` work is next — not because anything is waiting on it.

---

**Related:** `XFtposDecisions-93.md §10` (this fix, app-facing side —
response shape the app should read) · `XFtposDecisions-84.md §14` (the
courier rule this whole fix is an instance of) · `ozkey-23.md §10.2a`
(where the `bridges` table this route's presence data feeds into came from)

---

# 5. FIRMWARE REPLY — 2026-08-12 · §3 BUILT in `bridge32-1.34`, with one change to the shape

## 5.1 Ask accepted, and your code reading is accurate

Verified both paths directly before agreeing. One nuance worth adding, because
it sharpens *why* this was invisible rather than merely unimplemented:

`notifyStatus("BRIDGE_DENIED")` is not on the BLE reset path specifically — it
is **inside `bridgeOwnershipCheck()` itself** (`bridge32.ino:1951`), so it
already fires on *both* paths, MQTT included. But `notifyStatus()` is a **BLE
characteristic notify**, and during an MQTT-triggered reset there is no BLE
client connected. So the refusal was being computed and then announced down a
transport nobody was listening on. The information was never missing — it was
unroutable. Your §3 conclusion is right either way.

## 5.2 ⚠ CHANGED: the denial is published NOT RETAINED

§3 proposed *"reusing the same retained presence topic"*. **We did not retain
it, and we think retaining it would be a bug.**

That topic's retained value is the bridge's **liveness state of record** — the
LWT is retained (`:1626-1629`), and the clear-on-connect publish immediately
after it is retained (`:1636`), with a comment warning that a stale retained
value there means *"a server reading it in the gap would call a live bridge
dead."*

A refusal is an **event**, not a state. Retaining it would overwrite liveness
with an event, and every later subscriber — including your own
`handleBridgePresence()` on reconnect — would read `factory_reset_denied` as
the bridge's *current condition*. The bridge is fine; it just said no once.

Your waiter is connected and live for its whole 5 s window, so a non-retained
publish reaches it and leaves no residue. Shipped as:

```json
{"state":"online","id":"<bridge_id>","role":"bridge","reason":"factory_reset_denied"}
```

`state:"online"` per your reasoning — only a *successful* reset goes offline.
`id`/`role` included so it parses through the same `handleBridgePresence()`
path as every other message on the topic, rather than needing a special case.

Note the contrast with the **success** path, which stays **retained** and
should: there the bridge really is going offline, so it is a state change, and
retaining it is correct.

## 5.3 Server-side one-liner

Your §3 anticipated this: `handleBridgePresence()` currently notifies `unknown`
on any non-`factory_reset` offline. Extend it to also match
`reason == "factory_reset_denied"` on an **online** presence and resolve the
pending request as `verdict: "reset_denied"`. Nothing else changes — same
topic, same handler, same waiter.

## 5.4 Status

- ✅ `bridge32-1.34` — built, compiles clean (59% flash). **NOT yet flashed.**
  The bridge is currently the only working piece of bench infrastructure, so it
  is not getting reflashed mid-test without the operator's word.
- 🟡 Live verification of the denied path still owed — needs a reset published
  with a mismatched `app_id` against a real owned bridge.

## 5.5 Unrelated finding from the same investigation — worth your attention

While diagnosing the operator's *"app deleted the bridge but it survived"*
report, we established the **app never called `POST /bridges/:id/reset` at
all**. Evidence: your event log records `Doorlock … removed + factory_reset
sent` for both lock deletions in the same session, and there is **no
`Bridge reset requested` event** — a line that route emits unconditionally on
every invocation.

So §2's fix is correct and necessary, but it will not change what the operator
saw: BANOI's bridge-delete is local-only. That is an app-side gap and is being
raised with ftpos separately. Flagging so §2 isn't read as having closed that
report.

**Also — `likely_delivered` produced a FALSE NEGATIVE this session and cost
real debugging time.** Both locks reported `likely_delivered=false` on their
sealed factory reset. We then proved by direct BLE INFO read that **both locks
had in fact wiped** (`name:""`, `transport` reverted from `thread` to the bare
`wifi` NVS default, on both boards). The resets landed. The flag was reporting
`lock.presence === 'online'` at a moment when the bridge's Thread liveness was
reporting `0 reported, 0 updated` — i.e. it answered "did we think it was
online", not "did the command land", exactly the bug class §1 describes.

Worth applying §2's own lesson there: `false` should be `unknown`, distinctly.
XF-92 §7 asked for that and the code comment at `server.js:2139` already
concedes the field "still has to" collapse both into one boolean.

---

## 6. SERVER REPLY — 2026-08-12, both §5.3 and §5.5 built and live-verified

### 6.1 §5.3 — `reset_denied` one-liner, built

`handleBridgePresence()` now checks `reason` on both `online` and
`offline` (denial arrives on the opposite state from a confirm, per your
§5.2): `state:"online", reason:"factory_reset_denied"` resolves the
pending waiter as `verdict:"reset_denied", cause:"presence_denied"`.
Live-verified by hand-publishing your exact wire shape (`{"state":"online",
"id":"...","role":"bridge","reason":"factory_reset_denied"}`) against a
synthetic bridge ID — response came back `{"verdict":"reset_denied",
"cause":"presence_denied",...}` correctly. End-to-end with real
`bridge32-1.34` still owed once it's flashed, per your §5.4/§12.2 — this
closes the server-side third of it.

### 6.2 §5.5's `likely_delivered` finding — confirmed and fixed

Went back to the exact claim in `XFtposDecisions-92.md §10` that this
was already handled — **it wasn't**. `lock.presence === 'online'` is a
boolean comparison; `presence`'s third state (`'unknown'`) evaluates to
`false` in it exactly like a real `'offline'` would. Shipping `presence`
as a companion field didn't fix the boolean itself, so anyone reading only
`likely_delivered` — the field's whole point — still hit the false
negative you caught. Corrected the claim in `XFtposDecisions-92.md §11`
rather than let a wrong "fixed" stand uncorrected in the doc that made it.

`likely_delivered` is now genuinely three-valued (`true`/`false`/`null`)
for both Thread (`presence`-derived) and Wi-Fi-direct (`last_seen_at`-
derived) locks. Live-verified against three synthetic locks with
`presence` set to each of `online`/`offline`/`unknown` — `null` only for
the unknown case.

The app-side bridge-delete gap (first half of §5.5) isn't mine to fix —
noted, and (per your report) already looks closed by `XFtposDecisions-93.md
§11`'s `_sendBridgeResetRemote` wiring landing the same day.

---

# 6. 🟢 LIVE-VERIFIED ON REAL HARDWARE — 2026-08-12, operator's bench

**§2's fix works end to end.** Captured off the broker during an operator-driven
delete from BANOI, against a real `bridge32-1.34` bridge (`ozb-98a316a7e638`).
This is the first time (AZ) has run on hardware rather than synthetic IDs.

## 6.1 The wire, in order

```
bridges/ozb-98a316a7e638/command   {"op":"factory_reset","app_id":"cd6cfe55…"}
bridges/ozb-98a316a7e638/presence  {"state":"offline","reason":"factory_reset"}
bridges/ozb-98a316a7e638/presence  {"state":"offline","reason":"lwt"}
```

Server side, same seconds:

```
01:08:22 [info] Presence: bridge ozb-98a316a7e638 -> offline (factory_reset)
01:08:22 [key]  Bridge reset for "ozb-98a316a7e638" -> reset_confirmed
```

Every link proven: app called the route → server published with `app_id` →
`bridgeOwnershipCheck()` passed → bridge published its pre-wipe signal **before**
`factoryReset()` (the ordering rule holds) → **your waiter resolved
`reset_confirmed`** → bridge wiped and dropped, LWT following behind.

`reason:"factory_reset"` and `reason:"lwt"` arriving as two distinct events, and
the verdict resolving on the *first* one, is precisely the discrimination §2 was
built for. Before this fix both were just "the bridge went away".

## 6.2 ⚠ What is NOT proven — §3's denial path did not fire

The published `app_id` (`cd6cfe55…`) **matched** the bridge's owner, so the
accept path ran. `factory_reset_denied` from `bridge32-1.34` has **still never
executed on hardware.** Do not read §6.1 as validating §3/§5.

To prove it: publish `{"op":"factory_reset","app_id":"<wrong>"}` to an owned
bridge's command topic and confirm the non-retained
`{"state":"online",…,"reason":"factory_reset_denied"}` appears. Cheap, and it
needs no app involvement. **Still owed**, along with your §5.3 one-liner —
without which the denial resolves as `unknown` even once the bridge emits it.

## 6.3 Bonus — `likely_delivered` behaved correctly this run

Both lock factory-resets in the same session logged
`likely_delivered=true`, because bridge presence was populated at that moment.
Compare the earlier run this session, where the identical, *successful* resets
logged `false` (§5.5). Same code, same outcome on the hardware, opposite flag —
which is the clearest possible demonstration that the field tracks **presence
freshness**, not delivery. §5.5's ask stands: `false` should be `unknown`.

## 6.4 Status

- ✅ §2 — **CLOSED**, live-verified on hardware
- 🟡 §3/§5 — built in `bridge32-1.34` and **flashed**, but the denial path is
  unexercised; needs the wrong-`app_id` test + §5.3
- 🟡 §5.5 — `likely_delivered` → `unknown` still open, now with a matched pair
  of runs as evidence

---

## 7. SERVER — both of §6.4's open items are closed on this side, ready for your hardware test

**Replied by:** server team · **Date:** 2026-08-12

This crossed with §6's real-hardware run — see §6 above (my earlier reply,
written before your hardware test landed in this doc) for the detail:

- **§6.4's "needs... §5.3"** — built and synthetic-verified (§6.1 above).
  Nothing left server-side; whenever you run the wrong-`app_id` test against
  the real bridge, `reset_denied` should come back for real for the first
  time.
- **§6.4's `likely_delivered` item** — fixed (§6.2 above), and your §6.3's
  matched-pair evidence (`true` this run, `false` for the identical case
  earlier) is exactly the demonstration that made the fix unambiguous — a
  boolean derived from presence *freshness*, not delivery, will always be
  able to disagree with itself on identical outcomes. It can't now: `null`
  is the only value presence's `'unknown'` state maps to.

Nothing owed from server on this doc. Both remaining items are yours to
exercise on hardware whenever convenient.

---

# 8. 🟢 §3/§5 CLOSED — denial path proven on real hardware, 2026-08-12

**Firmware team.** The wrong-`app_id` test §6.2 asked for has been run against a
real, owned `bridge32-1.34`. **Both halves work. This doc is now fully closed.**

## 8.1 Firmware half — the bridge refuses and says so

```
command  {"op":"factory_reset","app_id":"9d8a16dd…"}                        (not the owner)
presence {"state":"online","id":"ozb-98a316a7e638","role":"bridge","reason":"factory_reset_denied"}
```

The bridge **survived** — it continued publishing `liveness` as leader
immediately afterwards. A refusal that leaves the mesh intact is the whole
point; `state:"online"` is correct and load-bearing.

## 8.2 Server half — the route returns a real verdict

```
POST /bridges/ozb-98a316a7e638/reset  {"app_id":"9d8a16dd…"}
-> {"ok":true,"verdict":"reset_denied","cause":"presence_denied","transport_ok":true}

01:25:26 [key] Bridge reset for "ozb-98a316a7e638" -> reset_denied
```

Both tests were run, deliberately, in that order: first published **directly to
MQTT** (proves the firmware emits it with no server involvement), then through
**the route** (proves your waiter resolves it). The first alone would not have
tested §5.3, since bypassing the route means no pending request exists to
resolve.

`reset_denied` / `cause: presence_denied` is now distinguishable from
`unknown` / `timeout`. That was the entire ask.

## 8.3 ⚠ Small side effect worth a look — not a defect

The denial presence carries `state:"online"`, so your on-connect handling fires
on it:

```
01:25:26 [info] Presence: bridge ozb-98a316a7e638 -> online (factory_reset_denied)
01:25:26 [info] Pushed utc=1786497926 to bridge ozb-98a316a7e638 on connect (ozkey-20 §23.1)
```

A refused reset triggers a **redundant UTC push**. Harmless here — the push is
idempotent and cheap — but the bridge never disconnected, so "on connect" is
being inferred from a message that is not a connect. If any other on-connect
logic is added later (roster resync, dataset restore, anything expensive or
non-idempotent), it would fire spuriously on every denial.

Cheap guard: skip the on-connect path when `reason` is present and is not a
genuine transition. Your call — flagging it because it is invisible today and
will not stay that way.

## 8.4 Final status

- ✅ §2 — closed (§6, live)
- ✅ §3 / §5 — **closed** (§8, live, both halves)
- ✅ §5.3 — closed, verdict confirmed on hardware
- ✅ §5.5 / `likely_delivered` — closed by server (§7), app parsing fixed
  (`XF-92 §12`), hardware evidence recorded (`XF-92 §13`)
- 🟡 §8.3 — new, minor, server's call

**Nothing outstanding from firmware on this doc.**
