# ozkey-49 — AK1037 port plan: `ozcore` (portable C) + platform layers

> Status: **PROPOSAL, 2026-10-18, firmware (team lead).** For operator review. Nothing is built yet.
> Consumers: firmware, ozkit (test bench), server (contract impacts in §6).
> Inputs: AnyMicro1037 SDK V1.02 audit (vendor files in `~/Documents/Dev/vendor/ak1037/`, kept
> outside this repo, Anyka confidential); operator decisions of 2026-10-18 (no TuyaOS, no Tuya
> protocol, OZKIE v2 on open IETF parts, ESP32-C6 bridge stays, more device types to come).

## 0. Decision asked of the operator

> **DECIDED 2026-10-18 (operator): go straight to OZKIE v2, AES-128-CCM suite, "we go for speed".**
> No AES-256 on AK1037; X25519/Ed25519 run in software until Anyka exposes the PKE curve API.

**Port OZKIE v1 as-is, or go straight to OZKIE v2 on AK1037?**

The recommendation is to go **straight to v2**. Reasons:

1. AK1037 is a new product with no deployed locks, so no v1 compatibility is owed on the lock side.
2. v1 uses AES-256-GCM, and AK1037 hardware has neither AES-256 nor GCM (SDK audit). Porting v1 means
   software AES-256-GCM now, then rewriting it for v2 later. That's the same work twice.
3. ozkit's new test bench should test the protocol we will ship, not one we're retiring.

Cost: the BANOI app (ftpos) and `bridgeBLE` need v2 support before an AK1037 lock can be used
end to end. That goes through an XF doc to ftpos once ozkey-48 (OZKIE v2 spec) is approved.
The ESP32 sims (`dlch584m_19/147`) keep v1 until then and are unaffected.

## 1. Why not port `ozdoorlock_core.h` line by line

`common/ozdoorlock_core.h` is ~10k lines of Arduino/ESP-IDF C++. Most of it doesn't apply to
a BLE-only, Tuya-free AK1037 lock:

| Section (by header comment) | AK1037? |
|---|---|
| Tuya MCU wire, DP map / `ozprofile`, "ask the MCU which DPs", wait-for-MCU | ❌ dropped (no Tuya) |
| Thread/OpenThread, UDP relay, dataset parsing | ❌ lock is BLE-only |
| Wi-Fi + MQTT wire, unicast uplink | ❌ the bridge does this |
| LCD screens, Arduino_GFX | ❌ different hardware (SDK has its own `lcd_disp`, if fitted) |
| BLE GATT server, provisioning, status ladder | ✅ rewrite on the Anyka GATT API |
| Bond #0 + member enrolment (M2/M3), control (M4), membership expiry (T5) | ✅ **core logic, port** |
| OZKIE semantic dispatch (F8), seq/replay + loss handling (ozkey-29) | ✅ **core logic, port** (v2 framing) |
| Credential store (ozkey-47, type-generalised, versioned) | ✅ **core logic, port** |
| Transaction log, time service (oztime) | ✅ port |
| Touch factory-reset ceremony, keypad presence (2.18) | ✅ re-do on SDK `touch_key` |

The keepers are deeply mixed with Arduino types (`String`, `Preferences`, `ArduinoJson`,
`BLECharacteristic`, mbedtls). The cheaper path is to **extract**, not translate.

## 2. Architecture

```
            ┌───────────────────────────────────────────────┐
            │ ozcore  (portable C11, no OS, no malloc in hot │
            │ path, no vendor headers)                      │
            │  ozkie/   handshake, envelope, seq/replay     │
            │  bond/    bond #0, members, expiry            │
            │  cred/    PIN / RFID / FINGERPRINT store (47) │
            │  lock/    unlock policy, tiers, tx log        │
            │  codec/   CBOR encode/decode                  │
            └──────────────┬────────────────────────────────┘
                           │  oz_port.h  (~25 functions)
     ┌─────────────────────┼─────────────────────┬────────────────────┐
     │ port/host           │ port/ak1037         │ port/esp32         │
     │ macOS/Linux, unit   │ Anyka SDK: GATT,    │ ESP-IDF/Arduino:   │
     │ tests + fuzz, CI    │ PKE/SKE/hash/TRNG,  │ future mains       │
     │ (runs TODAY)        │ littlefs, touch,    │ devices + bridge   │
     │                     │ RFID, fingerprint,  │                    │
     │                     │ motor PWM, RTC      │                    │
     └─────────────────────┴─────────────────────┴────────────────────┘
```

`oz_port.h` covers:
- **Crypto:** `x25519`, `ed25519_sign/verify`, `aead_seal/open`, `sha256`, `hkdf`, `rng`.
- **Storage:** `kv_get/set/erase`, with atomic write.
- **Time:** monotonic ms, and UTC set/get.
- **BLE:** advertising data, notify, on-write callback.
- **Hardware:** `actuator_unlock/lock`, keypad / RFID / fingerprint events, `log`.

One core then serves the lock, the coming sensors, curtains, alarm and garage relay (operator's
roadmap), and the bridge's OZKIE end. A new chip only needs a new `port/` directory.

## 3. Crypto mapping on AK1037 (from SDK V1.02 headers)

| Need | AK1037 hardware | Plan |
|---|---|---|
| SHA-256 / HKDF | ✅ `hal_hash` SHA-256/384/512 | HW hash; HKDF in core |
| RNG | ✅ `hal_trng` | HW |
| X25519 | 🟡 PKE microcode exists (`C25519_PMUL`), **no HAL API** | Asked Anyka 2026-10-18. Until then software (Monocypher 4.0.2, vendored) |
| Ed25519 | 🟡 microcode, no API | **Not needed**: ozkey-48 uses EDHOC method 3 (static DH), with no signatures |
| AEAD | 🟡 AES-128 + SM4, ECB/CBC/CFB/OFB/CTR; **no AES-256, no GCM/CCM** | v2 suite = **AES-128-CCM** (EDHOC suite 0): HW AES block + CCM mode in core. Asked Anyka whether HW GCM/CCM exists |
| Secure boot | ✅ RSA2048/4096 or SM2 signing, our keys, eFuse hash | Our signing keys, kept offline. Procedure doc before first production run |

## 4. Phases

| # | What | Needs hardware? | Exit test |
|---|---|---|---|
| P0 | `ozcore` skeleton + `port/host`, CMake, unit tests; Monocypher vendored | No | `ctest` green on macOS. 🟢 **DONE 2026-10-18**: `blelock/ozcore/`, 35 RFC/FIPS known-answer checks + 20k-case AES-CCM differential vs OpenSSL, all pass |
| P1 | Move bond / member / expiry / cred-store (ozkey-47) logic into `ozcore`, behaviour-matched to `ozdoorlock_core.h` | No | The same test vectors pass that `ozdoorlock_core` passes today |
| P2 | OZKIE v2 in `ozcore` (ozkey-48 approved 2026-10-18): EDHOC + OSCORE + CBOR, keypad pairing code. Deliverables for ftpos (XF-387 §5): host test lock over a socket, byte-exact vectors, and, if FFI is chosen, a reproducible iOS xcframework + Android per-ABI `.so` | No | Interop test: host lock ⇄ host "app" (Python); mobile libraries build bit-identical twice |
| P3 | Toolchain: build `ozcore` with the Anyka RISC-V GCC. **The tools are Windows-only.** The operator has a Windows machine firmware can use (2026-10-18; access method TBD). Linux/macOS tools are also asked of Anyka | No (compile only) | `.bin` links; size report against 2/4 MiB flash, 218 KiB SRAM |
| P4 | `port/ak1037` on the dev board: GATT service, littlefs KV, HW crypto, TRNG, RTC | **Yes** (dev board) | ozkit suite: pair, unlock, member add/revoke over BLE |
| P5 | Peripherals: touch keypad PIN, RFID 13.56 MHz, fingerprint enrol/match, motor H-bridge | **Yes** (+ peripherals) | ozkit suite on real parts |
| P6 | Power: sleep / advertising / connection current against the datasheet (4 / 31 / 40 µA); secure boot; OTA via the bridge | **Yes** | Measured µA table; signed-OTA round trip |

P0–P3 can start now. P4 onwards waits for the AK1037 kits.

### P2 mobile build targets (ftpos5, 2026-10-18; apply only if XF-387 OZV2-1 goes FFI)

- **iOS:** minimum 15.0; xcframework with an `ios-arm64` slice and an `ios-arm64_x86_64` simulator slice.
- **Android:** minSdk 24, target/compile 36, NDK **28.2.13676358**; arm64-v8a, armeabi-v7a, x86_64.
  Link with `-Wl,-z,max-page-size=16384`, and the exit test checks the 16 KB alignment (`llvm-readelf -l`).
- **Binaries:** stripped release builds, with debug symbols kept separately (dSYM, unstripped `.so`).
- **Symbols:** only `ozcore_*` symbols are exported, and that is the FFI API layer.
  - Everything else is built `-fvisibility=hidden`. That covers the internal `oz_*` symbols and
    Monocypher's `crypto_*`, which would otherwise **collide with libsodium-style `crypto_*`
    symbols** in other app libraries.
  - The exit test checks the exported symbol list with `nm -gU` / `llvm-nm -D`.
- **Reproducible:** pinned toolchains; two builds of one commit must be bit-identical.

## 5. Not doing

- No TuyaOS, and nothing from the L3T reference package's `TuyaOS/` tree. L3T `app_L3` is read only
  for peripheral/motor know-how (it is Anyka-licensed and Tuya-based).
- No vendor files in this repo. `port/ak1037` builds against an SDK path given at build time
  (`AK1037_SDK=~/Documents/Dev/vendor/ak1037/sdk-1.02/AnyMicro1037_SDK_V1.02`).
- No rename or retire of the CH584M-era targets (operator hold still stands).

## 6. Impacts on other teams (heads-up, no action yet)

- **ftpos (BANOI app):** OZKIE v2 client. XF doc once ozkey-48 is approved.
- **bridgeBLE:** v2 relay. The bridge mostly forwards sealed envelopes, so the change should be
  small; confirm during P2.
- **server:** as flagged by server on 2026-10-18, ozlodge/ozpms build Tuya DP frames for credentials.
  v2 replaces them with OZKIE-native credential messages. Spec goes in ozkey-48.
- **ozkit:** writes its test suite against `port/host` from P1 onwards, so no hardware is needed to start.

## 7. Open questions to Anyka (sent by operator 2026-10-18)

X25519/Ed25519 HAL API · AES-256 / GCM / CCM in hardware · Linux/macOS toolchain + CLI burn tool ·
re-send the damaged `libsdfilter.a` and `touchtool/resources.pak` · Bluetooth SIG QDID ·
LE Secure Connections + central role / GATT client example · English PKE/SKE register manual.

**Follow-up (sent by operator 2026-10-18):**
- Withdrawn: Linux/macOS toolchain (`app_mcu` builds from the CLI on the Windows host). Still asked:
  a **Windows command-line** burn tool for the production line and test automation.
- Withdrawn: `libsdfilter.a` (it was fine; it links). Still asked: re-send `resources.pak`.
- New: official RISC-V GCC version (we build with SysGCC; builds must be reproducible) · 2 vs 4 MiB
  flash on supplied parts, plus a dual-bank OTA partition layout (the demo `.bin` is already ~700 KB) ·
  whether secure-boot signing and eFuse programming can run from the CLI (signing keys stay offline) ·
  Windows 11 support · license terms for the SDK, the BLE host source and the closed `libbt.a`, and
  whether we may modify the host stack · how SDK updates and errata are announced · dev-kit ETA.
