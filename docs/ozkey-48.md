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
> Rev 4 (2026-10-19, P2 implementation): pairing-code exporter label corrected to the private-use
> range (§3.4), provisional EAD label for invites (§3.3). Implemented and RFC-vector-tested in
> `blelock/ozcore` (ozkey-49 P2).
> Rev 3 (2026-10-18, operator approved via ozkey-50 §9): `issuer` role (§6) and guest-key-bound
> invites `OZINV2` (§3.3). No server holds bond #0.
> Rev 5 (2026-11-05, PM nexus-e3 ruling): short id = SHA-256(device_id)[0..3], replacing raw MAC
> bytes (§2.2, test vectors, 100K-site collision gate). App A: result 35 `PIN_TOO_SHORT`.
> Rev 6 (2026-11-05, PM nexus-9e GO on ozkey-55 §5b, server ozkey-97 review): the bridge relay.
> A `relay` characteristic (§2, §2.3), a closed type byte on MQTT (§2.4.1), topics and
> correlation (§2.4.2), one relay EDHOC per lock (§2.4.3), timers and retries side by side (§2.4.4),
> failover hand-off (§2.4.5), MQTT buffer sizing (§2.4.6).
> Rev 7 (2026-11-07, PM nexus-6e accepted, F-AK-WDT-2): SAFE mode after a watchdog reset loop
> (§6.1): unlock never refused, LOCK refused, the rest owner only. Result 36 `SAFE_MODE` (App. A),
> event type 4 `safe mode` (§5.1). Servers get 5.03 `SAFE_MODE`: surface it, never retry-loop.

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
- **Why AES-128 instead of v1's AES-256 (operator asked, 2026-10-18):**
  - It gives 128-bit security, and it is the cipher of BLE, Thread, Zigbee and Matter.
  - The AK1037 has AES-128 in hardware and lacks AES-256/GCM, so it is faster and uses less battery.
  - Fresh EDHOC keys per session give **forward secrecy**, which v1 explicitly lacked (claim C18).
    That is a bigger real-world gain than key length.
- None of these need a membership, a licence fee or a certification program.

## 2. Transport: BLE GATT

New service, so a v1 phone app can never be confused by a v2 lock:

`4f5a4b32-0001-4c4f-434b-000000000001` ("OZK2…LOCK")

| Characteristic | UUID | Props | Content |
|---|---|---|---|
| `info` | `…0002` | read | CBOR map: `{1: device_id, 2: fw, 3: lock X25519 pub (32 B), 4: state (0 = unclaimed, 1 = claimed), 5: profile}`. Readable without a session. Leaks nothing a scanner couldn't learn by connecting |
| `ozkie` | `…0003` | write + notify | OZKIE frames (§2.1), both EDHOC and OSCORE. A **session** link: the app at the door |
| `relay` | `…0004` | write + notify | The same frames (§2.1) for the **relay** link: the bridge only (§2.3) |

Advertising, the touch window, the busy flag and the single-connection rule are **unchanged from
CONTRACT.md**.
The one exception is the short id (§2.2).

### 2.2 Advertising and the short id (PM ruling 2026-11-05)

ADV (29 of 31 B): flags `06` · complete 128-bit UUID list (the v2 service) · name `OZLOCK`.
Scan response (23 of 31 B): 128-bit service data for the v2 service = `busy` (1 B: 0 = free,
1 = a link is up) ‖ **short id** (4 B). Apps scan actively to see it.

**short id = the first 4 bytes of SHA-256(device_id)**, hashed over the device_id's bytes exactly as
`info` key 1 carries them. There is no case folding and no format assumption, so any maker's id
format works. The id must be 1..40 bytes; a lock with any other id doesn't advertise.

v1 used the device_id's first 4 MAC bytes. On Espressif boards 3 of those are the vendor OUI, which
leaves 256 values per site and puts the OUI on the air. A hash spreads evenly whatever the OUI.

The short id is only a **pre-connect filter**. After connecting, the app MUST read `info` and check
the full device_id, and disconnect on a mismatch. 4 bytes still collide sometimes: at 100K locks,
about 1 pair is expected.

Test vectors (Python `hashlib.sha256(id.encode()).digest()[:4]`):

| device_id | short id |
|---|---|
| `ozk-acebe63acab8` | `2b 9c 2d 79` |
| `ozk-ACEBE63ACAB8` | `45 21 2d 8d` (no case folding) |
| `ozk-b0a6048b5fd8` | `84 f5 dc bc` |
| `x` | `2d 71 16 42` |

Full scan response for `ozk-acebe63acab8`, free:
`16 21 01 00 00 00 00 00 4b 43 4f 4c 01 00 32 4b 5a 4f 00 2b 9c 2d 79`.

Gate (`test_adv`): a synthetic site of 100 000 Espressif-OUI ids. It requires at most 6 colliding
short ids (random 32-bit expectation 1.16) and even first-byte buckets. The run on 2026-11-05 gave 0
collisions; the raw-MAC scheme gave 392 distinct values for the same ids.

### 2.1 Frame

```
byte 0:  bits 7..6 = type   (0 = EDHOC, 1 = OSCORE/CoAP, 2 = error)
         bit  5    = more   (1 = more fragments follow)
         bits 4..0 = frag # (0..31, resets per message)
byte 1…: payload fragment
```

Messages larger than one write (ATT MTU − 3) are fragmented.

Over MQTT (bridge relay), a message travels as a **binary payload**, never hex or base64: one
type byte, then one whole reassembled OZKIE message (§2.4.1). Apps should negotiate MTU 247 at
connect. Then every v2 message in this spec fits one frame except credential-by-value
enrollment (§3.3), which fits two. The maximum reassembled message is 1024 bytes.

### 2.3 Session link and relay link (rev 6)

The lock has two kinds of link, and the characteristic decides which one a write belongs to:

| | `ozkie` (session) | `relay` |
|---|---|---|
| Who | the app at the door, **and the bridge's own `bridge` bond** (time), on the bridge's connection | the bridge, carrying the remote senders: the owner's app via ozlock, ozpms, ozlodge |
| Roles | all, as §6 allows | **admin and service only** (`oz_lock.c`: other roles are refused at message_3) |
| OSCORE context | one, made by EDHOC on this connection, **gone on disconnect** | one per bond, **persisted** (`r0`..`r15`), found by `kid` |
| Echo | not needed | required for `lock/unlock`, `lock/lock`, `dev/reset`, and after a reboot (§4.2, App. A) |
| Members (§4.1) | allowed | refused (on premises only) |
| First pairing (§3.4) | allowed | refused: an unclaimed lock answers no relay EDHOC |

- The relay carries **only EDHOC and OSCORE-protected OZKIE messages**. There are no plaintext
  commands on it, so the bridge stays a blind relay and keeps nothing (PM condition 1). It needs no
  auth of its own: a stranger who writes to it gets exactly what they would get on `ozkie`.
- **The contexts are isolated per link.** A relay context can't be used, read or reset from the
  session link, and the reverse. A new handshake or a disconnect on one link leaves the other
  link's contexts alone. Tested both ways in `test_lock` `link_isolation`. A mutation check
  confirms it: a session link that falls back to relay contexts actuates an unlock without Echo.
- When the bridge's connection drops, the lock closes the relay link (`oz_link_close`). That wipes
  a half-done EDHOC and the link's Echo state. **Completed relay contexts persist.**
- **Two connections, as v1 has had since 2026-08-30** (`ozdoorlock_core.h` `OZ_BLE_MAX_LINKS 2`,
  a live bench finding). With one connection, the bridge's permanent link made every app at the
  door see `busy` forever. A **member** (relay refused, §4.1) would then have no path at all.
  - **Slot 1, the bridge:** permanent. On it the bridge writes relayed traffic to `relay`, and its
    own `bridge` bond (time) to `ozkie` as a session.
  - **Slot 2, the app at the door:** a session on `ozkie`. The `busy` flag and the non-connectable
    switch (CONTRACT.md [XF-47]) now mean "slot 2 is taken". A connection that holds only `relay`
    traffic doesn't make the lock busy.
  - Never more than 2: unbounded connectability is a DoS surface on a lock.
  - Each connection has its own session `oz_link`. There is **one** relay link. The first
    connection that writes to `relay` owns it until it disconnects, and `relay` writes from the
    other connection are dropped and counted.
  - 🟡 The C6 port supports this (NimBLE, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`). The AK1037 SDK's
    peripheral connection count is unverified until the kit (~11-12). If it is 1, the AK1037 lock
    needs a yield rule (open, ozkey-55), and this section says so then.

### 2.4 Bridge relay over MQTT (rev 6)

#### 2.4.1 Payload

`type (1 B) ‖ message`, binary. The type is a **closed set**:

| byte | meaning |
|---|---|
| `0x00` | EDHOC message (§3) or EDHOC error |
| `0x01` | OSCORE-protected CoAP message (§4) |
| `0x02` | error (the lock sends none today; a peer's type 2 ends a handshake in progress) |

Any other value is **dropped and counted, never parsed**, by the bridge in both directions, by ozlock
in both directions (a sender's downlink is refused with 400 before it is published; counters in C0b
metrics), and by the lock (`oz_lock.rx_bad_type`; it changes no state, not even a handshake in progress). An empty
payload, or one over 1 + 1024 bytes, is dropped and counted too. The bridge never opens the message.

#### 2.4.2 Topics and correlation

| direction | topic | addressed to |
|---|---|---|
| server → lock | `ozkie/<site>/bridges/<bridge_id>/ozkie/<lock_id>` | the **chosen bridge**, so with ≥ 2 bridges only one writes |
| lock → server | `ozkie/<site>/locks/<lock_id>/ozkie/<bridge_id>` | the **lock** (v1's U3 rule). The server subscribes to `locks/+/ozkie/+` |

- QoS 0, never retained. Loss and duplicates are handled above MQTT (§2.4.4).
- The trailing `bridge_id` on the uplink is the relaying bridge. Two bridges holding one lock
  (XF-151) show up as uplinks from two `bridge_id`s.
- A downlink for a lock this bridge doesn't hold is dropped and counted. It isn't queued or forwarded.
- **The server correlates OSCORE responses by (lock_id, CoAP token). The `kid` identifies the
  context on requests only**, because an OSCORE response omits it (RFC 8613 §6.1).
  - Every sender uses a **random token of ≥ 4 bytes**. ozlock refuses a second in-flight
    request on the same (lock_id, token).
  - Observe notifications reuse the registration's token.
  - A response that matches nothing is dropped and logged.

#### 2.4.3 One relay EDHOC per lock

EDHOC on the relay is raw EDHOC (type 0), not CoAP, so it has no token. The lock's relay link
holds **one** responder handshake.
- **The lock:**
  - drops a handshake waiting for message_3 after **30 s** (`OZ_LOCK_EDHOC_TIMEOUT_MS`);
  - treats a message_1 that arrives mid-handshake as **abort and restart**: message_1 opens with
    a CBOR integer (< 0x40), message_3 is a byte string;
  - resets on a failed message_3 and sends EDHOC error 1.
- **The server (ozlock)** holds one EDHOC **lease per lock across all senders, 35 s**. A second
  sender's message_1 is refused as busy and never queued. Every type 0 uplink belongs to the
  lease holder. A type 0 uplink with no lease open is dropped and logged.
- With the restart rule, a lease that ends early costs one failed handshake, never two.

#### 2.4.4 Timers and retries

| timer | value | owner | rule |
|---|---|---|---|
| message_3 wait | 30 s | lock | half-done handshake dropped (§2.4.3) |
| relay EDHOC lease | 35 s | server | must outlast the lock's 30 s |
| Echo validity | 10 s, one use | lock | the repeat after `4.01 + Echo` goes at once, inside 10 s |
| request timeout | 10 s | each sender | no response in 10 s = lost |
| pairing window | §3.4 | lock | session link only (no pairing on the relay) |
| request timeout value | 🟡 provisional | — | 10 s is the §4.2 end-to-end target; F1 measures it on the bench and rev 7 fixes it |

- **These are the SENDER's rules** (the app, ozpms, ozlodge). OSCORE hides the real CoAP method
  (the outer code is POST/FETCH), so a relay can't tell a read from an unlock. **ozlock's relay
  never retries, never re-sends and never queues any v2 frame. If the lock's bridge is offline, it
  fails fast with 503.** That is what makes "a v2 unlock is never held" enforceable on the server
  (ozkey-97).
- **No CoAP-layer retransmission.** The lock keeps no response cache. A retransmitted request has
  the same PIV, so the replay window drops it **with no reply** (RFC 8613 §7.4). Senders send CON
  or NON, but never resend the same bytes.
- **A retry is always a new request** with a new PIV and a new token.
  - **Reads** (CoAP GET, e.g. `lock/state`; an `events` re-registration) may be retried that
    way, at most twice.
  - **Actuation (`lock/unlock`, `lock/lock`, `dev/reset`) is never auto-retried.** A lost response
    is reported as outcome **"unknown"**. The app shows it, and a user's new tap is a new request.
    **A v2 unlock is never queued or held for later delivery** (PM nexus-9e).
  - Today's server (ozkey-97, 2026-11-05): nothing re-sends after a lost or late reply. Each
    pending_queue row is published at most once, then marked `sent` (queue.js:140-144). A reset with
    no reply reads `unknown`. But **v1** remote unlock is held for 60 s (30 s when assisted) and
    delivered at the next wake (api.js:1126-1142). That is operator question Q2, and it doesn't
    carry over to v2: the v2 path is new code and follows this section.
- Writes with an epoch (`cred`, `bond`) may be retried as a new request. The epoch rule (§4.3) makes
  a duplicate harmless: the second write gets `STALE_EPOCH` and the current epoch.

#### 2.4.5 Failover hand-off

1. The lock takes one central. Only the bridge holding the BLE link can write to `relay`.
2. When that link drops, the lock closes its relay link (§2.3) and advertises again. Another bridge
   connects and announces it in the lock's presence (`via`, XF-144).
3. The server moves `lock.bridge_id` to the new bridge when its presence carries `via`, using the
   XF-144 reconciliation and the XF-151 gate: while the current bridge is still confirmed online,
   the claimant is refused and sent `release_lock`. From then on the server sends to the new
   bridge's downlink topic. The old bridge drops anything still addressed to it (§2.4.2).
4. An exchange in flight is pinned to its bridge (EDHOC m1 → m3, or request → `4.01 + Echo` →
   repeat). If the bridge changes mid-exchange, the server aborts it and starts again on the new
   bridge. It is never split across bridges. The server aborts the exchanges pinned to the old
   bridge **as soon as it moves `bridge_id`**, without waiting for the 10 s timeout. An aborted
   actuation is "unknown" (§2.4.4).
5. **No new EDHOC is needed after a failover.** The relay contexts are stored on the lock, so the
   next request just uses them. Actuation needs Echo as always.

#### 2.4.6 MQTT buffer

PubSubClient's buffer holds the whole packet: header + topic + payload.

The largest relayed packet is at most **1,230 B**, taken from the server's column maxima. No new
length limits are imposed:
- payload 1 + 1024 = 1,025 B;
- topic ≤ 200 B: `ozkie/` + `sites.id` ≤ 50 + `/bridges/` + `bridges.id` ≤ 64 + `/ozkie/` +
  `locks.id` ≤ 64;
- header 5 B.

The bridge's buffer is **2,048 B**, a margin of about 818 B (40 %). Before, it was 1,024, and a full
v2 message would have been silently dropped (the 256-byte cliff at a new size). The F1 gate
sends a maximum-size message in each direction, **with a 50-character site and 64-character ids**
(not `lab`).

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

**Guest-key binding (🟢 operator approved 2026-10-18, ozkey-50 §4).** An invite can name the one app
key that may redeem it.
- **Format:** `OZINV2:`, the same fields as OZINV1 plus:
  - `bind` = the first 16 bytes of SHA-256 of the redeeming app's X25519 static public key;
  - `iss` = the issuing bond's id.
  Both are covered by the invite MAC.
  - **Naming (P2, 2026-10-19):** the app's existing invite already has a JSON `"v":2` (XF-87, signed
    membership expiry), still under the `OZINV1:` prefix. So the guest-bound form is **canonical
    version 3** under the `OZINV2:` prefix:
    `3|d|i|r|l|n|e|me|bind_hex|iss`, with the same HKDF key as v1/v2.
    `blelock/ozcore` reproduces ftpos's frozen v1 and v2 MACs byte for byte.
  - **Transport in EAD_3:** a CBOR array `[v, d, i(32 B), r, l, n(16 B), e, me, m(32 B)
    (, bind(16 B), iss)]`, not the QR's base64url JSON. The fields and the MAC are unchanged. The app
    transcodes what it scanned, and the lock needs no JSON or base64 decoder. Labels containing `|`
    are refused (they would make the canonical string ambiguous), so the app should refuse them at
    invite time.
- **Lock check:** EDHOC method 3 already authenticates the initiator's static key (ID_CRED_I), so
  the lock checks `bind` against the key that just completed the handshake.
- **On mismatch:** `INVITE_BOUND_ELSEWHERE`, not consumed. The rightful guest can still redeem it.
- **When required:** `bind` is mandatory on invites created by `issuer`, and optional on admin-made
  invites. OZINV1 stays valid from admin.
- **What it buys:** an invite intercepted at a reseller, in Cloudflare, or in a message thread is
  useless to anyone else.
- **Vectors:** `OZINV2` vectors are added to the P2 deliverables for ftpos.
- **EAD label** 🟡 (P2, 2026-10-19): RFC 9528 §10.5 has **no private-use EAD range**. Labels 24–65535
  are "Specification Required" with IANA. Provisional label **20297** (0x4F49, "OI"), sent as
  **critical** (−20297), so a lock that doesn't understand it refuses the handshake. It is safe
  inside our closed system, but before any public release either register it (this doc can serve
  as the specification) or accept the small collision risk. **Operator's call.**

### 3.4 First pairing: keypad confirmation code (operator, 2026-10-18)

The operator rejected a QR sticker (labour on every unit, and it can peel off). Instead:

1. On an **unclaimed** lock, after message_3, both sides compute
   `code = (uint32 big-endian of EDHOC_Exporter(label 0xCF50, context empty, 4 bytes)) mod 10^6`,
   shown as 6 digits with leading zeros. 0xCF50 (53072) is in the private-use exporter label range
   32768–65535 (RFC 9528 §10.1). **Correction 2026-10-19 (P2):** rev 3 said 0x4F50, which is in the
   Expert Review range (24–32767), not private use. Nothing had shipped with it.
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
- Responses carry a CoAP code plus a CBOR result `{0: result code (int), …data}`, with data keys
  as in the §5 table (**rev 4:** the result moved to key 0 because the §5 table already used key 1
  for data; see Appendix A).

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
| `lock/state` | GET | `lock.state` | member+ | → `{1: lock_state, 2: door_state, 3: battery %, 4: battery level (0 ok, 1 low, 2 critical)}`. `lock/lock` answers 5.03 `BATTERY_LOW` at critical; `lock/unlock` is never refused for the battery (ozkey-49 P5-F) |
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

**Wire (built 2026-10-29 for the admin bond, ozkey-49 P5-E0; the service bond and member
filtering add roles, not fields):**
- `GET events`: answers 2.05 `{0: OK, 1: next_seq, 2: oldest_seq_held}`.
  - With **Observe 0** it also registers. There is one registration per BLE session and one per
    bond on the relay; a new one replaces the old. Optional **Uri-Query `after=N`** is the last
    seq the reader already has. Without it, the lock sends every event it still holds.
  - **Observe 1** deregisters. A BLE registration ends with the session. A relay registration ends
    with a new relay context for that bond, a revoke, or a reboot; re-register after a restart.
  - Roles today: **admin only**; anyone else gets `FORBIDDEN`.
- **Notifications:** CoAP NON 2.05 with the registration's token, an increasing Observe value and
  an OSCORE Partial IV. The payload is one event:
  `{1: seq, 2: type, 3: time basis (0 none, 1 utc), ?4: utc_s, ?5: actor bond slot, 6: data}`.

  | type | name | data |
  |---|---|---|
  | 0 | gap | `{1: events missed}`. `seq` is the first missed |
  | 1 | keypad lockout | `{1: level (1 = the first, after 5 misses), 2: seconds}` |
  | 2 | card reader lockout | as type 1, for wrong RFID cards (own counter) |
  | 3 | battery | `{1: level (0 ok, 1 low, 2 critical), 2: percent}`, on each level change (ozkey-49 P5-F) |
  | 4 | safe mode | `{1: watchdog resets in a row, 2: 0}`, once per boot that starts in SAFE mode (§6.1) |

  - Further types (access, alarm, duress, doorbell, bolt) take the next numbers as each
    is built. A receiver ignores data keys it doesn't know.
- **Ring and loss:** the lock holds the last **16** events in RAM. When the ring is full the oldest
  is dropped. A reader that fell behind gets one `gap` event first, then the oldest held.
  - Notifications are NON and not retransmitted. The receiver detects loss from `seq` and
    re-registers with `after=` to refill from the ring.
  - Sequence numbers are 32-bit (modulo 2^32) and never repeat across reboots: the lock reserves
    them in blocks of 32 in kv. A reboot loses the ring and shows as a jump in `seq`.
  - A factory reset restarts them at 0.

## 6. Roles (lock-enforced)

| Role | Gets it how | May |
|---|---|---|
| `admin` (bond #0) | first pairing of an unclaimed lock | everything |
| `member` | invite from admin | unlock/lock, state, info, own events, **on premises only** (direct BLE session, never via the bridge, §4.1). Expiry enforced, as since v1 1.58 |
| `service` | admin enrolls the operator's own ozpms / ozlodge key | credential put/delete/sync, **event observe**. **No actuation**: it cannot unlock (the audit source, §5.1). **Never** bonds or bond #0 (CONTRACT.md "Service bond") |
| `bridge` | admin enrolls the bridge's key during bridge pairing | **time set only**. No events, nothing that opens a door |
| `issuer` | admin enrolls the hotel server's (ozlodge's) key. 🟢 **Operator approved 2026-10-18** (ozkey-50 §3) | **Create member invites only**, each with: a validity window ≤ `max_days` (set by admin at enrolment; default 30, range 1–90); `start` ≥ now − 1 h; a guest-key binding (§3.3), which is required. **No actuation, no credentials, no events, no bond or admin verbs.** The lock rejects anything else with `FORBIDDEN`. Admin can revoke it like any bond, and revocation voids the unredeemed invites it issued (by the issuer's bond id in the invite) |

### 6.1 SAFE mode (F-AK-WDT-2, PM rulings 2026-11-06)

A lock whose watchdog reset it `OZ_BOOTGUARD_LIMIT` = 3 times in a row boots in SAFE mode for that
boot (ozkey-49, F-AK-WDT-1). A power-on reset (batteries out and in) clears the count. A run that
stays up 2 minutes clears it too, but never while in SAFE mode. While SAFE:

- **Unlock is never refused.** `lock/unlock` from anyone who may unlock (admin, member), a keypad
  PIN and an RFID card all work as usual. This is the never-lock-anyone-out rule, the same rule as
  the battery's (P5-F).
- **LOCK is refused for everyone.** `lock/lock` answers 5.03 `SAFE_MODE`, the motor is not driven,
  and `lock/state` reads `not fully locked`. The motor may be what keeps resetting the lock.
- **Everything else is owner (admin) only.** Any other request from member, service, bridge or issuer
  answers 5.03 `SAFE_MODE`. That covers state, info, time, credentials and invites. The admin keeps
  every verb, including `events` and `dev/reset`. A factory reset does not leave SAFE mode, because
  SAFE belongs to the boot, not the bond table.
- **One `safe mode` event** (§5.1 type 4) is logged when SAFE starts, `{1: resets in a row}`. The owner
  sees it on `events`. Service-bond `events` aren't built yet (§5.1), so a server learns of SAFE mode
  from 5.03 `SAFE_MODE` on its next `cred` request. The server shows "lock in SAFE mode" to the owner
  and the console, and must not retry in a loop (PM, 2026-11-07).

**No server ever holds bond #0** (ozkey-50 §3, server review). The servers hold only `service`,
`issuer` and the ozpms custodian, whose limits the lock enforces (ozkey-51). Server-side remote unlock
of hotel rooms is **not** granted. It would need an explicit operator decision recorded here.

## 7. What other teams need to do (after approval)

- **ftpos (BANOI):** an EDHOC initiator and OSCORE client in Dart. Invite generation is
  unchanged. Recommendation: wrap a C library through FFI (e.g. `ozcore` itself, or the open
  source `uoscore-uedhoc`) rather than write the crypto in Dart. An XF doc follows approval.
- **bridgeBLE:** relay frames between MQTT and the lock's `relay` characteristic without opening
  them (§2.3, §2.4: the wire, the topics, the timers and the failover are specified there). It also holds
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

## Appendix A — wire details fixed by the P2 implementation (rev 4, 2026-10-19)

These are the choices `blelock/ozcore` (`oz_lock.c`) makes where the text above was silent. They
are tested end to end in `tests/test_lock.c`. Server and app builds should match them; raise any
disagreement before S3 interop.

**Credentials.** `CRED_x` for every party is the canonical 44-byte CCS
`a1 08 a1 01 a3 01 01 20 04 21 58 20 ‖ X25519 public key`, i.e.
`{8: {1: {1: 1 (OKP), -1: 4 (X25519), -2: x}}}`.
- The lock's `ID_CRED_R` is `{4: h'00'}`.
- A bonded app's `ID_CRED_I` is `{4: h'<slot>'}`, sent in compact form as the one-byte integer.
- An app that is not yet bonded sends `{14: CCS}` (kccs). The lock accepts only the canonical form
  above, so both sides hash identical bytes.

**Connection IDs.** The lock picks `C_R` = `0x01` on BLE. On the relay it picks the first value in
`0x02…0x17` that no stored relay context uses as its Recipient ID. The relay finds a context by
the OSCORE `kid`.

**Pairing (§3.4).** After message_3 on an unclaimed lock, the app sends `POST pair` (no body). The
lock holds the response until the keypad answers:
- `2.04 {0: BOND_OK, 1: 0}` on a match;
- `4.01 {0: PAIR_FAILED}` on a mismatch or timeout.

Any other request during pairing gets `4.03 {0: PAIR_PENDING}`. The owner's later sessions use
`ID_CRED_I = {4: h'00'}`.

**Extra verb.** `POST bond {0: epoch, 1: role (3 service, 4 bridge, 5 issuer), 2: pub (32 B),
3: label, 4: max_days (issuer, 1–90, default 30)}`, admin only, answers
`{0: BOND_OK, 1: slot, 2: epoch}`. It is the enrolment §6 describes.

**State-changing answers** carry the new epoch at key 2, and `STALE_EPOCH` carries the current
epoch at key 1. Cred and bond listings never include secrets: bonds show the first 8 bytes of
the key as a tag.

**Echo.** It is required on the relay for `lock/unlock`, `lock/lock` and `dev/reset`. After a
lock reboot it is required on the first request of each restored relay context (B.1.2). The Echo
value is 8 random bytes, good for one use within 10 s.

**Result codes** (§5; names as in v1, ozkey-28 §4):

| int | name | int | name |
|---|---|---|---|
| 0 | OK | 20 | CRED_NOT_FOUND |
| 1 | UNLOCK_OK | 21 | EXPIRED |
| 2 | LOCK_OK | 22 | STALE_EPOCH |
| 3 | BOND_OK | 23 | FORBIDDEN |
| 4 | MEMBER_OK | 24 | BOND_DENIED |
| 5 | REVOKE_OK | 25 | MEMBER_REPLAY |
| 6 | CRED_OK | 26 | MEMBER_FULL |
| 7 | TIME_OK | 27 | MEMBER_EXPIRED |
| 8 | PAIR_PENDING | 28 | INVITE_BOUND_ELSEWHERE |
| 16 | DENIED | 29 | PAIR_FAILED |
| 17 | BAD_REQUEST | 30 | PAIR_LOCKED_OUT |
| 18 | UNSUPPORTED | 31 | NOT_FOUND |
| 19 | NO_SLOT | 32 | REVOKE_DENIED |
| | | 33 | TIME_REJECTED |
| | | 34 | BATTERY_LOW |
| | | 35 | PIN_TOO_SHORT |
| | | 36 | SAFE_MODE |

`PIN_TOO_SHORT` (2026-11-05, PM ruling): a `cred` POST or a `cred/sync` entry of type PIN with fewer
than `OZ_LOCK_PIN_MIN` = 6 digits. A sync with one such entry is refused whole and changes nothing.
Other malformed creds stay `BAD_REQUEST`. The relay can't see sealed digits, so the lock and the
app enforce this; the server can't.

EDHOC-stage refusals (`BOND_DENIED`, `MEMBER_REPLAY`, `INVITE_BOUND_ELSEWHERE`, `PAIR_LOCKED_OUT`, …)
travel as EDHOC error code 1 with the name as the text.

**Not built yet:** `events` for the service bond and members (admin is built, §5.1), `lock/settings`, custodial mode (ozkey-51), `ozkie_version` on
the bridge. 🟡 The Matter enum values (lock state 0/1/2, credential types PIN 1, RFID 2,
Fingerprint 3) are still unchecked against the Matter 1.x spec.
