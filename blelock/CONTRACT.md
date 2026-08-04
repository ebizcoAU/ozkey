# blelock BLE GATT contract — OZ commissioning (v0, ozkey-08 §10)

The profile MAOI/BANOI's `OzkeyBleTransport` and this firmware both build
against (XFtposDecisions-43 §7.5 / ozkey-08 §10; payload/status logic = the
shared `ozkey_commissioner` Dart package, firmware implements the mirror
image).

> **v0 amendments (operator directive 2026-07-16, ozkey-08 §10.3):**
> advertised name is plain **`OZLOCK`** (supersedes `OZKEY-<last4>` below);
> v0 targets **mode 3** (`mode=ozkey-cloud`, ozlockserv :4200, site `lab`,
> terminal status **ENROLLED**); payload gains optional **`name`** (doorlock
> display name) and `info` reports it back; hotel `mode=ozkey-local` (below)
> becomes v1 — same firmware, different payload.

## Advertising

- Name: **`OZLOCK`** (v0; ~~`OZKEY-<last4 of device_id>`~~ superseded)
- Advertises the service UUID below; connectable while UNCOMMISSIONED or in an
  operator-opened pairing window. Stops advertising once provisioned (re-opened
  by factory-reset / re-provision gesture).

## Service

`4f5a4b31-0001-4c4f-434b-000000000001`  (ASCII motif "OZK1…LOCK")

| Characteristic | UUID (…0002/3/4) | Props | Payload |
|---|---|---|---|
| `provision` | `…0002` | write | `ProvisionPayload` JSON — v1 plaintext (bench); v2 = `OzkeyEnvelope`-sealed bytes, same characteristic |
| `status`    | `…0003` | **read + notify** | `OzkeyStatus` wire strings: `BLE_OK`, `WIFI_JOINING`, `WIFI_OK`, `BROKER_JOINING`, `BROKER_OK`, `WIFI_FAIL`, `BROKER_FAIL` |

> **READ on `…0003` is NORMATIVE, not incidental — XF-53 (Z), 2026-08-03.** It was
> listed as `notify` only, while all four firmwares have always declared
> `PROPERTY_READ | PROPERTY_NOTIFY` (`doorlock.ino:1963`, `bridge32.ino:1020`,
> `threadcomm.ino:347`, `blecomm.ino:840`). The app now **depends** on it: it polls
> the characteristic every 2 s while a write is outstanding, because
> `notifyStatus()` calls `setValue()` unconditionally — **so a status whose
> notification was never transmitted is still readable, and reading is the only way
> to see it.** That is what closes the XF-53 hang, where a terminal rung arriving
> 13 ms after its predecessor was lost and commissioning stalled forever.
>
> A build that drops READ degrades safely (the read throws, the app detects it and
> disables polling) but brings the hang back with it. No wire change — this makes an
> existing property a promise.
>
> **Do not rely on the value being reset to `BLE_OK` on connect.** Every firmware
> happens to do it; none of them promises to, so the app takes its own baseline read
> before each write and reports only a *change*. Otherwise a poll could read back an
> hour-old `THREAD_OK` and complete a commissioning that had not started.
| `info`      | `…0004` | read | JSON `{"device_id":"ozk-…","fw":"blelock-0.1","mac":"AA:BB:…"}` |

## Provision payload (mode=ozkey-local — the hotel case)

```json
{
  "v": 1,
  "mode": "ozkey-local",
  "ssid": "…", "password": "…",
  "broker_host": "10.1.1.20", "broker_tcp_port": 1883,
  "server_ip": "10.1.1.20", "server_port": 3200,
  "device_id": "ozk-<machex>",
  "site_id": "hotel",
  "heartbeat_s": 60
}
```

Validation rules = `ozkey_commissioner/lib/src/provision_payload.dart`
(authoritative). Unknown fields ignored (forward compat).

## Sequence (5-phase, XF-43 §7.5)

```
0. app connects, reads info, subscribes status        → lock notifies BLE_OK
1. app writes provision JSON
2. lock joins Wi-Fi (WIFI_JOINING → WIFI_OK)
   lock dials MQTT broker (BROKER_JOINING → BROKER_OK)   ← terminal success for
   BLE stays up through both (C6 coex) — closed loop      mode=ozkey-local
3. lock announces on hotel/locks/unpaired/heartbeat → MAOI pairs to a room
   (POST /locks/pair → provision_assign; identical to LockSim)
4. operational: heartbeat / DPID credential frames / log
```

Failure: notify `WIFI_FAIL`/`BROKER_FAIL`, stay connectable, accept a
re-written provision (re-provisionable, never one-shot — §7.5).

## Operational / member profile — CANONICAL (confirmed XF-47, 2026-07-30)

> The N-bond multi-user contract (ozkey-08 §0.4 / ftpos XFtposDecisions-46,
> **confirmed by ozkey-team in XFtposDecisions-47 §2 on 2026-07-30 — this section
> is now canonical, not a draft**). UUIDs, wire shapes and the invite-MAC
> realization are all confirmed; `ozInviteMac()` in `blecomm/ozcrypto.h`
> implements the MAC below byte-for-byte, and `ozLockPubHex()` is already wired
> into `info` as `"pub"` (`blecomm.ino:851`, `doorlock.ino:1269`).
>
> **Firmware acceptance test is ftpos's byte-exact vectors**, not our own:
> `ftpos/packages/ozkey_commissioner/test/member_invite_test.dart` and
> `tool/gen_invite_vector.dart`.
>
> Additions agreed in XF-47 round 2 are in the subsections below marked
> **[XF-47]**. Delivery milestones M1–M4 (2026-08-01 → 2026-08-08) are in XF-47 §6.

**Advertising (operational):** touch-window only — an enrolled lock
advertises `OZLOCK` + the service UUID for **~60 s after any keypad/screen
touch**, never while idle (power + no trackable beacon; ozkey-08 §0.4).
Production adds BLE RPA rotation; bench keeps the plain name.

**`info` gains `"pub"`:** the lock's X25519 ceremony public key (lowercase
hex, 64 chars). Needed by the member ceremony (and the future v2 sealed
commissioning) to derive the pairing secret. Lock keypair minted at first
boot, NVS-persisted, survives re-provision, wiped on factory reset.

**New characteristics** (same service `4f5a4b31-0001-…`):

| Characteristic | UUID (…0005/6/7) | Props | Payload |
|---|---|---|---|
| `challenge` | `…0005` | read | 16 random bytes, fresh per read; valid for this connection, ~30 s |
| `control` | `…0006` | write | `utf8(app_id_hex, 64 chars)` ‖ `OzkeyEnvelope` (app→lock, per-bond counter). Envelope **plaintext = challenge(16 B) ‖ DPID frame**. Lock: look up bond by app_id → open envelope (counter > bond floor) → verify challenge == last-issued → execute frame (DP 1 remote-unlock in v1; role-gate admin verbs to bond #0) |
| `member_enroll` | `…0007` | write | plaintext JSON `{"app_id":"<member X25519 pubkey hex>","invite":"OZINV1:…"}` — chunked like `provision` (buffer resets on `{`, parse on JSON-complete). No bond exists yet, so this is unsealed; the INVITE is the authenticator |

**Member-enroll lock-side algorithm:** decode invite (`OZINV1:` +
base64url JSON, fields v/d/i/r/l/n/e/m) → recompute MAC:
`mac_key = HKDF-SHA256(ikm = bond#0 pairing secret, salt = utf8(device_id ‖
issuer_app_id_hex), info = "ozkey/invite-v1")`;
`mac = HMAC-SHA256(mac_key, utf8("1|device_id|issuer|role|label|nonce|expires"))`
(byte-exact vectors: ftpos `packages/ozkey_commissioner/test/
member_invite_test.dart` + `tool/gen_invite_vector.dart`) → nonce unused
(replay cache, suggest 32-entry LRU in NVS; nonce = the HARD guarantee) →
expiry best-effort (clock drift tolerated) → capacity ≤16 bonds → add bond
`{pubkey, role, label}` → pairing secret = X25519(lock_priv, member_pub) →
notify. Lock reports `bond_added` / `bond_revoked` on its log topic at next
sync (OZLOCK builds the door→apps map passively).

**New `status` wire strings:** `MEMBER_OK`, `MEMBER_FAIL`, `MEMBER_FULL`,
`MEMBER_REPLAY`, `MEMBER_EXPIRED` (enroll) · `UNLOCK_OK`, `UNLOCK_DENIED`
(control). These are OUTSIDE the commissioning ladder — apps consume them on
a raw-status stream, `OzkeyStatus.parse` ignores them.

### M3 — BUILT 2026-08-03, `doorlock-1.5`. Realization notes

Everything below is implemented and compiles clean; hardware verification is the
open item. Six points where the build made a decision the spec above left open —
all are now canonical.

**1. The touch window is REAL, and it is what makes any of this reachable.**
The "~60 s after any keypad touch" rule above was specified in XF-46 and had
never been built: only a short BOOT press opened a window (v1.2, XF-52 R). BOOT
is on the board — *inside* the door. The keypad is *outside*. A member standing
at a commissioned lock therefore had no way to make it advertise, so
`member_enroll` was unreachable no matter how correct the ceremony was. v1.5
opens the same 60 s window on **any tap**, not a designated key: the keypad grid
is a hit-test with nothing drawn on it, so "tap the invisible # key" is not an
instruction a user at a door can follow. Every tap re-arms the 60 s. BOOT stays
as the hardware escape hatch for a unit with a dead touch controller. There is
still no remote verb, and per XF-52 §4 there must never be one.

**2. `role: "admin"` is REFUSED in v1** — `MEMBER_FAIL`. A second admin is
listed under "Deferred (v2)" together with bond #0 transfer, and accepting one
would create an authority with no revoke story behind it. BANOI never sets
`role` (`doorlock_service.dart` calls `buildMemberInvite` without it), so this
costs nothing today.

**3. Re-invite of a key that already holds a bond refreshes the label and KEEPS
`counter_floor`.** BANOI's `reinviteMember` mints a fresh nonce for an existing
member, so this path is normal, not exceptional. Resetting the floor to 0 here
would re-open every frame that member had already sent — the same replay hole
XF-47 closed for revoke→re-invite, arriving by a different road.

**4. The nonce cache evicts FIFO, not LRU, and fails CLOSED.** A nonce is read
once and never again in the normal case, so recency and insertion order are the
same ordering and LRU buys nothing. If the 3 KB working buffer cannot be
allocated, the check reports `MEMBER_REPLAY` rather than proceeding unchecked —
a lock low on heap must not become a lock with no replay protection.

**5. The busy flag is realized as SCAN_IND, and that IS the single-connection
enforcement.** While connected the lock keeps advertising but switches to PDU
type `ADV_TYPE_SCAN_IND` (scannable, **non-connectable**): a second phone can
still see the lock and read `busy = 1` from the scan response, while the link
layer refuses the connection outright, so a half-finished enrolment can never be
aborted by someone else connecting. **This requires BANOI to scan ACTIVELY** —
service data lives in the scan response because the ADV packet is full at 29 of
31 bytes and the 0x07 UUID list cannot be displaced without breaking discovery.

**6. `member_enroll` on an unowned lock is `MEMBER_FAIL`.** With no bond #0
there is no issuer to verify against, so no invite can be authentic. The lock
refuses rather than inventing a trust root.

**Storage.** M2's three flat NVS keys (`b0pub`/`b0role`/`b0ctr`) become slot 0
of a single 16-slot blob `bondtab`; a lock carrying the M2 keys migrates
silently on first v1.5 boot and the old keys are deleted, so a stale owner
cannot linger in an NVS dump. One blob rather than 48 keys because a bond is
written as a unit and NVS has no cross-key transaction — a partial write that
left a pubkey without its role would be an unauthenticated bond. Nonce cache is
a second blob, `noncecache`. Both live in the `blelock` namespace that
`factoryReset()` clears, so a reset still wipes owner, members and burned nonces
together with the keypair.

**`control` …0006 is deliberately NOT created yet.** It is M4 — **now built, see
below.** An advertised but inert characteristic would let the app's capability
probe conclude the lock can authorise an unlock.

**Cost:** +10,956 B flash (62% of 3.34 MB), +1,784 B RAM — the 16-slot bond
table plus a 512 B invite-decode buffer. The 3 KB nonce cache is heap-transient,
allocated per enrolment and freed.

### M4 — BUILT 2026-08-04, `doorlock-1.8`. Realization notes

`control` …0006, the DP dispatch split, DPID 101/102, role gates and the
unconditional challenge. Compiles clean; **hardware verification is the open
item** (the operator does all flashing). Seven points where the build decided
something the spec above left open — all now canonical.

**1. `control` reassembly uses the GCM tag as its completeness oracle.** The
other two writable characteristics carry JSON and share the rule "a chunk
starting `{` resets the buffer, parse when the JSON completes". `control` is
binary, and the frozen wire shape — `utf8(app_id_hex,64) ‖ envelope` — carries
**no length field**, so nothing in the bytes says how many chunks are still
coming. Rather than change the wire, the lock attempts to open on every chunk
once the buffer could hold a shortest-possible message: a truncated envelope
fails the tag, the complete one passes. At the MTU 247 the app requests this is
a single pass. **No wire change; ftpos need do nothing.**

**2. …and an idle timer, because the tag cannot tell "incomplete" from
"forged".** Both are simply a failure to open. Left there, a corrupted write
gets silence — and silence is precisely the XF-53 hang: the app writes, waits
for a status that never comes, and stalls forever. 400 ms with no further chunk
closes the message out with a definite `UNLOCK_DENIED`.

**3. The whole control path runs on the Arduino task, not the BLE task.** The
BLE callback only appends, under a critical section (a torn `ctlLen` is a torn
message). Two tasks opening envelopes into one buffer would race; this also
keeps a 512 B plaintext buffer, an X25519 agreement and the 300 ms self-revoke
flush off the BLE stack — the same constraint that forced M3's invite-decode
buffer to be static.

**4. The challenge is CONSUMED on a successful match, not just compared.** The
spec says "fresh 16 bytes per read"; the build additionally invalidates it once
a frame has used it. Without that, one `…0005` read would authorise every frame
for the rest of the connection. The app already reads the challenge before each
control write, so this costs a round trip that was being made anyway.

**5. `counter_floor` advances BEFORE the frame executes.** If execution reboots
the lock or the MCU write hangs, the frame must not be replayable on the way
back up. An unlock that maybe happened is a far better outcome than one anybody
who captured it can repeat.

**6. Role-gate ordering leaks nothing.** For 101, "target is bond #0" is checked
first (same answer for owner and member alike), then "a member may only revoke
itself", and only then "no such bond". Reversed, a member could walk the table
and learn which pubkeys hold a bond here from the difference between
`REVOKE_DENIED` and `REVOKE_NOT_FOUND`.

**7. `invite_cancel` reuses the replay cache rather than adding a second list.**
Cancelling burns the nonce against an **all-zero pubkey**, which is not a usable
X25519 key, so a later redeem arrives with the member's real key, sees a
different pubkey, and is refused as `MEMBER_REPLAY`. An already-redeemed nonce
answers `REVOKE_DENIED` (use 101 on the resulting bond); an already-cancelled
one answers `REVOKE_OK`, idempotently. **Note the one asymmetry:** `ozNonceCheck`
fails *closed*, which for a cancel is the unsafe direction — a heap failure
leaves the invite alive. It reports `REVOKE_DENIED` so that is visible and
retryable rather than silently ineffective.

**BLE `control` accepts DP 1, 101 and 102 in v1 — NOT 21–24. CANONICAL
(XF-59 (AV), confirmed by ftpos 2026-08-04.)** The row above says "DP 1
remote-unlock in v1", and credential frames keep arriving on the server path
where issuance actually lives.

**The reason is normative, not just the rule: a PIN issued over BLE has no
server record, so the lock and the server disagree about what is live on that
door — two authorities for one operation, and the quieter one wins by
accident.** An unlock has nothing to reconcile; it is a momentary actuation. That
is XF-58's "one fact, one authority" arriving from the credential side.

ftpos confirmed from their code rather than from intent: the only `DpidFrames`
21–24 calls in BANOI are on the demo branch, and `Keyring.sealAddTempPin` and
friends have exactly one caller in the repo — a test. No production path seals
21–24 over any transport. A DP 21–24 frame on …0006 is answered
`UNLOCK_DENIED`. If at-the-door PIN management is ever needed it is a v2 verb
with its own status strings and a sync-back to the server, not a widened
`control`.

> **Comment hygiene, from the same round.** ftpos found `keyring.dart:212`
> describing the sealed credential envelope as "the opaque envelope OZLOCK relays
> (or the BLE `control` write, same bytes)" — true of the bytes, and it would have
> read as permission. **The wire being capable of something is not the contract
> permitting it.** Same class as the two comments XF-53 (Z) falsified. Comments on
> the DP-forward path must state policy, not capability.

**Test vectors for 101/102 are byte-exact, and a checksum is not a substitute
(XF-59 §7.1).** The frames below are the literals in the `dp-frame-101/102`
self-test leg. A Tuya checksum is `sum & 0xFF`, so it is invariant under any
**permutation** of the value bytes — a byte-reversed `subject_pub` produces the
identical trailer `9E`. Never validate a value layout with a checksum; it cannot
see the defect that matters.

```
DP 101 subject_pub = A0 A1 … BF (32 B)
55 AA 00 06 00 24 65 00 00 20 A0 A1 A2 A3 A4 A5 A6 A7 A8 A9 AA AB AC AD AE AF
B0 B1 B2 B3 B4 B5 B6 B7 B8 B9 BA BB BC BD BE BF 9E                    (43 B)

DP 102 nonce = 10 11 … 1F (16 B)
55 AA 00 06 00 14 66 00 00 10 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 07   (27 B)
```

**DPIDs 101 and 102 are OURS**, invented for `bond_revoke` / `invite_cancel`.
They sit in the custom DP range, but they are an extension in a third-party
numbering space — recorded here so nobody later assumes the MCU vendor defined
them.

**The forward path is now an ALLOW-LIST, and this is the load-bearing change.**
`forwardHexToMcu()` previously sent anything that parsed as hex. It now requires
a 0x06 DP write whose DPID is in {1, 21–24}. 101/102 are dropped there with a
named log line — **not executed**, because that path is the anon-open MQTT
broker: a guest on the site Wi-Fi can publish to the command topic, and a
revoke arriving unauthenticated must not be honoured. Bond verbs exist **only**
on BLE `control`, where a bond, a challenge and a counter floor all have to line
up first.

**Boot self-test, two new legs.** `dp-allow-list` and `dp-frame-101/102`. The M4
property that matters is a negative one, and a negative is what a bench capture
is worst at proving — "I did not see the frame" and "I was not looking properly"
produce identical hex. LockSim still shows the wire; these check the predicate
that decides it, on every unit, every boot. Frames are **generated, not
transcribed** (the 1.6 leg-9 lesson) and were verified byte-for-byte against
the two frames specified below.

**Cost:** +5,640 B flash (62% of 3.34 MB), +520 B RAM — the 512 B reassembly
buffer and its bookkeeping.

### [XF-47] Bond #0 establishment, and the ownership-theft rule

Bond #0 is created from the **`app_id`** field already present in
`ProvisionPayload` (`ozkey_commissioner/.../provision_payload.dart:63`, emitted at
`:91`) — BANOI populates it with `keyring.appIdHex` on every real commissioning
path today. No new characteristic, no new field, no extra round trip.

```
provision accepted, app_id present:
  no bond #0 exists              → bond#0 = {pubkey=app_id, role=admin, counter_floor=0}
                                   pairing secret = X25519(lock_priv, app_pub)
                                   → emit BOND_OK
  bond #0 exists, app_id SAME    → idempotent: update WiFi/broker only → BOND_OK
  bond #0 exists, app_id DIFFERS → REFUSE: emit BOND_DENIED, change NOTHING
  no app_id in payload           → legacy path, no bond, no error
```

**`BOND_DENIED` exists because re-provision was an ownership-theft vector.**
`info.pub` survives re-provision, so without this rule anyone inside the ~60 s
touch window could re-provision a commissioned lock with their own `app_id` and
become bond #0. The refusal is **atomic** — a rejected re-provision must not
change the Wi-Fi or broker fields either, or an attacker who cannot steal the lock
can still repoint its broker. Only factory reset clears bond #0.

`BOND_OK` and `BOND_DENIED` are **inside** the commissioning ladder (`BOND_OK`
after provision-accepted, before `WIFI_JOINING`); `BOND_DENIED` is parallel to
`PAYLOAD_REJECTED`. `BOND_OK` is **optional** — its absence means pre-bond
firmware and the app falls back to `v1-bench`. `app_id` stays optional
firmware-side during cutover.

Factory reset wipes the ceremony keypair; `ozLockKeyInit()` mints a fresh one next
boot. Acceptance test: read `info.pub` → factory reset → read `info.pub` → assert
**different**. (Specified, not yet verified — cf. ozkey-10 root cause 3, where
`bridge32.factoryReset()` missed OpenThread's separate NVS namespace.)

### [XF-47] The challenge check is UNCONDITIONAL

The `challenge` comparison applies to **every** `control` verb — unlock, 101, 102,
and anything added later. Not unlock-only. Do not "optimise" it away for
non-unlock verbs.

**Why, because the reason is not obvious:** when a revoked pubkey is later
re-invited, its bond is re-created with `counter_floor = 0`, while frames captured
from its previous life carry counters 1..N — all of which clear that floor. The
stale challenge is the **only** thing preventing those captured frames replaying.

The challenge is 16 fresh bytes per read, **destroyed on disconnect**, and never
outlives its connection.

### [XF-47] DP dispatch split — 101/102 must NEVER reach the Tuya MCU

| DP | Handling |
|---|---|
| 1, 21–24 | `forwardHexToMcu()` — existing path, unchanged |
| **101, 102** | **handled in-lock, NEVER forwarded** |
| unknown | **reject, do not forward blindly** |

The MCU has no concept of a bond or a nonce; forwarding 101/102 would fault or be
silently ignored. The unknown-DP rejection is a hardening added in XF-47 — blind
forwarding of authenticated-but-unrecognised verbs is not a property worth keeping.

**DPID 101 (0x65) `bond_revoke`** — value = target member's 32-byte pubkey, raw:

```
dp    = 65 00 00 20 <subject_pub:32>                          (36 = 0x24)
frame = 55 AA 00 06 00 24 65 00 00 20 <subject_pub:32> <ck>   (43 bytes)
```

**DPID 102 (0x66) `invite_cancel`** — value = 16-byte invite nonce; burns an
**unredeemed** nonce so a leaked QR dies on demand:

```
dp    = 66 00 00 10 <nonce:16>                                (20 = 0x14)
frame = 55 AA 00 06 00 14 66 00 00 10 <nonce:16> <ck>         (27 bytes)
```

Role gates: **101** — bond #0 revokes any bond; a non-admin bond may revoke
exactly one target, **itself** (`subject_pub == sender app_id`, implements "Rời
khỏi cửa này"); **revoking bond #0 is always `REVOKE_DENIED`**. **102** — bond #0
only, unredeemed nonces only.

> **⚠ CORRECTED 2026-08-04 — this paragraph used to say "emit `REVOKE_OK` and let
> it flush *before* the bond becomes unusable; hold the connection until it is
> out, or a member can never confirm their own removal." That premise is FALSE
> and the rule it produced was fail-open.**
>
> `notifyStatus()` writes a **plaintext** string to `…0003` and gates only on the
> link count. It is not sealed under the bond and knows nothing about one;
> revoking does not drop the BLE link and cannot stop the answer arriving. A
> member confirms their own removal perfectly well from the far side of it.
>
> What the old ordering actually bought was a window in which the app had been
> told `REVOKE_OK` — and would drop the row — while the bond still sat in NVS. A
> reset inside that window left a member believing they had left a door that
> still trusted their key. **The rule is now: revoke and commit to NVS FIRST,
> answer second**, on self-revoke as much as any other. Caught when ftpos
> described the correct model independently ("the lock forgets first, then the
> app drops the row").
>
> General form, worth keeping: **a confirmation must never outrun the state it
> confirms.** Same shape as the `delivered ≠ the lock acted` problem on the
> command path — a claim emitted before its evidence exists.

New strings, **outside** the ladder: `REVOKE_OK`, `REVOKE_DENIED`,
`REVOKE_NOT_FOUND`. Log event `bond_revoked`.

### [XF-47] Nonce replay cache

Entry = `{nonce[16], member_pubkey[32]}`, **64 entries**, NVS (~3 KB).

- **Only successful enrollments write to the cache.** A failed MAC check never
  touches it — otherwise an attacker floods 64 junk nonces and evicts the record
  of a legitimate one, re-enabling replay of a captured invite.
- Burned nonce + **matching** `app_id` → `MEMBER_OK`, **no second bond**, and
  **`counter_floor` untouched**. This makes a dropped-notify retry idempotent;
  without it a lost BLE notify strands a member permanently and leaks a bond slot.
- Burned nonce + different `app_id` → `MEMBER_REPLAY`.

### [XF-47] Single connection, and the busy flag

**Max 1 concurrent connection. The in-progress connection is kept; a second is
refused** — dropping the first could abort a half-finished enrollment.

Because a link-layer refusal is indistinguishable from out-of-range, the lock
**advertises a busy flag**: service data, one byte, bit 0 = `busy` (1 = connection
active), set for the duration of a connection, cleared on disconnect. The lock
keeps advertising while connected so the flag is observable without connecting.
Survives future RPA rotation, since service data is address-independent.

| Scan result | Meaning |
|---|---|
| advertising, `busy = 0` | available |
| advertising, `busy = 1` | in use by another phone |
| not advertising | out of range, or outside the touch window |

### [XF-47] `expires` is parse-and-ignore in v1

Firmware parses `expires` and **does not act on it**. `MEMBER_EXPIRED` is reserved
in the wire protocol and **never emitted** until a clock exists. The nonce is the
hard guarantee.

**State plainly: expiry is UX hygiene in v1, not a security control.** BANOI
checks expiry twice (QR scan, and `completeEnrollment`) but both run on the
*cooperative* device — a malicious redeemer strips them. So a
photographed-but-unredeemed QR stays redeemable by someone with physical access
until cancelled. **DPID 102 is the real kill switch**, and it is cheaper than an
RTC.

Neither team may describe v1 expiry as a security control.

### [XF-47] Time synchronisation

Neither doorlock nor bridge32 has an RTC. **Expiry needs UTC only — timezone is
not required** for it (timezone matters only for future time-of-day schedules and
for display, which is the app's job). IP geolocation is **rejected**: it would be
the product's only phone-home, it is unreliable behind CGNAT/VPN/mobile, and the
app already holds the authoritative timezone.

Sources, in priority order: **NTP over WiFi (bridge only)**, daily and on
reconnect, pool host from DHCP option 42 if offered → **bridge → lock over
Thread**, since a Thread-mode lock has no WiFi → **app over BLE**, the only
timezone source.

Thread distribution: every forwarded command **carries a UTC timestamp** (free —
the datagram is already being sent), plus a slow **time beacon** to the lock group
(~6 h) for locks that receive no commands. The lock updates on any received
timestamp — no round trip, no extra wake cost.

**SECURITY RULE — the clock is monotonic-forward only.** Firmware refuses any time
value earlier than its current estimate, from every source including the app,
except immediately after factory reset. A clock that only moves forward cannot
resurrect an expired token. This holds even while time distribution is
unauthenticated, which it is today.

**HARDWARE — R6 (32.768 kHz crystal) RAISED AND DECLINED for the prototype run,
2026-07-30.** There is no RTC chip on the PCB and none is proposed. The C6 has an
RTC counter in its low-power domain regardless; only its clock source varies —
internal RC (~±5%, minutes-to-hours drift/day) versus an external 32.768 kHz
crystal (~±20 ppm, ~1.7 s/day).

Declined because:

1. On the ESP32-C6 the crystal pins are `XTAL_32K_P`/`XTAL_32K_N` = **GPIO0 and
   GPIO1 — the board's only two remaining spare GPIOs**
   (`docs/DoorLockHW/ESP32C6SEEEDSTUDIO.md:124`). Fitting it leaves zero
   expansion headroom.
2. Same reasoning as **R3**: the board already carries ~10 departures from the
   proven reference and a first run should not add more.
3. **Nothing in v1 needs an accurate lock clock.** `expires` is parse-and-ignore
   (ftpos, XF-47 3.4); time-of-day schedules are not v1.

**Replacement design for the audit trail — monotonic event sequence, not a wall
clock.** An audit trail needs reliable *ordering*, not accurate wall-clock time at
the lock:

- The lock stamps each log event with a **monotonic sequence number** persisted in
  NVS, never a self-generated timestamp.
- **The bridge or server applies authoritative time on receipt.**
- Events that occurred offline are bounded server-side between the lock's previous
  sync and the batch arrival time, with the sequence giving exact order within the
  batch.

This is more defensible in a dispute than a drifted timestamp, costs no hardware,
and keeps GPIO0/GPIO1 free. Revisit the crystal for rev 2 only if time-of-day
scheduling or lock-side `expires` enforcement is actually built.

### [XF-47] Bond taxonomy

| Pair | Required? | Purpose |
|---|---|---|
| **app ↔ doorlock** | **YES** — the XF-46 bond | Credential plane: unlock, DPID frames, admin verbs. Bond #0 admin, 1–15 members, ≤16 |
| **bridge ↔ doorlock** | **YES**, control plane only | Authenticated time distribution, state/telemetry uplink. **Not** needed to relay commands — the bridge forwards opaque payloads and cannot forge an app→lock envelope |
| bridge ↔ bridge | no, v1 | Thread's network key + routing suffice for a range extender. Revisit for multi-bridge failover |
| doorlock ↔ doorlock | **no, ever** | Locks exchange no application data; FTD relay is link-layer under the network key. Would add attack surface with no capability |

Thread's network key authenticates "a member of this mesh", **not** "the bridge" —
too weak for telemetry feeding a hospitality audit trail, hence the control-plane
bond. Proposed establishment: the app mints a random 32-byte control key per
(bridge, lock) pair during commissioning and writes it to both; HMAC-SHA256 via
the existing `ozHmacSha256()`. **Open — awaiting ftpos in XF-47 §6.2.5(b).**

### [XF-47] Service bond — OZKEY / OZPMS only

`role = service` allows the **operator's own** ozkeyserv/ozpms instance to hold a
bond and build sealed frames, so autonomous issuance (auto check-in,
cron-provisioned arrivals, web-only front desk) works with no MAOI device present.
Not a sovereignty breach: that server belongs to the hotel, not to us.

May issue and delete guest credentials. **May not** add or revoke member bonds, and
**may not** touch bond #0. A credential issuer, not an authority.

**OZLOCK residential keeps the app as sole frame builder** — no service bond, and
`grants.raw_value` is dropped from ozlockserv only. `ozkeyserv`/`ozpms` retain
`raw_value` and `buildCredentialFrame()` by design, which is also how front-desk
PIN re-display keeps working without an app-side cache.

## Deferred (v2)

- Factory-pubkey trust anchor (QR on screen) + X25519 session → sealed payload
- Matter-takeover semantics (this emulator boots straight into OZKEY mode)
- Admin-PIN keypad menu / battery-compartment factory reset (§7.5 device-side)
- Member profile: RPA advertising rotation · second admin / bond #0 transfer
  · member self-remove verb ("rời khỏi cửa này" currently local-only)
