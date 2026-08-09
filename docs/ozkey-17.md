# ozkey-17 — OZKIE: our own protocol between app and doorlock, with
    ozlockserv reduced to a mailman and Tuya-hex confined to the MCU pin

**Status: APPROVED and PARTLY BUILT, 2026-08-09.** Approved by the operator
with five corrections folded in (§5e, §5c-bis, §6, §6a, §6b). **F8, U0, U1
are built in `doorlock-1.25`; U2/U3 in `bridge32-1.9`** — compile-verified
and flashed to DoorA + the bridge, but **not yet bench-verified end to
end**. §6c is the normative contract and is implemented, not proposed.
Remaining: S10 (server), K1/K2 + Q2 (ftpos), Q1 (firmware).
Written 2026-08-09. Operator decision the same day:
name the app↔lock protocol **OZKIE**; ozlockserv is "just a mailman";
Tuya hex commands are generated *inside the doorlock*, not on the wire.

Absorbs and supersedes `ozkey-16.md` (bridge32 MQTT uplink) — that doc
scoped a one-way push channel, which turned out to be the narrow case of
the two-way channel specified here. Do not implement ozkey-16 separately.

## 1. The problem this closes

The lock currently only ever **answers** a request that a phone puts in
front of it over an active BLE session. It never volunteers anything, and
nothing that is not physically standing at the door can ask it anything.

That single gap generated most of the 2026-08-09 bench session's XF
traffic. Worth stating concretely, because it is the justification for
this work:

- **XF-75 / XF-78** — an admin phone tapped "cancel invite" six times
  against a lock whose roster it could not query, because its local copy
  was stale and it had no way to ask. The lock answered correctly every
  time; nobody could see that but the serial log.
- **XF-77** — ftpos filed a suspected bond-table persistence bug. The
  actual cause was a revoke that *did* happen, which their side could not
  observe. It took my serial trace to resolve.
- **XF-72 / XF-73 / XF-74 / XF-76** — four separate retry/terminal-state
  bugs, all fundamentally "the app cannot find out what the lock's state
  is, so it re-drives an action and hopes."

None of these are app bugs in the deep sense. They are all the same
missing capability: **banoi1 can issue a QR code, but cannot ask the lock
a question.**

## 2. What OZKIE is

One protocol, spoken end-to-end between the app and the doorlock,
encrypted so that no intermediary can read or forge it:

```
APP (OZKIE) ──► OZLOCKSERV ──► BRIDGE32 ──► DOORLOCK (OZKIE ──► Tuya hex ──► MCU)
                 mailman        courier      the only place Tuya exists
```

Two properties define it:

1. **The mailman property.** ozlockserv (and bridge32) move sealed bytes
   they cannot read, cannot author, and cannot meaningfully alter. They
   route; they do not participate. This is the sovereignty claim from the
   Sovereign Edge paper made literal.
2. **Tuya is a local translation, not a wire format.** The Tuya `55 AA`
   frame is synthesized by doorlock firmware only at the UART hop to the
   strike-driver MCU, because that sub-board speaks nothing else. No
   network hop anywhere in the system carries a Tuya frame.

## 3. Current state, verified against the tree (not from memory)

Better than expected in two places, and worse in one.

### 3a. Grant/delete already satisfy both properties

The `ozkey-13` S1–S9 cutover did this already. `grants.raw_value` and
`buildCredentialFrame()`/`buildDeleteFrame()` are gone from
`ozlockserv/server.js`; the app seals an envelope, the server queues and
relays it opaquely, and the **lock** builds the DP21–24 Tuya frame locally
before the UART write. That is exactly the target architecture, already
live and hardware-verified.

### 3b. The lock→app crypto is already built — and completely unused

`ozcrypto.h` has both directions:

```c
// ozcrypto.h:593
static bool ozEnvKey(..., bool appToLock, uint8_t outKey[32]) {
  const char *info = appToLock ? "ozkey/app->lock" : "ozkey/lock->app";
```

`ozEnvSeal()` (ozcrypto.h:615) seals a **lock→app** envelope, and the
`"ozkey/lock->app"` key derivation exists. Grepping the whole tree, the
only caller of `ozEnvSeal()` is the frozen-test-vector self-test at
`ozcrypto.h:767`. **Nothing in production has ever sealed a message from
the lock outward.**

This is the single most important finding for scoping: the two-way channel
needs *transport and semantics*, not new cryptography. The hard, careful,
byte-verified part is done and sitting idle.

### 3c. Unlock is the one command still violating both properties

```js
// ozlockserv/server.js:213
function buildUnlockFrame() { ... }
// used at server.js:1584
const payloadHex = toSpacedHex(buildUnlockFrame());
```

Confirmed live on the wire tonight — the bridge relays literal Tuya bytes
the lock never decodes, it just forwards them to the MCU:

```
07:20:20.672  ozkey/lab/bridges/ozb-98a316a7e638/command
              {"target":"ozk-acebe639f8c4","payload":"55 AA 00 06 00 05 01 01 00 01 01 0E"}
07:20:20.986  port1432201  [TUYA->] 55 AA 00 06 00 05 01 01 00 01 01 0E
```

The server composes a Tuya frame (so it is not a mailman) and that frame
crosses three network hops (so Tuya is a wire format). Both violations, in
the one command that matters most.

## 4. S10 — migrate unlock to envelope-only

Straight port of the S1–S9 pattern to the one command it skipped. The app
seals `{"kind":"unlock", ...}`; the server relays it opaquely; the lock
decides what unlock means and builds DP1 itself at the UART boundary.
Delete `buildUnlockFrame()` when it lands, the same way S3/S4 deleted the
credential-frame builders — leaving it in place invites regression.

### 4a. The acceptance criterion in the first draft of this doc was WRONG

It said: *"After S10, `grep -rn "55 AA" ozlockserv/` should return only
comments."* That test can pass while the architecture stays broken, and
finding out why changed the shape of this work.

**What ozkey-13 actually achieved was moving Tuya frame COMPOSITION from
the server to the app, and encrypting it.** Verified in-tree 2026-08-09:

```
ozctl.py       dp_grant() -> dp_frame(dpid, 0x00, val)   # builds a Tuya frame…
ozcrypto.h     ozEnvSeal(key, …, pt=that frame, …)       # …and seals it
core.h:1391    const uint8_t dp = frame[6];              # lock parses plaintext AS a Tuya frame
```

So the sealed envelope's *plaintext is a Tuya frame*. It still crosses
three network hops — as ciphertext. Grepping the server would have come
back clean while the frame simply lived in the app instead.

This is not cosmetic. Sealing a proprietary wire format leaves every
command shaped by Tuya's single-byte DP id and fixed value layout — which
is exactly the constraint that prompted "we must have our own protocol."
Encryption without a protocol of our own is a locked door on a rented
house.

**Corrected criterion:** *no Tuya frame exists anywhere outside the
doorlock, sealed or not.* The 55 AA bytes are constructed by
`ozBuildDpFrame()` inside `ozSemanticDispatch()`, microseconds before
`tuyaWireSend()` pushes them at the strike MCU, and exist nowhere else in
the system — not on MQTT, not on Thread, not on BLE, not in the app.

## 5. Key derivation — the operator's four identifiers, done safely

The proposal was to derive the OZKIE key from `device_id`, `app_id`,
`server_id`, and a `secret_phrase`. The *binding* intent is right and
partly already implemented. The *key material* needs correction.

### 5a. What already exists

```
key = HKDF-SHA256(ikm  = pairing_secret,                    // X25519, never transmitted
                  salt = utf8(device_id) ‖ utf8(app_id),    // two of the four already bound
                  info = "ozkey/app->lock" | "ozkey/lock->app")
AAD = ver(1B) ‖ counter(8B BE) ‖ utf8(device_id)
```

So `device_id` and `app_id` are already mixed in, and `info` already gives
a coarse two-value purpose separation. Missing: `server_id`, a richer
purpose, and the pepper.

### 5b. Why the identifiers cannot *be* the secret

`device_id`, `app_id` and `server_id` are all public. Not theoretically —
they were on the wire in tonight's own logs:

| Value | Where it leaked tonight |
|---|---|
| `device_id` | Thread UDP payload `{"target":"ozk-acebe639f8c4"…}`, every MQTT topic, the QR code, the device label |
| `app_id` | An MQTT topic segment: `ozkey/lab/members/63e371206add95f3…/request_remove` |
| `server_id` | A URL path — visible in config, in DNS, to anyone on the network |

If the key were derived from those four values alone, the entire security
of OZKIE would rest on `secret_phrase`. Human-chosen → brute-forceable.
Baked into firmware → extracting it from one lock breaks every lock in the
fleet, and per `threat-model-bond-and-physical` we *assume* firmware
extraction rather than hoping against it. The open-source posture makes
that assumption mandatory, not pessimistic.

### 5c. The construction to adopt

Keep the X25519 pairing secret as the root — it is the only value in the
system that has never crossed a wire, computed independently on both sides
during the bond ceremony, and unique per pairing rather than per fleet.
Use the identifiers for what HKDF's `salt`/`info` fields are *for*:
context binding.

```
key = HKDF-SHA256(
        ikm  = pairing_secret,                      // X25519, never transmitted
        salt = utf8(device_id) ‖ utf8(app_id) ‖ utf8(server_id),
        info = "ozkie/v3/" ‖ direction ‖ "/" ‖ purpose)
```

This delivers exactly the operator's intent, cryptographically rather than
advisorily:

- A packet is bound to **one device**, **one app identity**, and **one
  server path**. Route it through a different mailman and it does not
  decrypt — not "looks wrong at the network layer," but mathematically
  fails to open.
- `purpose` gives per-command-class key separation (`unlock`, `grant`,
  `query`, `uplink`), so a captured envelope of one kind can never be
  repurposed as another.
- An attacker who knows all three identifiers still has nothing.

### 5c-bis. `secret_phrase` — dropped from v3, deliberately

**Operator decision 2026-08-09: the pepper is not in v3.** Recorded here
with its reasoning so it is not re-litigated later.

The idea was a per-installation secret mixed into the key. It was dropped
for one unsolved reason: **distribution.** Both the lock and the app must
hold it to derive the same key. Set at doorlock config time it reaches the
lock fine — but the path to the *app* is the problem. In the QR code it is
only as protected as the QR. Typed by a human it is not high-entropy in
practice. Fetched from the server it is no longer a secret the server
lacks, which defeats the mailman property it was meant to reinforce.

A proposal to derive it from `server_id` was rejected outright: `server_id`
is public (§5b), and a value derived from a public value is public. That
would have produced the *appearance* of a second factor with none of the
substance — the most dangerous kind of security addition.

The pairing secret already carries the strength. If a pepper is wanted
later it needs its own provisioning design first, as deliberate work, not
a line in a key-derivation formula.

### 5d. Honest constraint: this is a coordinated breaking change

The v2 envelope format is **frozen and byte-verified** against ftpos's
`packages/ozkey_commissioner/lib/src/envelope.dart`, which `ozcrypto.h`
itself names as authoritative. Changing the salt changes every derived key.

Therefore: introduce this as **`OZ_ENV_VER = 0x03`**, keep v2 open-only
support during rollout, and cut over app + firmware + server together.
Do not silently alter v2's derivation — a version byte exists precisely
for this.

### 5e. The v2→v3 migration rules (decided — these are safety-critical)

No re-pairing in the field. The v3 key is a deterministic function of the
same `pairing_secret` both sides already hold, so a bond can move to v3
without a new ceremony. But "both sides just switch" is **not atomic**, and
getting this wrong locks legitimate holders out of a lock they own. Three
rules, all mandatory:

**1. Switch on successful RECEIVE, not on send.** The commit point is
"I have opened a valid v3 envelope from this peer" — never "I have sent
one." Sending proves nothing about the far side. Concretely: app sends
`{"kind":"v3_handshake"}` under v2; the lock replies under v3 but **stays
willing to open v2**; only when the lock successfully opens a subsequent
v3 envelope from that bond does it record the bond as v3-established.

Why this rule exists: if the lock switched on send, a lost reply — BLE
disconnect mid-ceremony, Thread packet loss, both routine on this bench —
leaves the lock expecting v3 while the app still speaks v2, with the lock
holding the door.

**2. Version state is per-bond, not per-lock.** Phones update
independently through the app store. A global flip when the first phone
upgrades would lock out every member still on the previous build. With 16
bond slots this is a certainty, not a risk.

**3. Sunset v2 per-bond, once v3 is established.** After a bond has
successfully used v3, that bond refuses v2 permanently — a flag in the
bond table alongside `counter_floor`. Without this, v2's weaker binding
(no `server_id`) stays available forever and the migration never actually
completes. Forging v2 requires the pairing secret, so this is not urgent,
but an unbounded dual-accept window means the weakness is permanent by
default.

## 6. The two-way channel

With §3b's finding, this is plumbing. Same sealed-envelope mechanism, now
travelling lock→app, with a `msg_id` echoed back so a reply can be matched
to its request the way downlink commands already are.

**Message shape.** One reserved marker says "the real message is inside";
the JSON payload is self-describing:

```json
{"v":3,"msg_id":"…","kind":"query_roster","args":{…}}
{"v":3,"msg_id":"…","kind":"roster_response","bonds":[…]}
```

`kind` is a string, not a DP number. This is the point of the design and
worth stating plainly: **we do not add DP 200+ for new capability.** The
Tuya DP id is a single byte (see the frame `55 AA 00 06 00 05 01 01 …` —
one byte of DP id), so "DP200–400" cannot be expressed in the format at
all, and more importantly a single byte plus a short payload cannot carry
a roster, an audit log, or a query with parameters. DP numbers stay
reserved for the small set of genuinely hardware-facing commands that must
cross the Tuya-formatted UART link. Everything else is a `kind`.

**Transport.** Lock seals with `ozEnvKey(appToLock=false)` → Thread UDP →
bridge32 → MQTT under **the lock's own topic** (`ozkey/<site>/locks/<id>/…`,
not the bridge's) → server relays opaquely → app opens it. Publishing
under the lock's own device_id is what lets the server stay a mailman; it
already subscribes `ozkey/lab/locks/+/…` wildcards and cannot tell the
difference.

**bridge32 needs a receive path it has never had.** Its `threadUdp` is
send-only today (`sendToThreadGroup()`, comment: "sender: plain unicast
bind") — no `parsePacket()`, no read loop. Port the lwIP raw-socket
technique from `ozdoorlock_core.h`'s `pollThreadUdp()`, which exists
precisely because `OThreadUDP`'s receive half is broken on this
Arduino-ESP32 core (`thread-relay-debug-2026-07-28`). This is a port, not
an invention.

**First queries to support**, chosen because they each retire a live bug
class from §1: `query_roster` (retires XF-75/78's stale-roster tapping),
`query_bond_state` (retires XF-77's "did the revoke land?"), and a pushed
`roster_changed` event so an admin phone learns immediately instead of
discovering it 20 minutes later through a stale cache.

**The lock needs a send counter it does not currently have.** Verified
against the tree: the bond table persists `counter_floor` (inbound
anti-replay) and nothing else. `ozEnvOpen()`'s `outCounter` is the
*received* counter returned to the caller, not a transmit counter. Sealing
lock→app traffic requires a **per-bond outbound counter persisted to NVS**
— if it resets on reboot, the app either rejects valid replies or accepts
stale ones, and a lock that reboots on a brownout (see
`doorlock-brownout-suspicion`) would do exactly that. Tracked as U0, and
it blocks U1.

### 6c. THE OZKIE CONTRACT (normative)

**This section is authoritative. Server and app align to the lock, not the
reverse.** The reference implementation is `ozSemanticDispatch()` in
`common/ozdoorlock_core.h` (doorlock-1.26).

**HARDWARE-VERIFIED 2026-08-10 01:17**, not merely compiled. Full chain
proven on DoorA (`ozk-acebe639f8c4`) + bridge `ozb-98a316a7e638`:
enrolment → `roster_changed` sealed and sent → bridge relayed → MQTT, in
**348 ms**. The envelope was byte-verified as real AES-256-GCM in the
frozen format (99 B; `ver=0x02`, counter 66 in both header and nonce tail,
62 B ciphertext, 16 B GCM tag) — not a synthetic fixture.

Where this doc and the firmware ever disagree, the firmware is right and
this doc is stale; say so and it gets fixed.

#### Downlink — app → lock

Plaintext inside the sealed envelope is UTF-8 JSON. Field `kind` selects
the verb.

| `kind` | Fields | Who may send | Lock action |
|---|---|---|---|
| `unlock` | — | any bond | DP 1 BOOL 0x01 → MCU |
| `grant_pin` | `slot`(int), `cred`(hex), `from`(u32), `to`(u32) | bond #0 | DP 21 RAW → MCU |
| `delete_pin` | `slot`(int) | bond #0 | DP 22 RAW → MCU |
| `grant_rfid` | `slot`, `cred`, `from`, `to` | bond #0 | DP 23 RAW → MCU |
| `delete_rfid` | `slot`(int) | bond #0 | DP 24 RAW → MCU |
| `bond_revoke` | `pub`(hex, 64 chars) | bond #0¹ | in-lock, never forwarded |
| `invite_cancel` | `nonce`(hex, 32 chars) | bond #0 | in-lock, never forwarded |
| `list_bonds` | — | bond #0 | in-lock, chunked reply |

¹ `bond_revoke`'s rule is subtler than admin-only and is owned by
`handleBondRevoke()`: bond #0 is never revocable by anyone, and a member
may revoke *itself*. Do not re-implement this check app-side; send the
verb and read the status.

**An unrecognised `kind` is rejected and never forwarded.** This is an
allow-list, deliberately — blind forwarding of an authenticated-but-
unrecognised verb is not a property worth keeping.

Grant value layout, for reference (the lock builds this; no one else needs
to): `slot`(2 BE) ‖ `cred` ‖ `from`(4 BE) ‖ `to`(4 BE).

#### Uplink — lock → app (new in doorlock-1.25)

Sealed with the **`ozkey/lock->app`** key direction, counter from the
lock's own per-bond send counter (U0). Same envelope format, opposite key.

| `kind` | Fields | Sent when |
|---|---|---|
| `roster_changed` | `reason`("member_enrolled"\|"bond_revoked"), `bonds`(int) | pushed to every admin bond, unprompted, the moment the roster mutates |

The wire wrapper carrying it (read by the bridge for routing only):

```json
{"from":"ozk-…","to":"<app_id hex>","envelope_hex":"…"}
```

MQTT topic is the **lock's own**: `ozkey/<site>/locks/<from>/uplink` —
never the bridge's, which is what makes a bridged lock indistinguishable
from a WiFi one.

> **CORRECTION (V1 result, 2026-08-10).** This doc twice claimed the server
> would need **zero changes** because it "already subscribes
> `ozkey/<site>/locks/+/…` wildcards." **That was wrong.** `SUB_ENROLL` and
> `SUB_HEARTBEAT` are `locks/+/enroll` and `locks/+/heartbeat`
> *specifically* — not a blanket `locks/+/#`. MQTT matches the full topic
> string, so a subscription to `.../heartbeat` does not also deliver
> `.../uplink`. No wildcard covered this and the server received nothing.
>
> The server team verified this live before writing any code (published a
> synthetic message to the uplink topic, observed zero server activity),
> which is exactly why V1 said *verify, do not assume*. The instruction was
> right; the assumption it was guarding against was mine.
>
> **Fixed server-side:** `SUB_UPLINK` (`ozkey/lab/locks/+/uplink`) added,
> with a handler that reads **only** `msg_id` (which travels outside the
> seal by design, for request/reply correlation) and the raw byte size, and
> records both via `recordAudit()`. `kind`, `args`, `bonds` and everything
> else in the payload are never read — §6a's metadata/content split, held.

**Not yet implemented, coming with Q1:** `query_roster`,
`query_bond_state`, and their `*_response` kinds with `msg_id` echo.
`roster_changed` shipped first because it is the one that retires
XF-75/77/78 without the app needing to ask anything.

#### Transitional rules

- **Dual-accept is live.** The lock accepts legacy Tuya-frame plaintext
  *and* OZKIE JSON, discriminated on the first byte (`{` vs `0x55` —
  unambiguous, no version field needed). Nothing app-side breaks today.
- The legacy path is deleted **only** once a sealed semantic sender is
  confirmed shipped — same discipline the server applied to
  `buildUnlockFrame()`. Cutting it early just breaks unlock for everyone.
- Uplink travels Thread UDP port **5053** (downlink stays 5052).

### 6d. Thread multicast — MEASURED behaviour (authoritative reference)

**This section supersedes every earlier assumption in this repo about
`ff03::4f5a`, including in ozkey-10/11 and the bridge32 source comments.**

`ff03::4f5a` — the custom "OZ" group this project defined — **has never
delivered a single packet on this mesh.** Not unreliable: non-functional,
measured across every downlink datagram the bench has ever logged.

```
Received downlink packets, by arrival group:   18 × "via":"ff03::1"
                                                0 × ff03::4f5a
bridge32 transmitted, per command:              1 × ff03::1
                                                1 × ff03::4f5a
                                                1 × unicast ML-EID
```

**Why.** `IPV6_JOIN_GROUP` fails with `errno=125` on *every* board, lock
and bridge alike. OpenThread's `otIp6SubscribeMulticastAddress()` succeeds
and the node cheerfully reports `mcast joined: ff03::4f5a` — but lwIP never
joined it, so lwIP never delivers those datagrams to the socket. Subscribed
at the Thread layer, dead at the socket layer. The two layers disagree and
only one of them is holding the packet.

`ff03::1` (realm-local all-nodes) works because every Thread node joins it
automatically, with no explicit subscription and no lwIP join needed.

**Why nobody noticed for months.** `bridge32`'s `forwardOverThread()` sends
every downlink to all three destinations. The shotgun worked, and it hid
that two of the three barrels are blanks. The first code to depend on
`ff03::4f5a` alone was U1's uplink (doorlock-1.25), and it failed
immediately and silently — the lock sealed and transmitted perfectly to an
address nothing receives on.

**Rules, for anything that sends over Thread from now on:**

1. **Never rely on `ff03::4f5a` alone.** Always include `ff03::1`.
2. Sending to multiple destinations is the established, proven pattern —
   duplicates are harmless here (the receiver filters on `target`/`from`,
   and the counter dedups at the crypto layer), whereas a miss is silent
   total failure.
3. `mcast joined:` in a boot log is **not** evidence a group works. Only a
   received packet is. Check `"via"` on the receive side.
4. Unicast to the peer's ML-EID is the third destination bridge32 uses and
   the most reliable of the three — worth preferring where the peer address
   is known.

Fixed in doorlock-1.26: `ozThreadUdpSend()` now transmits to both
`ff03::1` and `ff03::4f5a`. Hardware-verified 2026-08-10 01:17 — full
chain lock → bridge → MQTT in 348 ms.

### 6a. What "mailman" means precisely — metadata vs. content

A blanket "the server must never parse uplink" would break working
behaviour. `server.js:682` parses heartbeats today and legitimately must:
it updates `last_seen_at`, `fw`, `transport`, and **flushes the pending
command queue on the lock's wake**. That is routing work, not
eavesdropping.

The rule is therefore a distinction, not a prohibition:

| Class | Examples | Sealed? | Server reads it? |
|---|---|---|---|
| **Operational metadata** | heartbeat, presence, wake signal, `fw`, `transport` | No | **Yes** — this is how it routes and flushes queues |
| **Content** | query responses, roster, bond state, event payloads, log bodies | **Yes** | **Never** — opaque bytes it forwards |

The mailman property is that the server cannot read *what was said*, not
that it cannot see *that a letter arrived*. A postal service reads the
envelope; it does not open it. Uplink content goes under a separate topic
suffix from heartbeat so the two are never confused at the routing layer.

### 6b. Rate limiting

Required, but sized against the real threat. A "rogue app" must already
hold a valid bond to send a sealed query at all, so that is the narrow
case. The observed case is our own software: XF-72, XF-74 and XF-76 were
all BANOI retry-looping against a lock this very session, and on a 4×AA
battery lock the cost of a query storm is **battery, not CPU**.

So: a limit generous enough never to interfere with legitimate admin use,
firm enough to stop a runaway loop — plus a back-off on repeated errors
from the same bond, since a loop that is failing is exactly the one that
will not stop on its own. Exact figures to be set during Q1
implementation against real battery measurements, not guessed here.

## 7. Work breakdown

Status as of 2026-08-09. "built" = implemented and compile-verified;
**nothing below is bench-verified end-to-end yet.**

| # | Where | Task | Status |
|---|---|---|---|
| **F8** | `ozdoorlock_core.h` | `ozSemanticDispatch()` — OZKIE JSON in, Tuya frame built locally at the MCU boundary (§4a, §6c) | **built** (1.25) |
| **U0** | `ozcrypto.h` | Per-bond outbound counter, 48-bit in `OZ_BOND_REC`'s spare bytes, block-reserved | **built** (1.25) |
| **U1** | `ozdoorlock_core.h` | `ozUplinkSend()` + `ozNotifyRosterChanged()`, wired to enrol and revoke | **built** (1.25) |
| **U2** | `bridge32.ino` | lwIP raw-socket receive on uplink port 5053 | **built** (1.9) |
| **U3** | `bridge32.ino` | Republish under the lock's own topic | **built** (1.9) |

Sizing note for F8/U0/U1: `OZ_UDP_RX_BUF` 512 → 1024, because OZKIE JSON is
2–3× the Tuya frame it replaces once hex-encoded (a `grant_pin` with a hex
credential lands near 430 B against a 512 B buffer). U0 deliberately does
**not** grow `OZ_BOND_REC`: `ozBondsLoad()` validates the blob against
`OZ_BONDTAB_SZ`, so a longer record would make every existing table fail
that check and silently drop every bond in the field on first boot.

Remaining:

| # | Where | Task |
|---|---|---|
| S10 | `ozlockserv/server.js` | Unlock → sealed envelope; delete `buildUnlockFrame()` (§4) |
| K1 | `ozcrypto.h` + ftpos `envelope.dart` | `OZ_ENV_VER 0x03`: add `server_id` to salt, `purpose` to info. No pepper (§5c-bis) |
| K2 | both | v2 open-only compat; switch-on-receive, per-bond, sunset-after-v3 (§5e) |
| U0 | `ozdoorlock_core.h` | **Per-bond outbound counter, persisted to NVS — blocks U1** (§6) |
| U1 | `ozdoorlock_core.h` | Uplink send path — seal via `ozEnvKey(appToLock=false)`, emit over Thread UDP |
| U2 | `bridge32.ino` | lwIP-socket receive path on the uplink port (port `pollThreadUdp()`) |
| U3 | `bridge32.ino` | Relay uplink to MQTT under the **lock's own** topic (§6) |
| Q1 | `ozdoorlock_core.h` | Query handler: `query_roster`, `query_bond_state`, `msg_id` echo, rate limit (§6b) |
| Q2 | ftpos app | Query client + `roster_changed` consumer — retires the XF-75/77/78 bug class |
| V1 | live bench | Verify server needs zero changes to relay uplink (do not assume — test) |

## 8. Decisions taken (operator, 2026-08-09)

The four questions this section originally posed are answered. Recorded as
decisions, not options.

| # | Decision | Where specified |
|---|---|---|
| 1 | **No re-pairing in the field.** v3 keys re-derive from the existing pairing secret — but switch on *receive*, per-bond, and sunset v2 per-bond afterwards | §5e |
| 2 | **Queries get full sealing**, same as commands. One trust model, one mechanism — and §3b means it is nearly free | §6 |
| 3 | **`secret_phrase` dropped from v3.** Distribution to the app was never solved; deriving it from `server_id` would have made it public | §5c-bis |
| 4 | **txlog: new events only, plus a bounded catch-up window.** No full 10,000-event drain on reconnect | U6 / ozkey-16 §4.4 |

Additionally adopted during review, all safety-relevant:

- The v2→v3 handshake is **not atomic** and must not be treated as such (§5e).
- "Mailman" is a metadata/content distinction, not a blanket no-parse rule —
  the server must keep parsing heartbeats to flush queues (§6a).
- Rate limiting is justified by **our own retry-loop bugs and battery cost**,
  not by hypothetical rogue apps (§6b).
- Lock→app sealing needs an **NVS-persisted outbound counter** that does not
  exist today (U0, blocks U1).

## 9. What this does not change

The Tuya `55 AA` frame stays exactly as-is at the UART hop to the strike
MCU, including DP1/DP21–24. That sub-board speaks Tuya and nothing else,
and there is no reason to touch it. The change is that the frame is
*born inside the lock*, microseconds before it is written to the pin,
instead of being composed a continent away and shipped through three
networks that have no business knowing what it says.
