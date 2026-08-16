# ozkey-35 — How the system actually works

> **Orienting document. Written 2026-08-16 at the operator's request, after a PM
> handover lost the thread.** Consumers: **everyone** — PM, architect, firmware,
> server, NEXUS, ftpos.
>
> This describes **what is built and running**, not what any specification says
> should be. Where the two differ, that is called out. Every claim here is
> either backed by a file:line, a bench capture, or is explicitly marked as
> unverified.
>
> **If you are writing a directive, start here.** Three recent directives
> assigned work to a team that doesn't exist, assumed an identifier that means
> something else, and marked six claims verified for code that was never
> written. None of that was carelessness — the system genuinely has two things
> called "the server" and two things called `app_id`. §2 exists to stop that.

---

## 1. The five components

| Component | What it is | Where it lives | Owner |
|---|---|---|---|
| **Lock** | ESP32-C6 N8. Identity, bonds, credentials, audit log — all on-device. Talks to a Tuya MCU over UART for the bolt/keypad/RFID. | `ozkey/blelock/doorlock/` (1.47"), `doorlock19/` (1.9"), shared logic in `blelock/common/ozdoorlock_core.h` | firmware |
| **Bridge** | ESP32-C6 N16. Thread border router + MQTT relay. Transparent — relays opaque envelopes. | `ozkey/blelock/bridge32/` | **firmware** (there is no separate bridge team) |
| **ozlockserv** | Residential relay. Content-blind: holds no keys, no credential values, no door-event log. | `ozkey/ozlockserv/` — port 4200, site `lab` | server instance |
| **NEXUS** | The app's backend. Public-key directory (`lock_registry`), factory ingest, app identity. **A different service from ozlockserv.** | `~/Documents/Dev/nexus/` — `api/src/routes/`, `mysql/` | NexusPM |
| **BANOI** | Flutter app. The owner's key container. | `~/Documents/Dev/ftpos/` (`lib/services/ozlock/`) | ftpos |

Also on the bench: **LockSim** (`ozkey/locksim/`, port 3100) — a browser MCU
emulator that sits on the Tuya UART. It is the hardware truth for the 55AA/DPID
codec. It cannot see any BLE ceremony, because it is on the wrong wire.

## 2. 🔴 Glossary — the words that have caused every recent misunderstanding

**"The server"** — ambiguous, and the single biggest source of confusion.
There are two, they are different codebases, and neither is a superset of the
other:

- **ozlockserv** relays MQTT/REST for locks. Content-blind by design. It has
  **no** public-key column and **no** `/pubkey` endpoint, and it never will —
  that is the Sovereign Edge guarantee.
- **NEXUS** is the app's backend and *does* hold public keys
  (`lock_registry`) and serves `GET /api/v1/locks/:mac/pubkey`
  (`nexus/api/src/routes/locks.js:25`).

Firmware got this wrong on 2026-08-16 — grepped `ozlockserv`, found no endpoint,
and told the PM the endpoint didn't exist. It exists, in NEXUS. **Always name
which server.**

**`app_id`** — also two things:

- In the **security rationale**: a per-install identity bound to a lock. **This
  does not exist.** NexusPM checked (`nexus-02.md` §N-6/§N-9).
- In **NEXUS today**: an alternate wire-name for `variant` (BANOI vs MAOI) at
  login. Not an identity, not a dev/prod flag.
- On the **lock**: a bond's 32-byte X25519 public key *is* its `app_id`
  (`g_bonds[].pub`).

**`mode`** — two unrelated axes, both spelled "mode":

- `cfgMode` = `ozkey-cloud` | `ozkey-local` — cloud relay vs on-prem server.
  **Not** residential vs commercial, though they correlate.
- `operational_mode` = `development` | `production` — where the identity key
  comes from (NVS vs eFuse). See `ozkey-34.md`.

**`device_id` / MAC** — the same 6 bytes in three dresses. USB serial number =
MAC = `ozk-<12hex>` as `device_id`. NEXUS's `lock_registry.mac_address` is the
bare lowercase 12-hex. LockA: MAC `AC:EB:E6:39:F8:C4` → `ozk-acebe639f8c4` →
`acebe639f8c4`.

**Bond #0** — the owner. Slots 1-15 are members. Role gating is `slot == 0` or
`role == OZ_ROLE_ADMIN`, checked per verb.

## 3. The three flows that actually run

All three are bench-verified. Evidence in §5.

### 3.1 At the door (BLE, offline, no server involved)

```
app  → read challenge (…0005)          Gate 3, fresh 16 bytes, per write
app  → sealed `control` (…0006)        AES-GCM under X25519(lock_priv, app_pub)
lock → verify bond, challenge, counter floor
lock → DP1 frame to the Tuya MCU over UART
lock → notifyStatus("UNLOCK_OK")
```

Works with no network of any kind. This is the path that makes the product
survive the vendor disappearing.

### 3.2 Remote (queued, never a live socket to the lock)

```
app → POST /locks/:id/... (ozlockserv)   → queued row
                                          → MQTT publish
bridge → receives, relays over Thread UDP → UNICAST to the lock's own address
lock  → opens envelope, counter-only freshness (no live challenge possible)
lock  → DP frame to MCU
```

**Updated 2026-08-16 — the downlink is now unicast** (`bridge32-1.39`, D2, §9.1).
It used to be multicast `ff03::1`, which a Sleepy End Device cannot hear: a
sleeping radio is not present for a link-layer broadcast. That was the blocker
on SED, and it is cleared — the bridge learns each lock's address from its own
uplinks and sends downlinks back to it, keeping multicast only as the fallback
for a lock it has not heard from yet.

🟡 **SED is unblocked but still off by default.** Removing the blocker is not the
same as flipping the switch — see §9.4.

### 3.3 Audit (the lock is the record)

```
lock  → txlogAppend()  writes LittleFS FIRST      (flash before radio)
lock  → seals event to bond #0, pushes on uplink
bridge/ozlockserv → relay ciphertext they cannot read
app   → query_events over BLE, sealed, cursor-based with dropped_before_seq
```

The server keeps no door-event log. The app holds the history. Verified
2026-08-15: 5 sealed uplinks / 0 unsealed, and the first successful
`query_events` round trip.

## 4. Identity — the part with the most spec drift

**Today, on every board:** the lock mints its own X25519 keypair on first boot
and stores it in NVS (`ozcrypto.h:218` `ozLockKeyInit`). `info.pub` on the INFO
characteristic (`ozdoorlock_core.h:5858`) returns the public half.

**`doorlock-1.79` adds** eFuse-first key loading, so `info.pub` returns the
eFuse-derived key on a burned unit and the NVS key otherwise (`ozkey-34.md`).
**1.79 is now on LockA and the spare 1.9" (2026-08-16), but no eFuse has ever
been burned anywhere**, so the production half of that path has still never
executed — every board falls through to NVS.

**Three things that follow, and that keep being missed:**

1. **There is no hardware ECDH on the ESP32-C6.** No
   `esp_crypto_ecdh_shared_secret`, no ECDH peripheral, and
   `ESP_EFUSE_KEY_PURPOSE_USER` is documented in the header as *"User purposes
   (software-only use)"*. The C6 purpose enum has no `ECDSA_KEY` either. We use
   software X25519 (mbedtls `CURVE25519`), key read from eFuse into RAM and
   zeroised after derivation — "Option (b)".
2. **So eFuse buys immutability, uniqueness and survival across factory reset —
   not extraction resistance.** `--no-read-protect` is required for Option (b),
   which means software can read the key. Do not claim "unextractable in
   silicon".
3. 🔴 **Every bond secret is `X25519(lock_private, member_public)`**
   (`ozcrypto.h:513`). Change the lock's key and **every pairing dies silently**
   — the bond table survives, the lock still reports `bonds=1`, and every sealed
   envelope fails. **eFuse must be burned before a unit is ever paired.**

**What actually stops a MAC spoof:** the ECDH handshake. An attacker with the
MAC and the public key still cannot derive the shared secret. It is *not* a
NEXUS-side pairing check — `ozlock_locks` is explicitly "display only; real
authority lives in the lock's own bond list", and `/pubkey` is public/no-auth by
design. NexusPM (`nexus-02.md` §N-9) and firmware reached this independently.

## 5. Status — evidence or blank

Only claims with evidence are marked verified. A blank Evidence cell means
nobody has demonstrated it, regardless of what any other document says.

| # | Claim | State | Evidence |
|---|---|---|---|
| 1 | Sealed BLE unlock at the door | 🟢 verified | routine on bench since M4 |
| 2 | Remote queued unlock reaches the lock | 🟢 verified | `queue_id=435` → `ctr 46`, ~1 s, 2026-08-14 |
| 3 | Door events sealed, server cannot read | 🟢 verified | 5 sealed / 0 unsealed uplinks; broker shows `envelope_hex` only |
| 4 | `query_events` BLE pull, sealed | 🟢 verified | 2026-08-15 09:31, `[EVT] sealed 112 B -> 149 B envelope` |
| 5 | Rename, BLE and remote, with reconciliation | 🟢 verified | 2026-08-14, both directions |
| 6 | Clock provenance (`live`/`NVS-only`/`UNKNOWN`) | 🟢 verified | LockC `NVS-only` → `live` on retained topic |
| 7 | NEXUS `/locks/:mac/pubkey` | 🟢 verified | `nexus-02.md` §N-7; live 200 for LockA, `nexus-03.md` §5 |
| 8 | `lock_registry` with `operational_mode` | 🟢 verified | `migration_v128_lock_registry.sql`, applied |
| 9 | Factory bulk ingest | 🟢 built | `nexus-02.md` §N-8. 🔴 `FACTORY_API_KEY` still on `dev-factory-key` fallback |
| 10 | App reads `info.pub` and cross-checks NEXUS | 🟢 **verified 2026-08-16** | XF-106 §10: `nexus_pub == ble_pub == a99f0965e928…`, `match=true`. Also proves LockA's private key survived the 1.77→1.79 flash byte-identical (F-13) |
| 11 | MCU ack gate on credentials | 🟡 built, bypassed in practice | app uses the legacy path; ack mechanism itself proven sound (matcher would fire) |
| 12 | Thread SED + poll interval | 🟡 built, **default off**, never run | **unblocked 2026-08-16** — bridge unicast downlink verified (§9.1). Enabling still needs 1.80 flashed + a deliberate test |
| 13 | Battery life | 🔴 **not measured** | no power instrument exists on this bench. FTD ~35 mA/~3 days is a *datasheet estimate*; the µA figures have no measurement behind them. **See `ozkey-36.md`** — modelling the WHOLE lock (not just the comms module) puts a piggyback lock at ~1.1 years at 20 opens/day, and the DL MCU is 75% of that budget |
| 14 | Plaintext `log` gate (C7) | 🔴 not built | scoped only; `mode` mapping question still open |
| 15 | eFuse burn / production identity | 🔴 never burned | `ozLockKeyFromEfuse()` compiles, has never seen a burned block |
| 16 | `provision_key` (dev key delivery) | 🟡 built, needs a decision | sealing is circular — `ozkey-34.md` §4.1 |
| 17 | JTAG/UART disabled | 🔴 false | both live; USB CDC serial is how every bench log is read |
| 18 | `*01#` keypad command opens BLE (DP 104, our extension) | 🟢 **verified 2026-08-16** | `doorlock-1.83` on LockA: `[TUYA<-] KEYPAD COMMAND *01#` → `[KEYCMD]` → `[BLE] window OPEN`, all same second. Exactly ONE window in the capture, 0 reader drops. **LockSim only — no real DL MCU can send it** |
| 19 | Accidental BLE windows removed | 🟢 **fully verified 2026-08-16** | LCD any-key: 5 keys incl. 3×`#`, no window (1.81). 3-failed-entry gesture ran **twice** on 1.83: `1/3`, `2/3` open nothing, window on the 3rd, self-closes at 60s, counter resets after firing (4th entry logged `1/3`). A **successful** unlock (`ACCESS_RESULT SUCCESS`) opened no window — the privacy property holds |
| 20 | `info.pub` reports the CURRENT key after `provision_key` | 🟡 fixed in 1.83, unexercised | XF-106 §17 run 1 read the pre-provision key: `chrInfo` was a boot-time snapshot. Now rebuilt on change; needs a re-run to confirm |

## 6. Firmware versions in play

| Version | State |
|---|---|
| `doorlock-1.77` | **running on LockA** — the only lock on the mesh |
| `doorlock-1.78` | built, never flashed — SED, poll interval, 300 s heartbeat, `[MON] radio=` |
| `doorlock-1.79` | built, never flashed — operational modes, eFuse read, `provision_key` |
| `bridge32-1.38` | running on the bridge |

The Wi-Fi lock and the spare 1.9" board are on older builds. **Any claim citing
1.78 or 1.79 behaviour on hardware is citing something that has not run.**

## 7. Open decisions — each needs a person, not a doc

1. **`provision_key` sealing** — the key authenticating delivery is the key being
   replaced. Three options in `ozkey-34.md` §4.1; (b) implemented pending
   sign-off. → PM + ftpos
2. **Mismatch handling** when NEXUS and `info.pub` disagree. Recommendation:
   abort, prefer neither. → ftpos (XF-106 §4)
3. **Bridge unicast downlink** — the gate on SED ever shipping. → firmware
4. **Power instrument** — nothing about battery life is answerable without one.
   → operator
5. **Dev-provisioning gate** in NEXUS, since per-install `app_id` doesn't exist.
   → NexusPM + PM
6. **C7 `mode` mapping** — is `ozkey-cloud` the right proxy for "residential"?
   → PM

## 8. How to check a claim before repeating it

The recurring failure mode is a claim written in a spec, read back as fact, and
cited into the next document. Three cheap habits:

- **Name the service.** "The server" is ambiguous; `ozlockserv` and NEXUS are
  different codebases with opposite policies on holding keys.
- **Grep before asserting absence** — and grep the *right* repo. Firmware
  asserted an endpoint didn't exist after searching one of two servers.
- **A window too short to contain the event is not evidence of absence.** Two
  90-second captures against a 10-minute cadence once produced a confident,
  wrong conclusion that the server was broken.

And the one that produced §5: **"verified" means someone watched it happen.**
Compiled is not run. Flashed is not exercised. Specified is not built.

---

## 9. Decisions closed 2026-08-16 (PM)

| # | Decision | Outcome |
|---|---|---|
| D1 | `provision_key` sealing | **Option (b)** — seal under the lock's self-minted first-boot key, forced re-pair after |
| D2 | Bridge unicast downlink ownership | **firmware** — confirming §1's note that there is no separate bridge team |
| D3 | Spec corrections | server team updated from the actual findings |
| D4 | N-6 dev-provisioning gate | support-only bearer token, **not** a plain env var |
| D5 | Conflicting pubkey upload for a `production` row | **reject**, do not overwrite |
| D6 | `provision_key` sign-off | approved, with ftpos's two conditions (real gate; forced re-pair surfaced explicitly in the UI, never a silent reconnect) |
| D7 | XF-48 TRNG `lock_pub` | superseded |

### 9.1 🟢 D2 is built and verified — the SED blocker is gone

`bridge32-1.39`. The bridge learns each lock's IPv6 address from its own uplink
traffic and sends downlinks straight back to it, keeping multicast only as the
fallback for a lock it has not heard from yet (after a reboot, that is everyone,
for up to one beacon interval — and it says so in the log rather than failing
silently).

Verified on the bench, no multicast involved:

```
bridge  [UNICAST] ozk-acebe639f8c4 reachable at fd8c:af8b:c85b:9d72:41d0:7ccf:b332:c5fd
bridge  [UDP] >> [unicast] {"target":"ozk-acebe639f8c4",…,"via":"unicast"}
lock    [UDP] rx 119 bytes … "via":"unicast"
lock    [FWD] cmd -> MCU: unlock channel report
lock    [TUYA->] 55 AA 00 06 00 05 01 01 00 01 01 0E
```

**A sleeping lock would have received that.** §5 row 12 moves from "blocked" to
"unblocked, not yet enabled" — SED still defaults off, because removing a
blocker is not the same as flipping a switch, and the switch deserves its own
test.

### 9.2 `doorlock-1.80` — a gap found while testing D2

`cfgThreadSed` and `cfgThreadPollS` shipped in 1.79 as NVS keys **nothing could
write**. Same class of mistake as a diagnostic nobody can read: a setting you
cannot set is not a setting. Now:

```json
{"op":"thread_power","sed":true,"poll_s":5}
```

Poll retimes live. The link mode needs the stack re-read at boot, so `sed` is
persisted and applied on the next boot — and the log says so, rather than
leaving someone to infer it from a current measurement that did not move.

### 9.3 Version state after this round

| Version | State |
|---|---|
| `doorlock-1.79` | **running on LockA** — bond #0 survived the flash, broker confirms `fw` |
| `doorlock-1.79` | also on the spare 1.9" (flashed first as a canary — the eFuse read path had never run anywhere) |
| `doorlock-1.80` | built, **not flashed** — adds `thread_power` |
| `bridge32-1.39` | **running on the bridge**, unicast downlink verified |

Committed: `2f9297e` (1.79 + ozkey-34/35), `e2b3fe3` (bridge 1.39 + 1.80).

### 9.4 What D2 does NOT close

- **SED has still never run.** Enabling it needs 1.80 flashed, then
  `{"op":"thread_power","sed":true}`, then a reboot, then a downlink test to
  confirm commands still arrive while the radio sleeps.
- **Battery is still unmeasured.** D2 makes the saving *reachable*; it does not
  measure it. §5 row 13 stands — no power instrument exists on this bench.
