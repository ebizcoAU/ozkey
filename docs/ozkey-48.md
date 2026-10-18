# ozkey-48 — OZKIE Protocol v2: secure session + message layer

> Status: 🟢 **APPROVED by operator 2026-10-18.** Remote unlock is owner only (§6). First pairing is
> confirmed with a keypad code (§3.4). App changes: XF-387. Firmware (team lead).
> Decided already (operator, 2026-10-18): AK1037 goes straight to v2; cipher suite is AES-128-CCM
> ("we go for speed"); open IETF parts + Matter vocabulary, no paid memberships (ozkey-49 §0).
> Consumers once approved: ftpos (BANOI app), server (ozlockserv / ozlodge / ozpms), bridgeBLE, ozkit.
> v1 references: `blelock/CONTRACT.md` (wire), `docs/ozkey-28.md` (verbs).
> Rev 2 (2026-10-18): server team review folded in: held-credential rollback (§4.3), one active
> sender per context, the Echo roles corrected, service bond as the audit source, lock-side UTC stamps,
> MQTT buffer size, and ozlock routing.

## 0. What changes from v1, in one table

| | v1 (ESP32 / CH584M era) | v2 (AK1037 and later devices) |
|---|---|---|
| Identities | X25519 keys: app `app_id`, lock `info.pub` | **Unchanged**: the same X25519 keys |
| Bond model | bond #0 admin, ≤15 members, invites, roles | **Unchanged** |
| Invite format | `OZINV1:` + HMAC (ftpos byte-exact vectors) | **Unchanged**, carried inside the handshake (§3.3) |
| Session setup | none: static pairing secret per bond | **EDHOC** (RFC 9528), method 3, fresh keys every session |
| Encryption | AES-256-GCM envelope, per-bond counter | **OSCORE** (RFC 8613), AES-128-CCM-16-64-128 |
| Replay / freshness | counter floor + 16-byte `challenge` read | OSCORE replay window + **Echo** (RFC 9175) |
| Payload | JSON, `kind` verbs | CoAP request, path = verb, **CBOR** body, integer keys |
| Vocabulary | our own DP numbers (some collide with Tuya) | Matter DoorLock cluster semantics (§5) |
| Bridge | forwards opaque envelopes | **Unchanged**: forwards opaque OSCORE messages |

Everything that v1 got right (who is allowed, how invites work, what the bridge can see)
is kept. What changes is the crypto layer and the encoding, both replaced by published standards
so no part of it is our own invention.

## 1. Why these standards

- **EDHOC + OSCORE** are the IETF's purpose-built pair for small battery devices. They need
  one cipher (AES-CCM), one hash (SHA-256) and one curve (X25519), which the AK1037 mostly
  has in hardware (ozkey-49 §3). The whole handshake is about 100 bytes, against several KB for TLS/DTLS.
- **Method 3 (static Diffie-Hellman on both sides)** authenticates both ends with X25519 keys
  only. There are **no signatures**, so no Ed25519 code is needed. It reuses the identity keys we
  already have, and it is the fastest EDHOC method. Cost per handshake: about 4 X25519
  operations per side (to be measured on the AK1037 in ozkey-49 P4).
- **OSCORE protects the message end-to-end**, not the link. The bridge, the MQTT broker and
  the server relay bytes they cannot read or forge. That is the v1 property, now provided by
  a standard.
- **CBOR** (RFC 8949) is a compact binary JSON. The data model is the same as v1's JSON, at
  roughly half the bytes, which matters on BLE.
- None of these need a membership, a licence fee or a certification program.

## 2. Transport: BLE GATT

New service, so a v1 phone app can never be confused by a v2 lock:

`4f5a4b32-0001-4c4f-434b-000000000001` ("OZK2…LOCK")

| Characteristic | UUID | Props | Content |
|---|---|---|---|
| `info` | `…0002` | read | CBOR map: `{1: device_id, 2: fw, 3: lock X25519 pub (32 B), 4: state (0 = unclaimed, 1 = claimed), 5: profile}`. Readable without a session. Leaks nothing a scanner couldn't learn by connecting |
| `ozkie` | `…0003` | write + notify | OZKIE frames (§2.1), both EDHOC and OSCORE |

Advertising, the touch window, the busy flag and the single-connection rule are **unchanged from
CONTRACT.md**.

### 2.1 Frame

```
byte 0:  bits 7..6 = type   (0 = EDHOC, 1 = OSCORE/CoAP, 2 = error)
         bit  5    = more   (1 = more fragments follow)
         bits 4..0 = frag # (0..31, resets per message)
byte 1…: payload fragment
```

Messages larger than one write (ATT MTU − 3) are fragmented.

Over MQTT (bridge relay), a frame travels as a **raw binary payload**, never hex or base64. One MQTT
message carries one whole reassembled OZKIE message. Apps should negotiate MTU 247 at
connect. Then every v2 message in this spec fits one frame except credential-by-value
enrollment (§3.3), which fits two. The maximum reassembled message is 1024 bytes.

## 3. Session setup: EDHOC

- **Cipher suite 0:** AEAD AES-CCM-16-64-128, hash SHA-256, MAC length 8, ECDH X25519.
  The application AEAD is the same.
- **Method 3** (STAT-STAT).
- **Roles:** the **app or bridge is the Initiator**, the **lock is the Responder**.
- **Messages:**
  - message_1: app → lock.
  - message_2: lock → app.
  - message_3: app → lock.
  - message_4 is not used. The first OSCORE response gives the app key confirmation (RFC 9528 §5.5).
- **Connection identifiers:** C_I and C_R are 1-byte values. They become the OSCORE Sender and
  Recipient IDs.
- **Credentials:** a CWT Claims Set (CCS, RFC 8392) holding an X25519 COSE_Key.
  - **The lock's credential:** referenced by `kid` (ID_CRED_R). The app already holds the lock's
    public key from `info`, or from the QR code once that exists (CONTRACT.md "Deferred").
  - **A bonded app's credential:** referenced by `kid` = its bond slot (ID_CRED_I).
  - **An app that is not yet bonded:** sends its credential by value (`kccs`) in message_3.
- **Keys out:** the EDHOC exporter produces the OSCORE Master Secret (16 B) and Master Salt
  (8 B), as RFC 9528 Appendix A.1 specifies.

### 3.1 What the lock decides after message_3

The lock now knows, authenticated, which X25519 key it is talking to. Then:

| Lock state | Initiator key | EAD_3 | Result |
|---|---|---|---|
| unclaimed | any | none | **becomes bond #0** (admin), but only after the keypad code matches (§3.4), and only inside a factory-fresh / reset pairing window |
| claimed | = a bond, not expired | none | session for that bond and its role |
| claimed | unknown | valid invite (§3.3) | enroll as a member, then session |
| claimed | unknown | none or bad invite | EDHOC error, no session. Same refusal codes as v1 (`BOND_DENIED`, `MEMBER_REPLAY`, …) |

The v1 **ownership-theft rule** still holds. Only factory reset clears bond #0. Re-pairing a
claimed lock with a different key is refused, and the refusal changes nothing.

### 3.2 Why v1's `challenge` characteristic goes away

In v1 the challenge was the only protection against captured frames being replayed against a
re-invited bond (CONTRACT.md "The challenge check is UNCONDITIONAL"). In v2 every session has
fresh keys, so a frame captured in an old session cannot even be decrypted in a new one. The
attack is gone by construction, so the characteristic is not needed.

### 3.3 Member enrollment: the invite rides in EAD_3

The `OZINV1:` invite and its MAC are **byte-identical to v1** (CONTRACT.md "Member-enroll
lock-side algorithm"; ftpos vectors `member_invite_test.dart`). Only the delivery changes. It
travels in EDHOC's External Authorization Data field in message_3, encrypted and bound to the
handshake, instead of in a plaintext `member_enroll` write. The nonce replay cache, its
"successful enrollments only" rule and the idempotent-retry rule carry over unchanged.

### 3.4 First pairing: keypad confirmation code (operator, 2026-10-18)

The operator rejected a QR sticker (labour on every unit, and it can peel off). Instead:

1. On an **unclaimed** lock, after message_3, both sides compute
   `code = (uint32 big-endian of EDHOC_Exporter(label 0x4F50, context empty, 4 bytes)) mod 10^6`,
   shown as 6 digits with leading zeros. 0x4F50 is ASCII "OP", for OZKIE pairing; it is a
   private-use exporter label (RFC 9528 §10.1).
2. The app shows the code. The lock beeps and lights its keypad to ask for it.
3. The owner types the 6 digits and presses confirm on the lock keypad, within **60 s**.
4. **Match:** bond #0 is created, and the first OSCORE response to the app says `BOND_OK`.
   **Mismatch or timeout:** the session is dropped and nothing is stored. The app must start a new
   handshake, which gives a new code. There are no retries on the same code.
5. After **5 failed pairing attempts** the pairing window closes for 5 minutes. That stops
   anyone wearing out the keypad or the owner's patience.

The attacker cannot type on the keypad; the owner does. So a man-in-the-middle gets **one
1-in-10^6 chance per genuine pairing attempt by the owner**, and no way to retry on its own.

Why it is secure: a man-in-the-middle runs two separate handshakes, one with the phone and one
with the lock, and they produce different codes. It cannot make the two codes match except by a
1-in-10^6 guess per attempt. The code also proves the owner is physically at the door. It is used
once per lock, at first pairing only. Members don't use it: they are authenticated by their invite.

## 4. Message layer: OSCORE over CoAP

Every request is a CoAP message (RFC 7252 header, no UDP) protected by OSCORE:

- AES-CCM-16-64-128, so an 8-byte tag per message.
- Sender sequence number and replay window per RFC 8613 §7.4.
- The verb is the encrypted **Uri-Path**. Arguments are a **CBOR** body with integer keys (§5).
- Responses carry a CoAP code plus a CBOR result `{1: result code (int), …data}`.

### 4.1 Two kinds of security context

| Context | Lives | Used for |
|---|---|---|
| **Session** | one BLE connection, destroyed on disconnect | direct phone ↔ lock |
| **Relay** | persisted per bond, in lock flash and in the sender | commands via bridge + MQTT. Exists **only for bond #0 (owner) and `service` bonds**: a member never gets one (§6) |

A relay context is created by a normal EDHOC run (over BLE, or relayed through the bridge) and
marked persistent. Persistence follows **RFC 8613 Appendix B.1.1**: the sender's sequence number
is stored in blocks, so flash is not written on every message. The server team confirmed
(cross-session, 2026-10-18) that ozpms can do the same in InnoDB with an atomic `seq = seq + K`.

**One context = one active sender.** Block reservation stops sequence-number reuse, but not
reordering. If two server processes share a relay context, one process's numbers overtake the
other's, and the lock correctly rejects the late ones. A multi-process ozpms/ozlodge must shard
so that exactly one process owns a given lock's relay context at a time. Never design for
concurrent senders on one context.

**After a reboot**, each side does something different:

| Who rebooted | Its role on the relay context | Rule |
|---|---|---|
| lock | recipient of requests | replay window untrusted: the first request is answered with **Echo (RFC 8613 B.1.2)** before anything executes |
| ozpms / ozlodge / app | sender of requests | **B.1.1 only**: skip to the next reserved sequence block, no Echo |
| ozpms / ozlodge | recipient of Observe notifications (§5.1) | re-register the `events` Observe, which starts a fresh notification window |

A relay context is re-keyed by a fresh EDHOC at least every **30 days**, or when the sequence
number passes 2^32, whichever comes first. Revoking a bond deletes its contexts at once.

**Members are on-premises only** (operator, 2026-10-18). The lock refuses to mark a member's
context persistent, and refuses EDHOC from a member over the bridge relay. A member can only reach
the lock over a direct BLE session, standing at the door. Reasons (operator): a hotel guest must
not be able to open a room remotely, and a child or spouse must not be able to unlock the house
by accident from elsewhere.

### 4.2 Freshness for anything that moves the bolt

OSCORE stops replays, but not a message that was **captured and delivered late** (the classic
"held unlock" attack on relayed commands). Rule:

- Over a **session context**, the session itself is fresh. It was created on this connection,
  within the last few seconds, so no Echo is needed. This is v1's challenge semantics.
- Over a **relay context**, every actuating request (`lock/unlock`, `lock/lock`, `dev/reset`)
  first gets `4.01 Unauthorized + Echo`. The sender repeats the request with the Echo value
  within **10 s** 🟡 (provisional: to be measured end to end in ozkey-49 P4, server → MQTT → bridge
  → BLE → lock and back, including the bridge waking a sleeping AK1037 and opening a fresh
  connection). That costs one extra round trip, and only for actuation. Reads don't need it.
  Credential writes don't need it either, but they are protected by §4.3.
- **Consequence: actuation is never store-and-forward.** A queued relay unlock can only succeed
  while its original sender is online to answer the Echo. That is the point, since a held unlock
  is the attack. So servers **fail fast** on actuation instead of putting it in `pending_queue`.
  Credential writes may still be queued.

### 4.3 Credential and roster writes are conditional on the epoch

The OSCORE replay window accepts sequence numbers that arrive **out of order** within the window
(32 by default). Without a further rule, this attack works: capture `cred POST` (seq N) and hold
it. The admin sends `cred/<id> DELETE` (seq N+3). Then deliver N, which is inside the window and
not yet seen, and the deleted PIN/RFID is live again (found by the server team, 2026-10-18).

Rule: **every state-changing `cred` and `bond` request carries the epoch it was built against**
(body key 0, in the style of If-Match). The lock applies it only if that epoch equals the lock's
current epoch, then increments the epoch and returns it. Otherwise it changes nothing and
returns `STALE_EPOCH` together with the current epoch. The sender re-reads the state and rebuilds
the request. `cred/sync` already works this way. A late-delivered write is now always stale and
always harmless, whatever its sequence number.

**Consequence for senders:** the epoch is lock-wide, so writes to one lock are strictly serial.
A batch of N writes all built against epoch E gives 1 success and N−1 `STALE_EPOCH`.
- Seal each queued write **at flush time**, against the epoch the previous write returned, with
  one write in flight per lock. Never seal at enqueue time.
- Bulk changes go through `cred/sync`.
- Heavy contention between admin and service writes on one lock costs retries, not correctness.
  That is acceptable at hotel rates (server team, 2026-10-18).

## 5. Verbs and vocabulary

Paths are short ASCII strings, encrypted inside OSCORE, so they cost nothing in privacy. The
vocabulary follows the **Matter DoorLock cluster**: enum values for lock state, credential type,
operation source, alarm code and similar are taken from Matter so we don't invent our own.
🟡 Each numeric value is checked against the published Matter 1.x spec before this doc leaves DRAFT.

| v2 path | Method | v1 name (ozkey-28) | Role | Body keys |
|---|---|---|---|---|
| `lock/unlock` | POST | `lock.unlock` | member (BLE only), admin (BLE or relay) | — (Echo on relay) |
| `lock/lock` | POST | (new) | member+ | — |
| `lock/state` | GET | `lock.state` | member+ | → `{1: lock_state, 2: door_state, 3: battery %}` |
| `lock/settings` | GET / PUT | `lock.settings.*` | admin | `{1: volume, 2: autolock, 3: autolock_delay_s}` |
| `cred` | POST | `cred.put` | admin, service | `{0: epoch, 1: type, 2: cred_id, 3: secret, 4: from, 5: to, 6: uses}` (§4.3) |
| `cred/<id>` | DELETE | `cred.delete` | admin, service | — |
| `cred` | GET | `cred.list` | admin | → list |
| `cred/sync` | PUT | `cred.sync` | admin, service | full set, replaces |
| `bond` | GET | `bond.list` / `bond.query` | admin | → roster + epoch |
| `bond/<slot>` | DELETE | `bond.revoke` | admin | — |
| `bond/invite/<nonce>` | DELETE | `bond.invite.cancel` | admin | — |
| `time` | PUT / GET | `time.set` / `time.get` | bridge, admin | `{1: utc_s}`, monotonic-forward rule unchanged |
| `dev/info` | GET | `device.info` | member+ | → `{1: fw, 2: profile, 3: caps}` |
| `dev/reset` | POST | `device.reset` | admin | — (Echo on relay) |
| `events` | GET + Observe | `event.*` | admin, member (own), service | access, alarm, duress, battery, doorbell, bolt (§5.1) |

Credential `type` uses Matter's CredentialTypeEnum: PIN, RFID, Fingerprint. The v1 design points
all carry over:
- `cred_id` is allocated by the lock, not the caller's slot number.
- `uses` is real.
- `cred.sync` is the recovery path.
- Duress is a separate event from alarm.

**Result codes** keep the v1 **names** (`UNLOCK_OK`, `MEMBER_REPLAY`, …, ozkey-28 §4) but travel as
small integers. The name-to-integer table is fixed in this doc's appendix when it leaves DRAFT.
v1's overloaded `UNLOCK_DENIED` is split, as ozkey-28 §4 proposed: `DENIED` (policy),
`BAD_REQUEST`, `UNSUPPORTED`, `NO_SLOT`, `CRED_NOT_FOUND`, `EXPIRED`. New in v2: `STALE_EPOCH` (§4.3).

### 5.1 Events and the audit trail

Every event carries:
- the lock's **monotonic event sequence number** (always), so the receiver can detect gaps;
- **`utc_s` plus a time-basis flag** when the lock's clock has been set since boot (via `time`
  PUT from the bridge or an app);
- otherwise, the "none" flag.

The receiver uses the lock's `utc_s` when present, and its own arrival time only as a fallback.
Arrival time alone would stamp a whole offline backlog with its upload time.

**The audit source is the `service` bond, not the bridge.** ozpms/ozlodge observe `events` under
their own service bond, so events are OSCORE-protected from the lock all the way to the server. If
the bridge decrypted them instead, the audit trail would only be as trustworthy as the bridge,
which could drop, change or invent entries (server team, 2026-10-18). Together with the sequence
number, the server can detect missing and forged entries (ozkey-30 audit trail).

Who else observes events:
- **apps:** their own bond's events over BLE.
- **residential ozlock:** no service bond and no usage logs, unchanged.

## 6. Roles (lock-enforced)

| Role | Gets it how | May |
|---|---|---|
| `admin` (bond #0) | first pairing of an unclaimed lock | everything |
| `member` | invite from admin | unlock/lock, state, info, own events, **on premises only** (direct BLE session, never via the bridge, §4.1). Expiry enforced, as since v1 1.58 |
| `service` | admin enrolls the operator's own ozpms / ozlodge key | credential put/delete/sync, **event observe**. **No actuation**: it cannot unlock (the audit source, §5.1). **Never** bonds or bond #0 (CONTRACT.md "Service bond") |
| `bridge` | admin enrolls the bridge's key during bridge pairing | **time set only**. No events, nothing that opens a door |

## 7. What other teams need to do (after approval)

- **ftpos (BANOI):** an EDHOC initiator and OSCORE client in Dart. Invite generation is
  unchanged. Recommendation: wrap a C library through FFI (e.g. `ozcore` itself, or the open
  source `uoscore-uedhoc`) rather than write the crypto in Dart. An XF doc follows approval.
- **bridgeBLE:** relay `ozkie` frames between MQTT and GATT without opening them. It also holds
  its own `bridge` bond (time only). The relay part is a small change. 🔴 The MQTT buffer today is
  `setBufferSize(1024)` (`bridgeBLE.ino:2922`), and that buffer includes the topic and header. A
  full 1024-byte v2 message (a large `cred/sync`) would be **silently dropped**, the known 256-byte
  cliff at a new size. Raise it to 2048 when building the v2 relay, and test with a maximum-size message.
  - The bridge reports each lock's protocol from the service UUID it sees advertised (`…4b31` = 1,
    `…4b32` = 2). It goes as **`"ozkie_version": 1|2`** (integer) in the lock presence payload it
    publishes on `ozkie/<site>/locks/<id>/presence`, built by `ozBuildLockPresence()` in
    `common/ozpresence.h`.
  - The field is omitted when the bridge hasn't seen an advertisement. The server treats the
    bridge's value as authoritative over the app's (server team, 2026-10-18).
- **server, ozlock:** a per-lock protocol column (v1 / v2). For v2 locks: never wrap bytes in the
  55AA codec, pass OSCORE bytes through untouched, and route `4.01 + Echo` responses back to the
  original sender instead of treating them as final.
- **server, ozpms / ozlodge (service bond):** per the server team, 2026-10-18:
  - The long-term X25519 service key comes from process env or a 0600 key file. Never the DB
    or the repo.
  - Per-lock OSCORE relay contexts live in the DB, with the secret columns encrypted by a
    key-encryption key from env.
  - The sequence number is persisted in blocks, with one active process per lock context (§4.1).
  - Every cred write carries the epoch (§4.3). Actuation fails fast instead of queueing (§4.2).
  - Observe `events` under the service bond, and re-Observe after a restart (§4.1, §5.1).
  - The current code that builds Tuya DP_REPORT credential frames (ozlodge server.js:147-250,
    ozpms 130-226) is replaced by `cred` requests.
- **ozkit:** the test bench drives `port/host` first (ozkey-49 P1/P2), then real kits.

## 8. Open questions

1. ~~How the owner's phone verifies the lock at first pairing~~ **Decided (operator, 2026-10-18):
   a keypad confirmation code, specified in §3.4.** The QR sticker was rejected.
   The original proposal:
   - After EDHOC message_3 on an unclaimed lock, both sides derive a 6-digit code from the
     handshake (the EDHOC exporter, label "ozkie pair code").
   - The app shows the code, and the owner types it on the lock's touch keypad.
   - The lock creates bond #0 only if the codes match. A man-in-the-middle would hold a different
     handshake and so a different code. It gets a 1-in-a-million guess, and one try per pairing.
   - No sticker, no screen and no factory step. It also proves the owner is physically at the door.
     It uses the same idea as Bluetooth's passkey entry. It is used once per lock, at first pairing only.
2. ~~Remote unlock policy~~ **Decided (operator, 2026-10-18): owner only.** Members are
   on premises only, and the service bond cannot unlock at all. This is a change from v1, where
   members could unlock remotely.
3. ~~Events to the server~~ **Answered (server team, 2026-10-18):** service bond, end to end (§5.1).
4. **Matter enum check**: done by firmware before this leaves DRAFT.

## 9. Not in v2

- No Matter certification, no Thread in the lock, no TuyaOS, no Tuya DP framing anywhere
  (operator, 2026-10-18).
- No Ed25519 in the lock for the protocol. Firmware-image signing is the AK1037 secure-boot
  RSA/SM2 chain (ozkey-49 §3) and is separate.
- No change to the CH584M-era sims (`dlch584m_19/147`). They stay on v1 until retired.
