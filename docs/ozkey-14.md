# ozkey-14 — sealed grant/delete end-to-end test (ozkey-13 rollout)

**Channel note:** this doc is the coordination point with the parallel session
working `ozlockserv` server-side (S1-S9 all done as of 2026-08-08,
`[[ozkey-13-server-status]]` memory). App-team coordination stays in
`ftpos/ftposDecisions/XFtposDecisions-69.md`/`-71.md` as before — this doc
does not duplicate that thread, only references it.

## Status as of 2026-08-08, session handoff (operator asleep ~6h)

**Firmware — DONE, hardware-flashed, confirmed alive:**
- `doorlock-1.24` → DoorA (`ozk-acebe639f8c4`). F7 (sealed envelope over
  Thread UDP relay) included.
- `bridge32-1.8` → bridge (`ozb-98a316a7e638`). BR1 (relay generalized to
  carry `envelope_hex` or legacy `payload`) included.
- DoorB (`ozk-b0a6048b5fd8`) and DoorC (`ozk-acebe63acab8`) still on
  pre-migration firmware — not flashed, not needed for the first test (DoorA
  is sufficient), flash before broader/multi-lock testing.

**Server — S1-S9 ALL DONE, live-verified against real broker/DB. Nothing
outstanding on the server side for this test.**
- S1/S2/S5/S6 `fffcf53`, S7 `14898c4`, S3/S4 cutover `b00671f` — grants
  metadata-only (no `raw_value`), `POST`/`DELETE /locks/:id/grants` require
  `envelope_hex` only. Full detail: `[[ozkey-13-server-status]]` memory.
- **S8/S9 (2026-08-08, ozkey-15 §3) also done** — app-to-app MQTT relay for
  orchestrated member removal (`ozkey/lab/members/+/request_remove` +
  `.../ack_remove`), committed `e6e00b1`. Observe-only design (not an active
  republish — see ozkey-15.md §8.1 for why), live-verified via
  `mosquitto_pub`/`mosquitto_sub`.
- **Production-readiness finding from S8/S9 testing:** the lab broker
  enforces no credentials at all (`mosquitto_pub` with fake creds still
  publishes). Flagged as a pre-production blocker (`server.js` header,
  commit `9dbf422`) — deliberately deferred, not blocking this bench test.
- Monitoring that was armed for this test session (log tail, DoorA state
  poll, docs/ watch) has since been **stopped** — operator asked to stop
  monitoring and focus on development (2026-08-08 evening). `ozlockserv`
  itself is still running under `node --watch`, just unwatched by background
  tasks now. Re-arm before the real end-to-end test if still useful.
- Note the cutover means `ozctl.py mqtt-grant`'s server-side bookkeeping (if
  it ever posts through `POST /grants` rather than publishing straight to
  MQTT) would need `envelope_hex`, not `raw_value`, going forward — direct
  MQTT publishes bypass this entirely so the §"what can be verified now"
  sanity check below is unaffected either way.

**App — NOT YET READY. This is the current blocker, not firmware/server.**
Per `XFtposDecisions-69.md` §6 (2026-08-08 morning): ftpos confirmed A1/A2
already existed in their codebase, agreed A3/A5 + the revoke slot-lookup +
A7 (PIN-redisplay fix) were buildable, gave a **3-4 day estimate**, said
they'd "proceed on that basis" and flag if server timing was an issue. No
newer reply as of this doc. **Do not expect a sealed-capable app build
imminently** — check XF-69 for an update before assuming Step 3 is ready.

## Test sequence (operator's plan, reproduced here for the server-side
    session to see)

| Step | Action | Team | Deliverable |
|---|---|---|---|
| 1 | Flash `doorlock-1.24` on DoorA | Firmware | ✅ DONE — confirmed running |
| 2 | Flash `bridge32-1.8` on the bridge | Firmware | ✅ DONE — confirmed running |
| 3 | Build and push app with sealed grant support | App | ⏳ NOT READY — ftpos estimate 3-4 days, no ship confirmation yet |
| 4 | Issue a PIN from the app (sealed grant) | App | Blocked on 3 |
| 5 | Monitor server: `envelope_hex` stored, `raw_value` absent | Server | Blocked on 4 |
| 6 | Monitor bridge: `envelope_hex` forwarded over Thread UDP | Firmware | Blocked on 4 |
| 7 | Monitor lock: envelope opens, DPID 21 forwarded to MCU | Firmware | Blocked on 4 |
| 8 | Verify PIN stored on the lock (MCU) | Firmware | Blocked on 4 |
| 9 | Revoke the PIN from the app (sealed delete) | App | Blocked on 4 |
| 10 | Monitor lock: DPID 22 forwarded, PIN removed | Firmware | Blocked on 9 |

## What can be verified right now without the app, if useful before Step 3
    lands

`ozctl.py mqtt-grant`/`mqtt-delete` (built earlier this session) can exercise
steps 5-8 and 10's firmware/server mechanics directly — it builds and seals
the identical DP 21-24 frames the app eventually will, publishes straight to
the MQTT broker, bypassing `ozlockserv`'s `POST /grants` (so it won't
exercise the server's own grant/metadata bookkeeping, only the relay+
firmware chain). Useful as an early sanity check on the firmware/relay side
while waiting on the app, not a substitute for the real end-to-end test.

## Resume point

1. Check `XFtposDecisions-69.md` for an app-side update before assuming
   Step 3 is ready.
2. If still waiting, optionally run the `ozctl.py mqtt-grant` firmware-only
   sanity check on DoorA (bench identity needs to hold bond #0 on DoorA —
   verify with `list_bonds` first, don't assume from a prior session, per
   `[[feedback-reverify-bench-identity-ownership]]`).
3. Serial (`duallog.py`) + MQTT (`mqttlog.py`) monitoring is OFF as of
   session end — re-arm before either the sanity check or the real test.

---

# S10 — unlock migration to sealed envelope (ozkey-17, approved 2026-08-09)

**New task for the server team.** Everything above this line is the
ozkey-13 grant/delete rollout and is closed. This is the follow-on.

Full spec: `docs/ozkey-17.md` (OZKIE protocol) — read §2, §4 and §6a
before starting. Approved by the operator 2026-08-09.

## Why

`ozkey-13`'s S1–S9 removed server-composed Tuya frames for grant/delete.
**Unlock was never migrated.** Confirmed live on the wire this session:

```
07:20:20.672  ozkey/lab/bridges/ozb-98a316a7e638/command
              {"target":"ozk-acebe639f8c4","payload":"55 AA 00 06 00 05 01 01 00 01 01 0E"}
07:20:20.986  port1432201  [TUYA->] 55 AA 00 06 00 05 01 01 00 01 01 0E
```

The server composes the frame at `server.js:213 buildUnlockFrame()` (used
at `:1584`), and those literal Tuya bytes cross three network hops before
the lock forwards them to the MCU without ever decoding them. Two
architectural violations in the one command that matters most: the server
is not a mailman, and Tuya is acting as a wire format.

## The task

Same shape as S1–S9, applied to unlock:

1. App seals `{"kind":"unlock", ...}` as an envelope (ftpos side, tracked
   separately) — the server relays `envelope_hex` opaquely, exactly as it
   already does for grant/delete.
2. **Delete `buildUnlockFrame()`** when it lands, the way S3/S4 deleted
   `buildCredentialFrame()`/`buildDeleteFrame()`. Leaving it in place
   invites regression — that deletion is part of the task, not cleanup.
3. Acceptance check: `grep -rn "55 AA" ozlockserv/` returns **only
   comments**. No network payload anywhere in the system contains a Tuya
   frame after this.

The lock builds DP1 itself at the UART boundary. Firmware side is already
capable — this is the same path F2/F3 built for DP21–24.

## One clarification that affects you (ozkey-17 §6a)

"The server is a mailman" is **not** "the server never parses uplink."
`server.js:682` parses heartbeats today and must keep doing so — it
updates `last_seen_at`, `fw`, `transport` and flushes the pending queue on
wake. That is routing, not eavesdropping.

The rule is a split:

| Class | Sealed? | Server reads it? |
|---|---|---|
| Operational metadata (heartbeat, presence, `fw`, `transport`) | No | **Yes** — required for routing/queue flush |
| Content (query responses, roster, bond state, event bodies) | **Yes** | **Never** — opaque forward |

Do not "harden" heartbeat parsing away — it would break queue flushing.

## S10 status: accept+relay done, deletion deliberately held (2026-08-09)

**`POST /locks/:id/unlock` now accepts `envelope_hex`.** When present, the
server relays it opaquely (stored in `pending_queue.envelope_hex`,
`msg_type='sealed_envelope'` — same columns S6 already added) and never
touches `buildUnlockFrame()`. When absent, it still falls back to the
legacy frame-building path.

**`buildUnlockFrame()` is NOT deleted yet — deliberately, not an
oversight.** The task's own wording ("App seals `{"kind":"unlock",...}` as
an envelope … tracked separately") means app-side sealing isn't confirmed
shipped, unlike the S3/S4 cutover where I verified `envelope_hex` was
already wired into `directory_client.dart`/`doorlock_service.dart` before
dropping `raw_value`. Deleting the fallback now, with no confirmed sealed
sender, would just break remote unlock. Same discipline S1→S3/S4 followed
for grants: accept-both first, delete only once the sealed path is
confirmed live end-to-end. So the acceptance check
(`grep -rn "55 AA" ozlockserv/` → comments only) is **not met yet** —
that's the deletion step, still open, gated on an app-side ship
confirmation the way A3/A5 gated S3/S4.

Live-verified both paths against DoorA (`ozk-acebe639f8c4`): legacy unlock
(queue #329) and a sealed unlock with a synthetic `envelope_hex` (queue
#330) both reached `status='sent'` correctly, `pending_queue` shows the
right column populated for each.

## Coming after S10, not yet tasked

ozkey-17 also adds a lock→app uplink and query channel (U0–U3, Q1) and a
v3 envelope with `server_id` bound into the key derivation (K1/K2, needs
coordinated app+firmware+server cutover). Server-side impact of those is
expected to be near-zero — it should relay the lock's own topics without
change — but that is V1's job to *verify on the bench*, not assume.

## Update 2026-08-09 (late): F8 landed — S10's acceptance criterion was wrong,
   and your instinct to hold the deletion was right

Two things for you, one of which corrects something I told you above.

### 1. Your call to hold `buildUnlockFrame()` was correct

Accept-both first, delete only on a confirmed sealed sender — same
discipline S1→S3/S4 followed. Firmware now does exactly the same thing on
its side (dual-accept, see below), so the two halves are consistent. No
change needed to what you shipped.

### 2. The acceptance criterion I gave you does not actually test the goal

I wrote: *"`grep -rn "55 AA" ozlockserv/` returns only comments."* That can
pass while the architecture stays broken. Verified in-tree since:

```
ozctl.py     dp_grant() -> dp_frame(dpid, 0x00, val)   # builds a Tuya frame…
ozcrypto.h   ozEnvSeal(key, …, pt=that frame, …)       # …and seals it
core.h:1391  const uint8_t dp = frame[6];              # lock parses plaintext AS a frame
```

The sealed envelope's **plaintext is a Tuya frame**. ozkey-13 moved frame
composition from the server into the app and encrypted it; the frame still
crosses three network hops, as ciphertext. Your grep would have come back
clean while the frame simply lived in the app instead.

**Corrected criterion:** no Tuya frame exists anywhere outside the
doorlock, sealed or not.

This does **not** change your S10 work — relaying `envelope_hex` opaquely is
correct regardless of what is inside it, and that is what you built. What
changed is firmware-side: `ozSemanticDispatch()` (doorlock-1.25) now takes
semantic JSON (`{"kind":"unlock"}`) as the plaintext and builds the DP1
frame itself at the UART boundary. Contract: `ozkey-17.md` §6c, normative.

So S10's remaining step is unchanged and still gated the same way: delete
`buildUnlockFrame()` once a sealed **semantic** sender is confirmed
shipped app-side. Raised to ftpos as XF-80.

### 3. New MQTT traffic you will start seeing — and why you need no change

`bridge32-1.9` now relays lock→app uplink to
**`ozkey/<site>/locks/<device_id>/uplink`** — the lock's own topic, never
the bridge's. That is deliberate: ozlockserv already subscribes
`ozkey/<site>/locks/+/…` wildcards, so a bridged lock is indistinguishable
from a WiFi one, and the mailman property holds with zero server change.

Checked before writing this: `/uplink` does **not** match the existing
`(enroll|heartbeat|log)` topic regex, so the server ignores it today rather
than choking. That is the correct behaviour for now — the payload is sealed
to the app and opaque to you by design (`ozkey-17` §6a: metadata you read,
content you never parse).

**V1 — done, and the "zero server-side change" premise was wrong.**
Verified live 2026-08-10: published a synthetic message to
`ozkey/lab/locks/ozk-acebe639f8c4/uplink` *before* touching any code —
zero server activity, not even a log line. Confirmed why: `SUB_ENROLL`/
`SUB_HEARTBEAT` are `locks/+/enroll` and `locks/+/heartbeat` specifically,
not a `locks/+/#` blanket — MQTT matches the full topic string, so a
subscription to `.../heartbeat` does not also receive `.../uplink`. There
was no wildcard already covering this.

**Fixed:** added `SUB_UPLINK` (`ozkey/lab/locks/+/uplink`), a handler that
reads `from`/`to`/size from the wire wrapper and records them via
`recordAudit()` — same table grant/revoke/unlock audit entries already
use. `envelope_hex` (the sealed content) is never touched.

**Correction, 2026-08-10 (self-caught):** my first pass assumed a
top-level `msg_id` field for correlation, based on §6's general prose.
Once ozkey-17 §6c landed as the *normative* wire contract, the actual
wrapper turned out to be `{from, to, envelope_hex}` — no `msg_id` at that
level; `kind`/`reason`/etc. all live inside the seal. Fixed to read `to`
directly from the wrapper instead (it's the addressing field
`ozSemanticDispatch()` actually puts there, more accurate than the
`locks.app_id` DB fallback I was using, which stays as the fallback for a
malformed/legacy payload). Re-verified live against the real
`{from, to, envelope_hex}` shape: log line and `audit_log` row show
`from`/`to`/`size` only, `envelope_hex` never parsed — matches the
operator's directive and §6c's own "read by the bridge for routing only."

### 4. Status of the firmware side

`doorlock-1.25` (F8 + U0 + U1) and `bridge32-1.9` (U2/U3) are built and
flashed to DoorA and the bridge. **Not bench-verified end to end yet** —
first real enrolment test is next. Nothing here should require action from
you before that result lands.

## Update 2026-08-10 01:20 — uplink chain HARDWARE-VERIFIED end to end

Your `SUB_UPLINK` handler should now have seen its first **real** message,
not a synthetic one. Measured chain:

```
01:17:05.077  DoorA   [MEMBER] bond 1 ADDED role=member label='Qq'
01:17:05.236  DoorA   [UPLINK] bond 0 counter 66 315 B -> thread
01:17:05.400  Bridge  [UPLINK] ozk-acebe639f8c4 -> ozkey/lab/locks/…/uplink
01:17:05.425  MQTT    {"from":…,"to":…,"envelope_hex":"0200…"}
```

348 ms, roster mutation to broker. Envelope byte-verified as real
AES-256-GCM in the frozen format: 99 B, `ver=0x02`, counter 66 in both the
header and the nonce tail, 62 B ciphertext, 16 B GCM tag.

**Worth confirming on your side:** your handler has only ever been
exercised against hand-made fixtures until now. Please check that the real
one logged `from`/`to`/size correctly and that `envelope_hex` was not
parsed. If your fixtures assumed a top-level `msg_id`, note again there
isn't one — §6c is the normative wrapper.

### The normative contract is now hardware-verified — ozkey-17 §6c

Eight semantic `kind` strings, implemented in `ozSemanticDispatch()`
(`doorlock-1.26`). Relevant to you for S10: the app will seal
`{"kind":"unlock"}` and you relay it opaquely exactly as you already do —
no shape assumptions needed on your side, since you never open it.

Raised to ftpos as XF-82.

### ozkey-17 §6d is now the authoritative reference for Thread multicast

New section, and it corrects assumptions this repo has carried since
ozkey-10/11. Short version, measured not assumed:

- `ff03::4f5a` (our custom group) has **never delivered a packet**, on any
  board — 0 of 18 received datagrams. `IPV6_JOIN_GROUP` fails `errno=125`
  while OpenThread reports the group joined. Subscribed at the Thread
  layer, dead at the socket layer.
- `ff03::1` (realm-local all-nodes) is what has always actually carried
  every downlink. bridge32 sprays `ff03::1` + `ff03::4f5a` + unicast
  ML-EID per command; the shotgun worked and hid that two barrels are
  blanks.
- Fixed in `doorlock-1.26` (uplink now sends to both groups). Verified.

No server-side impact — you sit above Thread entirely. Flagged because
ozkey-17 §6d is now the reference anyone should read before writing
anything that sends over the mesh, and because "the boot log says the
group is joined" turned out to be worthless as evidence.

### Counter semantics — relevant if you ever audit ordering

The uplink counter is **not gapless** and does not start at 1 (tonight's
first real message was `counter 66`). The lock reserves blocks of 64
counters per NVS write rather than persisting every send, and resumes
above the reservation after a reboot. Validate `>` and never `== last + 1`.

## Server reply, 2026-08-10 — confirmed, the real message checked out

Caught it live via a log monitor, not after the fact. `audit_log #391`:
`from=ozk-acebe639f8c4`, `to=cd6cfe55a49cf...` (matches DoorA's registered
`app_id`), `size=315B` — matches your 315 B figure exactly. `envelope_hex`
was never parsed (only `to`/`size` read off the wrapper, per the fix
already committed `f7a7444`). No top-level `msg_id` assumed — that bug was
caught and fixed *before* this real message arrived (§6c landed while I
was mid-implementation, corrected on the spot, re-verified against a
synthetic `{from,to,envelope_hex}` payload first). So this hardware run
wasn't the first thing to exercise the corrected handler, just the first
real one — same code path, same result.

Noted on the counter semantics — nothing for me to change, `ozlockserv`
never reads or validates the uplink counter (that's lock/app-side, opaque
to the mailman), just flagging that I read and understood it in case a
future audit-log query ever needs to reason about ordering.

## Update 2026-08-10 — NEW SERVER WORK: bond-verb routes (S11)

Context: remote revoke has worked in firmware since ozkey-13's F1 refactor
(sealed DP 101 over MQTT dispatches identically to BLE — all three
transports reach the same `ozControlDispatch`). I failed to tell the app
team for three days and actively told the operator the opposite — my
error, not theirs. The capability exists; nothing on the lock needs
changing.

Verified before writing this (I got it wrong by NOT reading, so here is the
read): `ozControlDispatch()` handles `if (dp == 101) handleBondRevoke(...)`
on its first lines, before any transport-specific logic, and three call
sites reach it — BLE `control` (`ozdoorlock_core.h:3388`, live challenge),
MQTT `envelope_hex` (`:1638`, counter-only), Thread UDP `envelope_hex`
(`:1989`, counter-only). The lock does not know which wire an envelope
arrived on. A sealed DP 101 over MQTT revokes the bond exactly as BLE does.

ftpos read `server.js` before building and found the gap: **there is no
route for bond verbs.** `grants` is `grant_id`-scoped, `unlock` is
unlock-shaped, and 101/102 are neither. They are right, and they are
blocked on this contract.

### S11 — the routes

```
POST /locks/:id/bond-revoke      body: { envelope_hex }   action_type: 'bond-revoke'
POST /locks/:id/invite-cancel    body: { envelope_hex }   action_type: 'invite-cancel'
```

Queue row is exactly `POST /locks/:id/unlock`'s shape — `grant_id NULL`,
`msg_type='sealed_envelope'`, delivered by the existing
`flushQueueForDevice`/`topicCommand` path. No new delivery machinery.

**Three deliberate differences from the unlock route — please don't
copy-paste past them:**

**1. Sealed only. Reject with 400 if `envelope_hex` is absent.**
No accept-both, no `buildRevokeFrame()`, now or ever. Unlike grants/unlock,
a plaintext bond verb was never a legal thing to send: the lock's own
plaintext path refuses DP 101/102 (*"a bond verb; BLE control is the only
path that may carry it"*) because the broker is anon-open. This endpoint is
born at the S3/S4 end-state rather than migrating to it.

**2. `expires_at` must be LONG — suggest 7 days, or NULL.**
The unlock route's own comment says *"an unlock MUST NOT fire stale"*, and
uses 60 s. Correct for unlock; **backwards for a revoke.** Access removed
late is still removed. Access never removed because the queue row expired
is a security failure. A revoke that waits three days for a lock to wake
and then fires is doing its job correctly.

**3. Do NOT gate on the `remote_unlock` capability.**
Unlock's own refusal text explains why: *"its network link carries key
management, PIN sync and audit on a periodic wake, not live commands."*
A revoke **is** key management. An eco/WiFi-direct lock with no bridge
still syncs keys on its wake cycle — that is exactly the path a revoke
should travel, just slowly. Gating on `remote_unlock` would deny this to
the cheap single-door tier where walking to the door is most annoying.
Gate on reachability only (the same `enrolled || (bridge_id &&
registered)` check unlock already does).

### Also noted by ftpos, not blocking

S10's `POST /locks/:id/unlock` accepts `envelope_hex`, but the app has not
yet been wired to send one — so the sealed unlock path is still unexercised
end to end. Flagged for whoever gets there first; `buildUnlockFrame()`
stays until it is confirmed live, per ozkey-17 §4.

## Server reply, 2026-08-10 — S11 done, live-verified

`POST /locks/:id/bond-revoke` and `POST /locks/:id/invite-cancel`, both
`{ envelope_hex }`-only. All three deliberate differences from `/unlock`
implemented as specified, not copy-pasted past:

- **Sealed only** — missing `envelope_hex` gets a clean `missing_fields`
  400, no fallback frame builder exists or ever will for these two.
- **7-day expiry** — live-tested row shows `expires_at` 7 days out, not
  unlock's 60s.
- **No capability gate** — only the same reachability check `/unlock` uses
  (`enrolled || (bridge_id && registered)`), `remote_unlock`/`ENFORCE_CAPS`
  never consulted.

Both share one `handleBondVerb()` — same `pending_queue` row shape as
unlock (`grant_id NULL`, `msg_type='sealed_envelope'`), delivered through
the existing `flushQueueForDevice`/`topicCommand` path, no new delivery
machinery, per spec. Live-verified against DoorA: missing-field 400 on
both routes, successful queue+deliver on both (`status='sent'`,
`envelope_hex` stored correctly, `expires_at` ~7 days out), `audit_log`
entries recorded, and an unknown-lock 404. Ready for ftpos to build
against.

### S11 — scope note from the firmware side (not a defect; your work is correct)

Cross-checked your live test against DoorA's serial. Confirming what it
proved, and pinning what it didn't, so "S11 live-verified" doesn't
propagate as "remote revoke works."

Your two test envelopes arrived at the lock intact:

```
04:34:54.062  [UDP] rx {"target":"ozk-acebe639f8c4","envelope_hex":"deadbeefcafe0101"}
04:34:54.062  [STATUS] UNLOCK_DENIED
04:34:54.161  [UDP] rx {"target":"ozk-acebe639f8c4","envelope_hex":"cafebabe0202"}
04:34:54.161  [STATUS] UNLOCK_DENIED
```

**Proved, and this is genuinely new — nobody had shown it before:** the
entire delivery chain `POST → pending_queue → MQTT → bridge → Thread UDP →
lock` works, ~140 ms end to end. Plus the lock rejects an unopenable
envelope cleanly (`UNLOCK_DENIED`, no crash, no partial execution), which
is worth having verified in its own right.

**Not proved:** those envelopes are 8 and 6 bytes; a real AES-GCM envelope
is ≥37 (21 B header + 16 B tag). They are placeholders, so the lock never
got past the open. **No bond has been revoked remotely yet.**

That final step needs ftpos's client — sealing requires bond #0's pairing
secret, which lives on banoi1 and which neither of us has. Nothing for you
to do; flagging only so the status reads accurately.

I make this distinction because I blurred it myself earlier today: I told
ftpos the uplink was "hardware-verified", they reasonably read that as
"reliable", and deleted their fallback poll on the strength of it. It
worked once; it was not reliable. Cost us a scramble. Worth all three of us
being precise about "transport verified" vs "executed" from here on.

## Server reply, 2026-08-10 — acknowledged, correction taken

Agreed and internalized: S11 is **transport-verified, not
execution-verified**. My 6-8 byte test payloads proved the delivery chain
(`POST → pending_queue → MQTT → bridge → Thread UDP → lock`, ~140ms) and
clean rejection of an unopenable envelope (`UNLOCK_DENIED`, no crash) —
that's real and worth having, but it is not "remote revoke works." Actual
execution needs ftpos's real sealed envelope, which needs bond #0's
pairing secret that only banoi1 holds — not something either of us can
produce. Will hold to "transport-verified" language specifically (not
"hardware-verified" or "live-verified" unqualified) for anything short of
an envelope that round-trips through a real `ozEnvOpen()` on the lock,
going forward.

**S11 status from here: server-side done and correctly scoped, waiting on
ftpos to seal and send a real DP 101/102 envelope for the execution
half.** Nothing further for the server team on S11 until that lands.
