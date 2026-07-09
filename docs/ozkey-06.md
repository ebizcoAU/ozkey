# OZKEY-06 — End-to-End Envelope + BLE Transport Contract (Market A / OZLOCK)

> **DRAFT 2026-07-08 — for the BANOI team (P4 unblock).** Freezes the two
> pieces ozkey-05 §13 deferred: (1) the **AES-256-GCM application envelope**
> that makes OZLOCK a blind relay — with reproducible byte vectors so
> `ozkey_commissioner`'s `seal`/`open` byte-match a reference, and (2) the
> **BLE transport** for the at-the-door path (accepts XF-42 §6.1's
> operational-advertising ask). Envelope framing is **identical** to XF-42
> §14.3, which BANOI already implemented — this doc pins the bytes and the key
> schedule, it does **not** move the framing. Consumers: `ozkey_commissioner`
> (Dart), LockSim (reference lock, envelope), a new **`blelock/` bleno
> peripheral** (reference lock, BLE), OZLOCKSERV (relay-opaque switch),
> ESP32-C6 firmware. Depends on: ozkey-04 (identity/commissioning), ozkey-05
> (rendezvous), XF-42 §13/§14.

---

## 1. What this changes vs the lab today

The lab (ozkey-05) builds DPID frames **server-side, plaintext**. ozkey-06
flips that to the §13 target:

| | Lab today (ozkey-05) | ozkey-06 target |
|---|---|---|
| Who builds the `55 AA` frame | OZLOCKSERV | the **app** (keyring core) |
| What crosses OZLOCK | plaintext `payload_hex` | **AES-GCM ciphertext** OZLOCK can't read/forge |
| Frame in the queue | `payload_hex` | `envelope_hex` (sealed) |
| At-the-door path | not built | **BLE GATT**, same envelope |

Migration order is §9. Until it lands the lab keeps working plaintext; nothing
here breaks the P1–P3 surface.

## 2. Trust boundary (restated, one line)

Root of trust is **physical proximity at the BLE ceremony**, where app and
lock derive a shared secret OZLOCK never sees. OZLOCK authenticates neither
party (ozkey-05 amendment); the envelope is therefore the *only* confidentiality
+ integrity layer, and it is mandatory before any real door.

## 3. Key schedule

### 3.1 Pairing secret (X25519 ECDH, at the ceremony)

App and lock each generate an X25519 keypair; the app writes its public key in
the provision payload (or a dedicated `key-exchange` GATT step), the lock
returns its public key on `status`. Both compute the shared secret per
**RFC 7748** (standard X25519 — use RFC 7748 §5.2 test vectors to verify your
curve implementation; not re-pinned here). That 32-byte shared secret is
`pairing_secret`, persisted in each side's secure store, never transmitted.

### 3.2 Per-direction keys (HKDF-SHA256)

Two keys so a captured uplink frame can't be replayed downlink:

```
salt = utf8(device_id) ‖ utf8(app_id)
key_app_to_lock = HKDF-SHA256(ikm=pairing_secret, salt, info=utf8("ozkey/app->lock"), L=32)
key_lock_to_app = HKDF-SHA256(ikm=pairing_secret, salt, info=utf8("ozkey/lock->app"), L=32)
```

`app->lock` seals commands (grant/revoke/unlock frames); `lock->app` seals
events (door-log JSON). The receiver opens with the peer's sealing key.

## 4. Envelope wire format (frozen — identical to XF-42 §14.3)

```
envelope = ver(1) ‖ counter(8, BE) ‖ nonce(12) ‖ ciphertext(n) ‖ tag(16)
  ver        = 0x02                        // 0x01 = legacy plaintext (lab); 0x02 = this
  counter    = per-direction monotonic u64, big-endian
  nonce      = random(4) ‖ counter(8, BE)  // 12 bytes; low 8 == the header counter
  ciphertext = AES-256-GCM(key_dir, nonce, aad, plaintext)
  tag        = 16-byte GCM auth tag
  aad        = ver(1) ‖ counter(8, BE) ‖ utf8(device_id)
cipher = AES-256-GCM, key = key_app_to_lock | key_lock_to_app (§3.2)
```

- **Nonce uniqueness:** the counter in the low 8 bytes guarantees a fresh nonce
  per message under one key; the 4 random bytes are belt-and-suspenders.
- **Nonce-counter check:** on open, assert `nonce[4:12] == counter` (header) —
  a mismatch is a malformed/tampered frame, reject.
- **Anti-replay:** the receiver tracks the highest counter seen per direction
  per bond and rejects `counter ≤ seen`. Counters are per-bond (a household with
  N phones = N app→lock counters, §6).
- **AAD** binds the frame to `ver`, `counter`, and `device_id` so a valid frame
  can't be replayed against a different lock or downgraded to `ver 0x01`.

## 5. Test vectors (reproducible — byte-match your `seal`/`open`)

Fixed inputs (chosen for reproducibility, not real keys):

```
pairing_secret = 000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F
app_id         = app_00112233445566778899aabb
device_id      = ozl-00112233445566778899aabbccddeeff
salt           = utf8(device_id) ‖ utf8(app_id)
key_app_to_lock = 919E05D8DAB046BD5F2721FFE7FAE0FA039A2F0399024964F2C8FDAC9C9E5AC8
key_lock_to_app = AD1DAF444B95706AE9D97286498B79BA372C1F04C8ABB17B12C937C58B62E4D0
```

### 5.1 app→lock — sealing `SAMPLE_ADD_TEMP_PIN_FRAME`

```
counter    = 1
nonce      = AABBCCDD 0000000000000001            // random(4)=AABBCCDD
aad        = 02 0000000000000001 <utf8 device_id>
plaintext  = 55 AA 00 06 00 14 15 00 00 10 00 0E 34 38 32 39 31 35 69 55 B9 00 6B 36 EC 7F 0C
ciphertext = 9E129265AD2537F37A1485E72BD9F36CB304876C8F777B1A12925A
tag        = 7E0F57788C84DF29DD08A68A74E44458
ENVELOPE   = 020000000000000001AABBCCDD00000000000000019E129265AD2537F37A1485E72B
             D9F36CB304876C8F777B1A12925A7E0F57788C84DF29DD08A68A74E44458
```

`open()` of that envelope with `key_app_to_lock` yields exactly the
`SAMPLE_ADD_TEMP_PIN_FRAME` plaintext (verified). The plaintext is the
byte-verified DPID frame from ozkey-04 §71 / the §5.1-here PIN sample.

### 5.2 lock→app — sealing a door-event log JSON

```
counter    = 7
nonce      = 11223344 0000000000000007
plaintext  = {"result":"granted","detail":"REMOTE UNLOCK COMMAND","ts":1767225600000}
ENVELOPE   = 020000000000000007112233440000000000000007F87FD3DCAADC2DEDEBA91E5784
             BE64B327BAD1F9DA838C4BB88E8C9E45392590BB3528448E101B7BCD8F1579FD5AE0A
             AB97129E20C7D100353A61E0DD8864A3951ECF9BC5926601C4FFBE431AAD97CF046B7
             1F01928B2418
```

(Whitespace in the envelope hex is for wrapping only — concatenate.) Two green
tests: `open(§5.1) == SAMPLE_ADD_TEMP_PIN_FRAME` and
`open(§5.2) == the log JSON`. Add a **tamper test** (flip any byte → GCM tag
fails → reject) and a **replay test** (`counter=1` twice → second rejected).

## 6. Keyring model (formalizes XF-42 §13.1/§14.2)

The lock stores a **set of bonds**, not one owner:

```
Bond { app_pubkey, pairing_secret, counter_out, counter_in, added_by, added_at }
```

- **First pairing** writes bond #0 (owner). The owner adds household phones by
  writing their `app_pubkey` over BLE (or an owner-sealed `add-key` command);
  each new phone runs its own §3.1 ceremony → its own `pairing_secret` + bond.
- **Revoke** is owner-authored (`revoke-key` command naming a bond) and
  **delivered on heartbeat**, so a released/lost phone dies within one wake
  cycle even if it never re-contacts the cloud. No server-signed certs
  (XF-42 §6.1.3 withdrawn — the lock's key-set *is* the authority).
- **Lost-phone recovery:** (a) another household phone revokes + re-adds; (b) if
  it was the only phone, **owner-reset at the door** (hardware button + physical
  presence) clears the key-set and re-pairs. Physical access is the recovery
  root — the sovereignty feature, not a gap.
- **Backup seam:** `exportKeyring(passphrase)→blob` / `importKeyring` (Argon2id
  + AES-GCM). Storage location deferred (iCloud Keychain / printed code) —
  **never** on OZLOCK. Build the seam now.

## 7. BLE transport — accepts XF-42 §6.1

The at-the-door path. **Amends ozkey-04 §4's "never advertises while
operational":** an enrolled lock MAY advertise for owner reconnection.

### 7.1 Advertising (privacy-preserving)

- BLE **resolvable private address** (rotating) + manufacturer-data token
  derived from the device key, so only apps holding a bond can resolve
  "that's my Cửa trước". No stable beacon for strangers to track.
- Advertised at a slow interval while operational; firmware owns the
  battery-vs-latency number (still the market-A driver, §10 Q1).

### 7.2 GATT layout (extends ozkey-04 §4)

| Characteristic | UUID | Props | Flow | Content |
|---|---|---|---|---|
| `provision` | `4f5a4b45-5900-0002-0000-6f7a6b657900` | write | app→lock | ozkey-04 §5 payload + app X25519 pubkey (commissioning) |
| `status` | `4f5a4b45-5900-0003-0000-6f7a6b657900` | notify | lock→app | ozkey-04 §7 ladder + lock X25519 pubkey |
| `control` | `4f5a4b45-5900-0004-0000-6f7a6b657900` | write+notify | both | **§4 envelope**, plaintext = a fixed verb (§7.3) |

### 7.3 Control verbs (fixed set — NOT free-form DPID over BLE)

Sealed in the §4 envelope over `control`; the lock verifies, executes, notifies
a sealed result. Fixed vocabulary (a compromised app can't inject arbitrary
frames):

```
unlock            → lock runs unlockCycle
lock              → force relock
status?           → sealed state snapshot on notify
add-key <pubkey>  → owner-only; new bond (§6)
revoke-key <id>   → owner-only; drop a bond
```

Same envelope, same anti-replay counters as the relay path — **one crypto
design, two transports**. Credential grants (temp PIN/RFID) still go as DPID
frames, but sealed and carried as an `issue`/`revoke` verb payload.

### 7.4 Offline audit

BLE unlocks are logged locally on the lock and uplinked on next broker contact
(`…/log`, sealed lock→app for the app; the plaintext summary for OZLOCK push is
the §10-open log question). No BLE-shaped hole in the §6.4 audit trail.

## 8. Reference lock for BLE — `blelock/` (Node bleno peripheral)

**LockSim (browser) cannot be the BLE reference** — Web Bluetooth is
central-only, no advertising/GATT-server API (ozkey-03 §10.1). So ozkey-06 adds
a separate **`blelock/`**: a Node peripheral using `@abandonware/bleno` on the
Mac's radio that advertises the §7.1 service and hosts §7.2 characteristics,
running the *same* payload + §4 envelope + §6 keyring contract as LockSim's
Mode C. BANOI (central, on a phone — fine on iOS/Android) scans, connects,
commissions, and drives §7.3 verbs against it. This is the P4 bench target
before ESP32-C6 firmware exists. Scope: advertise, provision/status/control
GATT, X25519 ceremony, envelope open/seal, the five verbs, local-log + uplink.
LockSim stays the **network/relay** reference; `blelock` is the **BLE** one.

## 9. Migration (ordered; each step ships independently)

1. **Envelope in LockSim** (reference lock) — Mode C gains `ver 0x02`
   seal/open; publishes `envelope_hex`. LockSim becomes the byte-match oracle;
   emit vectors identical to §5.
2. **Keyring core seals** (BANOI) — `ozkey_commissioner` builds+seals; already
   built against §14.3, now cross-verified vs §5 and LockSim.
3. **OZLOCK relay-opaque** — queue/relay `envelope_hex` untouched; server stops
   building frames on the sealed path (keeps plaintext `ver 0x01` for the
   legacy lab loop during transition). `/locks/:id/unlock` etc. accept a sealed
   body from the app instead of building server-side.
4. **`blelock/` peripheral** — BLE reference; unblocks P4 + `flutter_blue_plus`
   / `local_auth`.
5. **Firmware** — ESP32-C6 implements §3/§4/§6/§7; `blelock` is its conformance
   oracle.

## 10. Open questions

1. **Advertising battery cost** (§7.1) — firmware measures connectable-advert
   current on C6; drives the market-A battery story. Still the №1 hardware input.
2. **Log push vs e2e** (XF-42 §13.4) — **RESOLVED 2026-07-10 (operator):
   option (b)** — the lock uplinks a plaintext **`result`-only** summary
   (`granted`/`denied`/`expired` + `device_id` + `ts`, **no** `detail`, PIN,
   or holder) that OZLOCK stores for the event feed and fans out as push; the
   full detail rides the **e2e envelope** (§4), readable only by the app. So a
   database breach or a curious relay sees "lock X, granted, 14:03" and nothing
   more. Firmware/keyring emit both: the summary on `…/log` cleartext, the
   sealed detail alongside (or as `ver 0x02` on the same topic — §9 keeps the
   plaintext summary at `ver 0x01`).
3. **X25519 vs P-256** — §3.1 specs X25519 (simpler, fast, no point-validation
   footguns). ozkey-04 §3 named P-256 for the eFuse *identity* key. These can
   differ (identity = P-256 attestation; session = X25519 ECDH) or unify —
   firmware-team call; default: keep both, they serve different roles.
4. **`add-key` authorization over BLE** (§6/§7.3) — owner-only enforced how:
   bond #0 flag, or a signed owner assertion? Default: bond #0 is owner; only
   its sealed `add-key` is honored.
