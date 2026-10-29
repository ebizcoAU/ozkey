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
| SHA-256 / HKDF | 🟡 `hal_hash` lists SHA-256/384/512, **but the shipped `hal_hash.h` enables only SM3** (`SUPPORT_HASH_SHA256` is not defined) | **Software for now** (correction 2026-10-18, P3). Bench-test the engine's SHA-256 at P4, then switch. HKDF in core |
| RNG | ✅ `hal_trng` (true-random mode, health test) | HW, one word per ready flag, conditioned through SHA-256, with a stuck-output check. The SDK's own burst read takes several words per ready flag (see §7, next email) |
| X25519 | 🟡 PKE microcode exists (`C25519_PMUL`), **no HAL API** | Asked Anyka 2026-10-18. Until then software (Monocypher 4.0.2, vendored) |
| Ed25519 | 🟡 microcode, no API | **Not needed**: ozkey-48 uses EDHOC method 3 (static DH), with no signatures |
| AEAD | 🟡 AES-128 + SM4, ECB/CBC/CFB/OFB/CTR; **no AES-256, no GCM/CCM** | v2 suite = **AES-128-CCM** (EDHOC suite 0): HW AES block + CCM mode in core. Asked Anyka whether HW GCM/CCM exists |
| Secure boot | ✅ RSA2048/4096 or SM2 signing, our keys, eFuse hash | Our signing keys, kept offline. Procedure doc before first production run |

## 4. Phases

| # | What | Needs hardware? | Exit test |
|---|---|---|---|
| P0 | `ozcore` skeleton + `port/host`, CMake, unit tests; Monocypher vendored | No | `ctest` green on macOS. 🟢 **DONE 2026-10-18**: `blelock/ozcore/`, 35 RFC/FIPS known-answer checks + 20k-case AES-CCM differential vs OpenSSL, all pass |
| P1 | Move bond / member / expiry / cred-store (ozkey-47) logic into `ozcore`, behaviour-matched to `ozdoorlock_core.h` | No | The same test vectors pass that `ozdoorlock_core` passes today |
| P2 | OZKIE v2 in `ozcore` (ozkey-48 approved 2026-10-18): EDHOC + OSCORE + CBOR, keypad pairing code. Deliverables for ftpos (XF-387 §5): host test lock over a socket, byte-exact vectors, and, if FFI is chosen, a reproducible iOS xcframework + Android per-ABI `.so` | No | Interop test: host lock ⇄ host "app" (Python); mobile libraries build bit-identical twice 🟡 **MOSTLY DONE 2026-10-19:** CBOR, EDHOC (method 3), CoAP, OSCORE, invites and the lock engine (`oz_lock`: pairing code, roles, invites v1/v2/v3, creds with epoch, Echo on relay, persisted relay contexts across reboot) are in `ozcore`.
- **RFC vectors:** RFC 9529 §3 and RFC 8613 App. C 1–8 pass byte for byte, as do ftpos's frozen invite MACs.
- **Host test lock** `ozhostlock` (TCP 7001 BLE / 7002 relay) and the **independent Python app** `tools/ozapp.py` (written from the RFCs) pass a 20-step interop scenario.
- **8/8 ctest suites** pass, and the key rules were mutation-checked.
- **Still to do:** `events` Observe, `lock/settings`, custodial mode (ozkey-51), mobile FFI libraries, AK1037 rebuild with the new sources |
| P3 | Toolchain: build `ozcore` with the Anyka RISC-V GCC. **The tools are Windows-only.** The operator has a Windows machine firmware can use (2026-10-18; access method TBD). Linux/macOS tools are also asked of Anyka | No (compile only) | `.bin` links; size report against 2/4 MiB flash, 218 KiB SRAM. 🟢 **DONE 2026-10-18** (details below) |
| P4 | `port/ak1037` on the dev board: GATT service, littlefs KV, HW crypto, TRNG, RTC | **Yes** (dev board) | ozkit suite: pair, unlock, member add/revoke over BLE |
| P5 | Peripherals: touch keypad PIN, RFID 13.56 MHz, fingerprint enrol/match, motor H-bridge | **Yes** (+ peripherals) | ozkit suite on real parts |
| P6 | Power: sleep / advertising / connection current against the datasheet (4 / 31 / 40 µA); secure boot; OTA via the bridge | **Yes** | Measured µA table; signed-OTA round trip |

P0–P3 can start now. P4 onwards waits for the AK1037 kits.

### Commit gate (firmware; PM nexus-a8 ruling 2026-10-29)

Firmware commits on its own branch; only the PM merges to main. Commit only when the gate passes:

- `blelock/ozcore/tools/gate.sh <label>` exits 0. It runs ctest from clean build directories, three
  ways:
  - **default build** (no build type), so `assert()` and littlefs's `LFS_ASSERT` are live
  - **Release** (NDEBUG), the optimised code
  - **ASan + UBSan**, where any sanitizer report fails the run
- The ruling requires the sanitizer pass for any change under `port/`, `common/` or `src/`. The
  script runs it every time, which costs about a minute.
- The log is written to `nexus-lab-backups/ozkey_notes/ctest/<date>_<label>.log`. Cite it in the
  report to the PM.
- Why: until 2026-10-29 the gate was Release-only. That compiled out the littlefs assert that, in a
  default build, exposed the kv bug fixed in 4389aa5 (P4-prep item 2 below).
- `blelock/common/` (the Arduino-era sketches) has no host build, so the gate doesn't cover it yet.

### P3 result (2026-10-18, Windows build host, SysGCC RISC-V GCC 10.1.0)

- **`port/ak1037/oz_port_ak1037.c`** (new):
  - AES-128 on the SKE engine, ECB, with a FIPS-197 self-test on first use that falls back to
    software. Interrupts are off around each use, since the driver isn't re-entrant. The key
    registers are overwritten afterwards, because `hal_ske_final()` leaves the key in them.
  - TRNG as above; software SHA-256; Monocypher X25519.
  - `get_ms_tick_func()` extended from 32 to 64 bits.
  - kv returns `OZ_E_HW` until P4. Plan: the SDK's **littlefs** (power-loss safe, so the atomic-set
    rule holds).
- **Build:** `make -f port/ak1037/Makefile`. The core gets the same `-Wpedantic -Wconversion -Werror`
  as the host build.
  - GCC 10 found two real 32-bit issues the Mac's clang didn't: MixColumns integer promotion, and
    a CCM AAD-length test that's always true when `size_t` is 32-bit. Both are fixed.
  - Host tests still pass after the fixes.
- **Link:** the SDK `app_mcu` objects plus `libozcore.a`, with the v2 primitives forced in as GC roots.
  This is a separate `app_mcu_oz.elf`, so the SDK image is untouched.

  | | text | data | bss |
  |---|---|---|---|
  | SDK `app_mcu` | 708,496 | 2,024 | 16,132 |
  | + ozcore | 721,428 | 2,024 | 16,156 |
  | **Δ** | **+12,932** | 0 | **+24** |

  That is ~0.7 MB of 2 MiB flash. EDHOC/OSCORE/CBOR (P2) will add to it, estimated 15–25 KB more.
- **No symbol collisions** with the SDK libraries.
- **Stack:** the deepest frame is ~1 KB (HKDF → HMAC → SHA-256). Give the OZKIE thread **≥ 4 KB** once
  EDHOC is in.
- **Not run:** no board yet. The self-tests and KATs run on the device at P4.

### P3b result (2026-10-29, VictorPC, SysGCC RISC-V GCC 10.1.0): the full P2 stack

The 10-18 build was crypto only. This one is everything P2 added: CBOR, EDHOC, CoAP, OSCORE,
invites and the lock engine.

- **Build host:** VictorPC (Windows 11). It's the permanent station, so Mac2 Boot Camp is no
  longer needed. All 11 objects build at the same `-Werror` level as before, with no new warnings.
- **Reproducible:** the SDK stamps `__DATE__`/`__TIME__` into three files. With
  `SOURCE_DATE_EPOCH` pinned, two clean `app_mcu` builds give a byte-identical `.bin`.
- **Library** (`libozcore.a`): text 81,931, bss 1,252. Monocypher is 48.6 KB of that before
  dead-code removal (`--gc-sections`).
- **Link** (`port/ak1037/link_app_mcu.ps1`): builds `app_mcu_oz.elf`. The SDK's own image is
  untouched.

  | | text | data | bss |
  |---|---|---|---|
  | SDK `app_mcu` | 708,496 | 2,024 | 16,132 |
  | + ozcore, realistic (`-RootPattern '^oz_lock_'`: the API the app calls) | 745,132 | 2,024 | 17,384 |
  | + ozcore, upper bound (every global, all of Monocypher) | 787,036 | 2,024 | 17,384 |
  | **Δ realistic** | **+36,636** | 0 | **+1,252** |

  The P3 estimate was 13 + 15–25 KB, and this is in line with it.
  - On a 2 MiB part: two ~750 KB OTA banks plus the bootloader and a littlefs partition leave
    under 0.5 MiB of headroom. It fits, but it's tight, which is why the 2 vs 4 MiB question to
    Anyka matters.
  - On 4 MiB there's no concern.
- **RAM:** `sizeof(oz_lock)` is **5,864 B** on rv32, allocated by the app. Plus 1,252 B bss and
  the thread stack below, ozcore needs about 15 KB of the 218 KiB SRAM.
- **Stack** (`-fcallgraph-info=su`, `tools/stack_report.py`): worst-case chain per entry point,
  not counting indirect calls.

  | entry | bytes | deepest chain |
  |---|---|---|
  | `oz_lock_rx` | **5,328** | rx_oscore (2,176) → admin `dev/reset` re-runs oz_lock_init (1,936) → X25519 |
  | `oz_lock_init` | 2,768 | init (1,936) → X25519 |
  | `oz_lock_pair_code` / `_tick` | ~1,560 | pair_finish → OSCORE protect → CCM → AES → log (224) |
  | `oz_lock_keypad_pin` | 1,184 | save_creds (1,056) → CBOR |

  - Not included: indirect calls (the app hooks, the EDHOC `dh`/`resolve_peer` callbacks,
    OSCORE `next_ssn` persist → kv), plus libc and the SDK.
  - CBOR `skip_item` is the only recursion. It's capped at `OZ_CBOR_MAX_DEPTH` 8 × 48 B, so
    hostile CBOR can't overflow the stack.
  - **Give the OZKIE thread an 8 KB stack**, up from the ≥ 4 KB said at P3. Check it on silicon
    at P4 with RT-Thread's stack high-water mark.
  - Follow-up worth doing: `rx_oscore` (2,176 B) and `oz_lock_init` (1,936 B) have large local
    frames. Moving their buffers into `oz_lock` would cut the worst case by about 3 KB.
- **Not run:** there's still no board.

### P4-prep item 2 (2026-10-29): kv on littlefs, power-cut tested on the host

- **Code:**
  - `port/common/oz_kv_lfs.c` holds the kv logic. Each key is one file, `oz.<key>`.
  - A set writes `oz.<key>.t` and then `lfs_rename`s it over the old file. littlefs commits a
    rename atomically, which satisfies the `oz_port.h` rule (old value or new, never a mix).
    Leftover `.t` files are swept at attach.
  - The same file is built by the host CMake (against upstream littlefs) and by the AK1037
    Makefile (against the SDK's `lfs.h`).
- **Why not the SDK's `ak_lfs` wrapper:**
  - It has no remove or rename.
  - It never formats; it expects a pre-built `lfs.bin` to be burned.
  - 🔴 It reads through the XIP window straight after programming, with
    `hal_dcache_flush_all()` commented out, so a read-back can return stale cached data.
  - Our block device (`port/ak1037/oz_port_ak1037.c`) reads with `spi_flash_read()` instead.
    It mounts its own **`OZKV`** partition and formats it on first boot. A mount that can't be
    repaired also ends in a format, which leaves the lock unpaired, the same as a factory reset.
- **The SDK's littlefs is upstream v2.4.1**, checked by diff:
  - Changes are cosmetic only: CRLF, blank lines, `\r\n` in trace output.
  - `lfs_util.h` adds `LFS_THREADSAFE` under RTOS, `LFS_NO_ASSERT`, and its own allocator.
  - The host test vendors upstream v2.4.1 (`third_party/littlefs`, BSD-3), so it tests the same
    filesystem code.
- **Host test `kv_lfs`** (ctest 9/9 now):
  - Geometry matches AK1037: RAM NOR, 256 B pages, 4 KiB sectors, 64 KiB.
  - Program only clears bits. A power cut tears the operation in flight (half a page, or half a
    sector erased), then everything fails until a "reboot" remounts the image.
  - The sweep cuts every flash operation of a replace, a create and an erase. Results: always
    old-or-new, an OK return always means committed, no temp files left, always mountable, other
    keys untouched.
  - A 3,000-operation soak with 192 random cuts stays consistent.
  - The full working set (16 bonds, 16 relay contexts, creds, keys) fits in 64 KiB and survives
    3,200 rewrites.
  - littlefs never asked the flash for a 0→1 program.
  - **Mutation check:** writing the final file directly, without temp+rename, fails the test.
    A cut while creating a key leaves an empty file (8 cut points), and the soak goes
    inconsistent. Replacing an existing key is atomic even without the rename, because littlefs
    file writes are copy-on-write; the rename is what protects *new* keys.
  - 🔴 **Bug fixed 2026-10-29 (item 4b work):** `oz_kv_lfs.c` passed littlefs a
    `struct lfs_file_config` that lived only for the open call. littlefs keeps that pointer
    (`file->cfg`) and commits `cfg->attr_count` user attributes from `cfg->attrs` on every
    sync/close, so each close read a dead stack frame and could write garbage attributes into
    the metadata. It would have happened on silicon too, unchecked (the SDK's `lfs_util.h` sets
    `LFS_NO_ASSERT`). Found because the soak's cut count depended on the optimization level
    (Debug 200 vs Release 192); ASan at -O2 then named it as stack-use-after-scope. The config
    now lives in a `kv_file` alongside the file and its cache. The count is 192 under every
    -O level and stack-init pattern, and the whole ctest is clean under ASan+UBSan.
  - The same review found that the test kept running littlefs after a simulated cut, which
    silicon can't do (the CPU stops), and littlefs's own assert fired on it. Release gates hid
    this because NDEBUG compiled the assert out. A cut now longjmps back to the test, and the
    host littlefs build keeps its asserts on in every build type.
  - 🟢 **Item 2 re-verified after the fix (4389aa5):** every result above still holds (sweep
    old-or-new / OK-means-done / no temp / mountable, 192-cut soak, 79 checks). It passes the full
    gate (default, Release, ASan+UBSan). **Mutation check of the gate:** with the pre-fix
    `oz_kv_lfs.c` put back, default and Release still pass and only the ASan leg fails
    (stack-use-after-scope in `lfs_file_rawsync`). That's why the sanitizer pass is part of the gate.
  - 🔴 **Remount after any I/O error (PM ruling 2026-10-29, item 4b work):** littlefs 2.4.1 keeps a
    stale program cache after a failed prog. `oz_kv_lfs` now unmounts and remounts from flash
    after any `OZ_E_HW`, and retries the mount before the next call if that fails. The new host
    case `ioerr` fails each prog of a replace in turn with the power still on, and checks
    old-or-new, no temp left, other keys intact, and usable with no reboot (5 failing progs, all
    pass). **Mutation check:** without the remount, the asserts-on build aborts on the stale cache,
    and with asserts off (as on silicon) 4 of the 5 break old-or-new. That is real corruption from
    one transient flash error.
  - A second 2.4.1 quirk surfaced along the way: creating a file reports **any** failed commit as
    `LFS_ERR_NAMETOOLONG` (`lfs_file_rawopencfg`). `oz_kv_lfs` had mapped that to `OZ_E_ARG`, so a
    flash error looked like a bad key and skipped recovery. Our names are ≤ 21 chars against
    littlefs's 255, so it now maps to `OZ_E_HW`.
- **AK1037 build:** `-Werror` clean.
  - Link: text 749,100 (**+3,968** over P3b, kv plus the SDK's `lfs.c`, which `app_mcu`
    previously garbage-collected); bss +252.
- **Stack with littlefs on the path** (SDK `lfs.c` built with `-fcallgraph-info`):
  - Single-pass worst case is `oz_lock_rx` at **6,128 B** (dev/reset → init → kv_set → rename →
    dir_commit). Each kv call costs about 1.5–1.6 KB.
  - littlefs 2.4.1 also recurses (`dir_commit`↔`dir_compact`/`dir_drop`, `dir_traverse`).
    Upstream removed recursion in 2.5, but the SDK ships 2.4.1.
  - **Raise the OZKIE thread to 12 KB until it's measured on silicon.** SRAM is not the limit
    (≈19 KB of 218 KiB in total).
- **Needs the burn config (item 5):** an `OZKV` partition, sector-aligned, ≥ 16 KiB; 64 KiB
  recommended.
- **Default 2 MiB layout** (`tools/burntool/config_app_mcu.txt`, KB):

  | OTA | PROG | LP | ROMFS | UDATA | FPDAT | total |
  |---|---|---|---|---|---|---|
  | 212 | 780 | 44 | 210 | 100 | 628 | 1,974 / 2,048 |

  - Our realistic image (751,124 B `.bin` = text + data, 733.5 KiB) fits PROG (780 KiB) with
    ~46 KiB spare. Stripping the SDK samples we never call will free more.
  - There's no second app bank: OTA goes through the separate 212 KB `app_ota` updater.
  - `UDATA` is the SDK's own littlefs, which we don't use, so 64 KiB of it can become `OZKV`.
  - Layout options go to the PM if Anyka confirms 2 MiB.
- **Not run on silicon:**
  - the real flash
  - erase/program while executing XIP from the same chip
  - read alignment
  - power-cut behaviour of the real NOR part (P25Q16SH in the config's flash list)

### P4-prep item 3 (2026-10-29): BLE GATT transport

- **Portable, host-tested:**
  - `src/oz_frame.c`: the §2.1 framer. It reassembles fragments and fragments replies at the ATT MTU.
    - Fragment 0 always starts a new message, so a central that gave up mid-message recovers.
    - Out-of-order fragments, a type change mid-message, an empty non-final fragment, fragment 31
      marked "more", and anything over 1,024 bytes are all refused, and the partial message is
      dropped.
  - `src/oz_ble.c`: one connection. Frame in → reassemble → `oz_lock_rx` → reply fragmented →
    the port's tx, one notification per frame.
    - Pairing-code and tick replies take the same path.
    - Malformed frames get no reply, so a stranger learns nothing.
  - GATT UUIDs as LE byte strings (`OZ_BLE_UUID_*` in `oz_ble.h`), following the v1 pattern
    (second group = characteristic): info `…-0002-…`, ozkie `…-0003-…`.
- **Tests (ctest 10/10):**
  - `frame`:
    - every message length 0–1,024 at MTU 23/64/185/247/517 (4,709 round trips)
    - all the refusal cases above
    - 200k fuzzed frames (bounded, clean state after an error)
    - the UUID bytes checked against the spec strings
  - `lock` now runs its **whole suite three times**: engine direct, then every BLE-link message
    through `oz_ble` at **MTU 23** (235 frames in, 131 out) and at **MTU 247**. Results are
    identical (208 checks). The relay link stays unframed, as on MQTT.
  - Mutation check: drop one byte of a reassembled message and the framed runs fail (3 + 3),
    while the direct run doesn't.
  - At MTU 23 the most one message can be is 608 bytes (32 × 19). Everything in the current
    suite fits. Credential-by-value at ~480 B fits too, but only just, so apps should still
    negotiate 247 as §2.1 says. A reply that doesn't fit is logged and dropped.
- **AK1037 glue `port/ak1037/oz_gatt_ak1037.c`** (compile + link only):
  - The service table follows the SDK's Nordic DFU sample: primary service + `info` (read) +
    `ozkie` (write + notify + CCCD), 128-bit UUIDs.
  - BLE callbacks only copy into an RT-Thread message queue (8 deep). The **OZKIE thread**
    (12 KB stack) owns the `oz_lock` and runs everything.
  - `info` reads are served from a snapshot the thread refreshes, so the BLE context never
    touches the lock.
  - Connect requests MTU 247. `eBT_MSG_LE_MTU_EXCHANGED` (MTU in `param1`, seen in the SDK
    source) sets the fragment size.
  - Notify retries while the stack's send ring is full.
  - The keypad posts the pairing code with `oz_gatt_ak1037_pair_code()`.
  - Build: `-Werror` clean, +1.4 KB text, 23.5 KB bss (12 KB stack, ~4.3 KB queue, 2 KB
    session buffers, link state).
  - Link with `oz_gatt_ak1037_start` rooted: text 751,604 / bss 41,092, with **no unresolved SDK
    symbols**.
- **Open, needs the kit or Anyka:**
  - the notify `att_rec_idx` meaning (Anyka question 5; the code uses 4, the ozkie value record)
  - real-phone MTU/PHY behaviour
  - notify back-pressure
  - ~~advertising per CONTRACT.md: not written yet~~ Done in item 4a (ef2844d, `oz_adv`).
    Default `OZ_GATT_ADV_ALWAYS=1` confirmed by PM nexus-a8 2026-10-29 (AK1037-ADV); CONTRACT.md
    amended to match, touch window is the compile-time alternative. Still open for the kit:
    - whether `ADV_SCAN_IND` works while a link is up in the single-link build
      (`SUPPORT_MULTI_BLE` off; v1 had two links, bridge + app)
    - measured current draw of always-advertise (feeds P6 and the battery estimate)
  - Nothing calls `oz_gatt_ak1037_start()` yet. That's item 4's self-test app.

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

**For the next email (found in P3, 2026-10-18), alongside ozkey-52 §6:**
1. **SHA-256 in the hash engine:** `hal_hash.h` defines only `SUPPORT_HASH_SM3`. Is SHA-256 supported
   in hardware on AK1037, and is it safe to define `SUPPORT_HASH_SHA256`?
2. **TRNG FIFO:** `hal_trng_get_randbuff_bytes()` reads several words from `TRNG_DR` after a single
   `DRDY`. What is the FIFO depth? Is each read of `TRNG_DR` fresh entropy? Also,
   `hal_trng_get_rand()` returns 0 on timeout with no error, and `hal_trng_init()` writes
   `TRNG_CR |= 17` (probably meant `1 << 17`). Please confirm.
3. **SKE key registers:** after `hal_ske_final()` the key stays in the SKE key registers. Is that
   register readable over the bus or a debug port?
4. **`ak_lfs` (found 2026-10-29, P4-prep item 2):**
   - `user_provided_block_device_read()` reads the littlefs partition through the XIP window
     straight after `spi_flash_write()`/`spi_flash_sector_erase()`, with `hal_dcache_flush_all()`
     commented out. Can that return stale cached data?
   - The wrapper also has no remove/rename/format. Is it maintained, or should customers use
     littlefs directly, as we do (reading via `spi_flash_read()`)?
   - Is `spi_flash_write`/`sector_erase` safe while code is executing XIP from the same chip?
5. **GATT (found 2026-10-29, item 3):**
   - In `com_ble_server_notify_ext(…, att_rec_idx)`, what does the index count? The Nordic DFU
     sample passes `0` with `//IDX_DFU_CTRL_VAL` commented out.
   - (The MTU question is answered by the SDK itself: `eBT_MSG_LE_MTU_EXCHANGED`, MTU in `param1`.)
