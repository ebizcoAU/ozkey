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
- `blelock/common/` (the Arduino-era sketches' headers) is **out of scope**. None of it links into
  the AK1037 images: the ozcore build includes only `ozcore/{include,port/common,third_party}` and SDK
  paths (checked 2026-10-29). `ozcore/port/common` is a different directory, and the gate covers it.

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

### P4-prep item 4b (2026-10-29): on-target self-test and lock bring-up images

- **Portable runner** `tests/selftest_all.c` (`oz_selftest.h`):
  - `oz_selftest_suites()` runs the 8 suites: crypto, cbor, edhoc, oscore, invite, frame, adv, lock.
  - Each `tests/test_<name>.c` is compiled with `main` renamed `oz_t_<name>`, so its statics stay
    its own. `nm` showed no other exported symbol in any suite.
  - The host builds the same thing as ctest `selftest_all`, so the runner is tested before
    silicon. `kv_lfs` stays host-only: it simulates power cuts on a RAM flash.
- **Two images**, picked by the link roots (`link_app_mcu.ps1 -Libs … -Name … -RootPattern …`):
  - **Self-test** (`oz_selftest_ak1037.c`, `^oz_selftest_ak1037_`). MSH `oz.selftest` runs the
    suites in a 16 KB thread under kv namespace `zt`, wiped before and after. Then the silicon-only
    checks:
    - SKE engine vs software AES over 256 random blocks
    - FIPS 140-2 monobit on 20,000 raw TRNG bits and on 20,000 conditioned bits
    - no repeat among 64 × 32 B from `oz_port_random`
    - It ends with one line, `OZ SELFTEST PASS|FAIL: n failing, t ms, stack peak x of 16384 B`.
      The stack peak comes from RT-Thread's `#` fill and is the number we need to size the OZKIE
      thread.
  - **Lock** (`oz_lockapp_ak1037.c`, `^oz_lockapp_ak1037_`). MSH `oz.start` runs `oz_lock_init`
    with device_id `ozk-` + the BLE address, log-only actuate/prompt hooks, then
    `oz_gatt_ak1037_start`. `oz.touch` and `oz.code N` stand in for the keypad until P5.
  - kv has one owner per boot (`oz_ak1037_kv_claim`): the self-test wipes its namespace between
    cases, so the lock can't start after it without a reboot, and the reverse. A first version used
    weak cross-references, which made ld pull the whole self-test into the lock image through the
    archive index. Replaced.
- **Build (VictorPC, GCC 10.1.0):** `make -f port/ak1037/Makefile selftest` is `-Werror` clean.
  - Suites: `-Wall -Wno-unused-function` (the shared `oz_test.h` helpers). One dead variable was
    removed from `test_lock.c`.
  - Commands: `-Wno-cast-function-type`, because the SDK's `AK_MSH_CMD_EXPORT` casts every command
    to `long (*)(void)`.
  - Both images link with no unresolved symbols. Each map keeps exactly its own FSymTab entries.

  | Image | text | data | bss | text+data vs PROG (798,720 B) |
  |---|---|---|---|---|
  | SDK `app_mcu` (reference) | 708,496 | 2,024 | 16,132 | |
  | self-test | 794,016 | 2,024 | 101,504 | 796,040 B, **2.6 KiB headroom** |
  | lock | 753,828 | 2,024 | 47,076 | 755,852 B, 42.8 KiB headroom |
  | both together (not built) | 797,488 | 2,024 | 130,924 | **744 B over** |

  - The suites add 47 KB of text. `t_lock`'s simulated apps and locks are 42 KB of bss.
  - The image budget goes mostly to the SDK's demo `app_mcu`: about 78 KB fingerprint matcher
    (`btl_*`) and 21 KB MP3 decoder (`mad_*`). Our own app without those is the real headroom
    lever. Item 5's partition table is the other lever (the default table has no 2nd app bank).
- **Unverified until the kit:**
  - whether the shell actually reaches the commands
  - thread priorities: `rtconfig.h` says 8 levels but the shell asks for 20, so the priorities are
    macros (self-test `RT_THREAD_PRIORITY_MAX−2`, lock `/2`)
  - the byte order of `com_ble_get_dev_addr()` against what a phone scans
  - whether the SDK `printf` handles `%zu`/`%lld` in the suites' messages (pass/fail counts don't
    depend on it)
- **Also done in this item:**
  - the kv fixes above (4389aa5, 3639482)
  - the commit gate (22d6df9)
- 🔴 **Fixed 2026-10-29, found while planning P5:** `oz_ct_equal()` returns **0 when equal**, and
  two AK1037-only callers had it backwards.
  - The P3 SKE known-answer test (`oz_port_ak1037.c`) trusted the engine when its output
    **differed** from FIPS-197 C.1 and fell back to software when it matched. A broken engine
    would have been used for all AES (fail-open); a working one was silently replaced by software.
  - The 4b `ske_vs_software()` check would have reported every block as a mismatch.
  - Neither file built on the host, so no test could see it. P5-A below now host-builds both, and
    its SKE good/wrong/error cases fail if either comparison is inverted again.

### P4-prep item 5 (2026-10-29): burning, and the OZKV partition

**Burn procedure** (VictorPC; the board on a USB-UART, as the SDK's eide upload task does):
1. Build and link: `make -f port/ak1037/Makefile selftest`, then
   `link_app_mcu.ps1 -Libs … -Name app_mcu_ozselftest|app_mcu_ozlock -RootPattern …` (item 4b).
2. Package: `port\ak1037\burn\mkimage.ps1 -Name <image>`. It writes:
   - `app_mcu\bin\ozkie_app.bin`: `.bootcode`+`.data`, then `.flash`, the SDK's own layout. It fails
     if the image is bigger than PROG as read from the config.
   - `app_mcu\bin\ozkv_blank.bin`: 64 KiB of 0xFF, so the first boot formats OZKV.
   - `tools\burntool\config_ozkie.txt`, converted to the UTF-16LE + BOM + CRLF the tool reads.
     Checked byte for byte against the SDK's file.
3. From `tools\burntool`, run `Anyka_BurnCli_Tool.exe -b 2000000 -f config_ozkie.txt` (the SDK's
   own command line, from `app_mcu/.eide/eide.json`). The tool says "press the boot button to start
   the burning mode". It auto-searches COM ports unless they're given.
   - 🔴 **Not run yet.** There's no board, and VictorPC also hosts the Linkly/EFT sandbox, so the
     tool must not auto-open its ports. On kit day, give the board's port explicitly. The tool's
     strings show `-c`, `-s`, `-h` and others, but the help text isn't in the binary; run `-h`
     once with the board attached to confirm the flag for the port.
4. Burning `ozkv_blank.bin` wipes the lock's identity and bonds, which is what bring-up wants.
   Drop download line 6 to keep them across a re-burn. Production updates go through OTA, not the
   burner.

**Images packaged 2026-10-29** (the `.bin` sha256 varies with the SDK objects' build stamp):

| Image | `ozkie_app.bin` | PROG 798,720 B free |
|---|---|---|
| self-test | 796,040 B | 2,680 B |
| lock | 755,852 B | 42,868 B |

**Partition table `port/ak1037/burn/config_ozkie.txt`** (sizes in KiB). It is the SDK's
`config_app_mcu.txt` with exactly these changes: OZKV added, FPDAT shrunk to pay for it, and the PROG
file renamed. The total stays the SDK's 1,974 KiB, because the boot area's size isn't documented.

| | OTA | PROG | LP | ROMFS | UDATA | **OZKV** | FPDAT | total |
|---|---|---|---|---|---|---|---|---|
| SDK default | 212 | 780 | 44 | 210 | 100 | — | 628 | 1,974 |
| ozkie | 212 | 780 | 44 | 210 | 100 | **64** | 564 | 1,974 |

- **PROG stays 780** (PM: size it assuming 5b, so PROG doesn't grow to carry demo code). After 5b
  drops the demo's MP3 decoder (`mad_*`, 21 KB) and other demo code, both images have room to spare.
- 🔴 **The fingerprint matcher is probably not demo code.** `btl_*` (about 78 KB) is `libfp_btl`, the
  SDK's matcher. If the lock ships fingerprint (P5), it stays, and the lock image after 5b is about
  715 KiB, 65 KiB under PROG. Operator decision, via the PM.
- **FPDAT 628 → 564** costs about 10% of fingerprint capacity. That's an estimate from the SDK's
  own ratio (`fpdat100.bin` 1,228 KiB for 100 templates is about 12.3 KiB each), so roughly 51 → 46.
  The SDK's `fpdat.bin` (628 KiB) no longer fits and isn't burned; the fingerprint store is unused
  until P5.
- **ROMFS** holds only the demo's voice prompts (`connected.mp3`, `update.mp3`, …). Whether the lock
  speaks is a product decision. Without prompts, ROMFS (210) could pay for OZKV instead of FPDAT.
- **Secure boot (P6):** the SDK's secure configs use **PROG 700** (giving 80 KiB to ROMFS/FPDAT).
  The lock with fingerprint (about 715 KiB after 5b) would NOT fit 700. Nothing shows secure boot
  itself needs a smaller PROG, so this looks like a layout choice. Ask Anyka before P6.
- **4 MiB flash:** the SDK supports 4 MiB parts (GD25Q32E, PY25Q32HB). Its 4 MiB config adds an
  `UPDATE` bank as large as PROG (A/B, fail-safe OTA) and a 1,228 KiB FPDAT. The 2 MiB table has no
  second bank, so an OTA (`app_ota`) rewrites PROG in place. If P6's bridge OTA must survive a power
  cut mid-update, 4 MiB is the clean answer. BOM decision, via the PM.
- UDATA (100 KiB) is the SDK's own littlefs (`ak_lfs`, demo config). ozcore doesn't use it. 5b will
  tell whether anything we keep needs it; if not, it can become OZKV.

### P4-prep item 5b (2026-10-29): our own app_mcu, without the SDK demo

- **What changed versus the SDK app.** No SDK file is edited. Two files are ours:
  - `port/ak1037/app/main_ozkie.c` replaces `app_mcu/src/main.c`. It's the same boot path
    (`app_proc` → hal/trap/heap init → `AK_Main` → main thread polling `msh_process()` and
    `com_ble_process()`), without the demo's boot-time `ble_lp_test_start()` (a low-power test task
    that would skew every P6 reading) and without the littlefs power-down test.
  - `port/ak1037/app/gen_app_mk.py` generates `Makefile.ozkie_fp<0|1>_v<0|1>` from the SDK's own
    `.eide` project: every `app_mcu/sample/*.c` (39 files) and `src/main.c` are dropped.
  - The CFLAGS, INCS and LDFLAGS of all four variants are byte-identical to the SDK reference, so
    every platform object is the SDK's.
  - Each sample registered shell commands, and `KEEP(FSymTab)` kept them, so they had been pulling
    in the MP3 decoder, fingerprint, LCD, image, RFID, SM2/PKE and nordic-OTA demos.
- **Optional features** (PM ruling 2026-10-29; kept until the product rules on them):
  - `OZ_WITH_FP` (`--fp`): the platform fingerprint component (it registers its own command, which
    keeps `libfp_btl`/`libfp_mrw`) plus the SDK's `fp.*` sample, for kit bring-up.
  - `OZ_WITH_VOICE` (`--voice`): the audio component plus the SDK's `audio.*` sample player. The
    MP3 decoder links only if something plays, so the flag has to link a player. Our own voice
    module replaces the sample in P5 if the product keeps prompts.
  - RFID is not behind a flag. The SDK's `rfid_*` came in only through `sample_rfid.c`, and P5
    brings our own driver.
- **Checks:**
  - All four variants link with no unresolved symbols and no undefined weak symbols.
  - Every weak default the demo overrode is overridden the same way in ours.
  - The lock image carries `com_ble_init/process`, `com_ble_set_gatt_service`, `msh_process` and our
    `oz.*` table entries, and no `ble_lp_test_start`.
- **Sizes** (VictorPC, `SOURCE_DATE_EPOCH` pinned; text+data against PROG 780 KiB = 798,720 B):

  | `OZ_WITH_FP` | `OZ_WITH_VOICE` | lock image | free | self-test image | free |
  |---|---|---|---|---|---|
  | 0 | 0 | 266,780 | 531,940 | 241,304 | 557,416 |
  | 1 | 0 | 450,496 | 348,224 | 428,988 | 369,732 |
  | 0 | 1 | 343,224 | 455,496 | 318,912 | 479,808 |
  | 1 | 1 | **526,860** | 271,860 | 506,204 | 292,516 |
  | SDK demo app (item 4b) | | 755,852 | 42,868 | 796,040 | 2,680 |

  - Fingerprint costs about **184 KB** and voice about **77 KB**, measured by link. My estimate from
    `btl_*` symbols alone (78 KB) missed `libfp_mrw` and the rest of the component.
  - The self-test image is smaller than the lock image because it never starts BLE, so the BLE
    stack isn't linked into it.
  - **Consequences for item 5's decisions:**
    - Even with everything on (fp1_v1, 515 KiB), the lock fits the secure-boot tables' **PROG 700**
      with 185 KiB to spare. Decision 3 is no longer a blocker.
    - On 4 MiB with A/B banks (decision 4), a PROG of about 640 KiB per bank would leave room for
      P5/P6 growth.
    - The bss of the lock image is 42 KB of 218 KiB SRAM.
- **Build:**
  1. `python3 port/ak1037/app/gen_app_mk.py <sdk> ../../ozcore <out> [--fp] [--voice]` (on the Mac,
     against the SDK copy)
  2. copy the Makefile to `app_mcu`, then `make -f Makefile.ozkie_fp1_v1`
  3. `link_app_mcu.ps1 -Makefile Makefile.ozkie_fp1_v1 -Libs … -Name … -RootPattern …`
  4. `burn\mkimage.ps1 -Name …`
- **Unverified until the kit:** whether the board boots and behaves the same without the demo. Any
  init that a sample did as a side effect would show here; none was found in the link check. The
  first kit-day boot should compare the boot log with the SDK demo's.

### P5-A (2026-10-29): port/ak1037 on the host, against SDK mocks
- **What:** `oz_port_ak1037.c`, `oz_gatt_ak1037.c`, `oz_lockapp_ak1037.c` and `oz_selftest_ak1037.c`
  now build and run in ctest under the gate (all three legs, ASan+UBSan included). They link the
  AK1037 port, not the host one.
  - `port/ak1037/hostmock/`: fake SDK headers with the SDK's names and the prototypes the port
    uses, written from the SDK's headers, not copied (the SDK stays out of the repo), plus
    `mock_sdk.c`. The SKE engine is software AES, or one that flips a bit, or one that errors.
    The TRNG is good, stuck or timing out. The flash is a 2 MiB RAM NOR (programming only clears
    bits) with an OZKV partition at 1 MiB; any access outside it is counted. "Interrupts off" is a
    counter, and the SKE driver counts calls made with interrupts on. Message queues have
    RT-Thread's capacity (a 4-byte header per message, as on RV32). Threads are pthreads; the
    OZKIE thread runs in lockstep with the test (it parks in `rt_mq_recv`), so runs are
    deterministic. Time is virtual. `AK_MSH_CMD_EXPORT` registers the command so tests run it by
    name.
- **Tests (9 ctests):**
  - `ak1037_port_{good,wrong,error}`: the SKE KAT keeps a good engine and rejects a wrong or
    failing one. AES equals software every time. The key registers hold zeros after each block.
    SKE runs only with interrupts off.
  - `ak1037_port_good` also covers the TRNG (stuck and timeout refused, then recovers) and the
    32-bit ms wrap. For kv: a filesystem that plain littlefs wrote at the partition offset is
    read, not reformatted. What the port writes, plain littlefs reads back. A failed program
    fails that call only.
  - `ak1037_port_{blank,nopart,badpart}`: a factory-blank partition is formatted. A missing or
    unaligned partition fails kv, and nothing is written.
  - `ak1037_gatt`: `oz.start`, then a simulated phone claims the lock and unlocks it using only
    the mocked BLE stack: GATT writes and reads, link events, notifications.
    - Covered: advertising checked byte for byte on every change (open, busy, open); a 247 MTU
      request; EDHOC at MTU 23 over several notifications; the pair request held until `oz.code`;
      `info` flipping to claimed.
    - Back-pressure: BUFFER_FULL ×3 retried 5 ms apart. A ring that never drains is given up
      after 50 tries. A notify refused outright loses that reply, and nothing hangs.
    - Junk writes are dropped: wrong row, empty, over MTU−3. A 12-frame burst overflows the
      8-deep queue safely.
    - Frames after a disconnect are ignored. An advertising update the stack refuses is retried
      every pass. A second `oz.start` is refused.
  - `ak1037_selftest_{good,wrong}`: `oz.selftest` runs all 8 suites on the AK1037 port and mock
    flash, then the SKE and TRNG checks. It reports PASS with a good engine; with a wrong one the
    suites pass (software fallback) and the SKE check says FAIL. Afterwards `oz.start` is refused
    (one kv owner per boot), and so is a second `oz.selftest`.
- **Mutation-checked (each caught by at least one test):** KAT comparison inverted (5 fail);
  `ske_vs_software()` comparison inverted; key-register wipe skipped; SKE with interrupts on;
  BUFFER_FULL treated as fatal; kv blocks addressed without the partition offset.
- **What it does not show:**
  - The real SDK's behaviour: these mocks encode our reading of its headers, and the open items
    above (notify index, address byte order, adv restart) stay open.
  - Stack use: code runs on pthread stacks, so the self-test's "stack peak" reads 0 here.
    P5-D measures stacks.
- **Found, then fixed (PM ruling 2026-10-29):** after one failed mount, `kv_ready()` never retried,
  so kv stayed down until reboot, and a lock with no kv has no identity. It now tries again on a later
  call, at most once per 5 s (`OZ_KV_RETRY_MS`), so a dead flash can't spin the OZKIE thread. A failed
  attach unmounts before the next try. `ak1037_port_retry` covers it:
  - a partition that appears, then a dead flash that comes back
  - 20 and 50 calls in between with no new attempt
  - nothing before 4.999 s, a retry at 5 s
  - mutation-checked: no rate limit, and no retry, each fail it

### P5-B (2026-10-29): the door keypad, with the wrong-PIN limit
- **Policy (PM nexus-a8, 2026-10-29), in `oz_limit` (`include/oz_limit.h`, portable, pure):**
  - 5 misses in a row lock the keypad out for 60 s.
  - Each further miss locks it out again, doubling: 2, 4, 8, 16 min, then capped at 30 min.
  - Only a success resets it: the right PIN, or an unlock from the owner's app (admin role,
    `lock/unlock`).
  - While locked out nothing is checked at all: the entry is dropped (`OZ_PIN_LOCKED_OUT`) and is
    not a miss.
  - The same module serves RFID in P5-E.
- **Persistence:** kv `"pl"` (8 bytes), saved after every change.
  - The monotonic clock restarts at boot, so a lockout is saved as the time it still has to run,
    and that time restarts in full at the next boot. Pulling the battery can only lengthen a
    lockout, never end it.
  - A record that exists but can't be read fails closed: a 30 min lockout.
  - A factory reset erases `"pl"` (it is an authenticated admin action).
- **`oz_lock_keypad_pin` (engine):**
  - Anti-peep: it opens if a live PIN appears anywhere in an entry of 1..16 digits. Every window of
    every PIN credential is compared, then the first live match is used.
  - Entries over 16 digits, or with non-digits, are misses.
  - A matching PIN outside its time window or out of uses is a miss like any other, so the
    lockout can't reveal that a PIN exists.
  - Returns `OZ_PIN_OPEN` (1), `OZ_PIN_WRONG` (0) or `OZ_PIN_LOCKED_OUT` (2, new).
    `oz_lock_keypad_lockout_ms()` gives the time left, for the panel.
- 🔴 **Rule: PINs are at least 6 digits (`OZ_LOCK_PIN_MIN`; PM ruling 2026-10-29). `cred` refuses
  shorter, with `OZ_R_BAD_REQUEST`.**
  - Why: anti-peep lets one 16-digit entry test up to 16 − L + 1 positions of an L-digit PIN. With
    4 digits, covering every PIN under the lockout takes about 16 days, which is too little. With 6
    digits it takes about 90,000 entries: about 5 years.
  - 📣 **Apps and server:** PIN entry and PIN generation must require **6 or more digits** (up to
    16). A shorter PIN is refused by the lock. Existing v2 locks have not shipped, so there is
    nothing to migrate.
- **`oz_keypad` (`include/oz_keypad.h`, portable, pure):** the entry state machine.
  - Digits, `*` clears, `#` enters; a 10 s pause drops a half-typed entry.
  - Up to 17 digits are kept, so an over-long entry still reaches the lock and counts as a miss.
  - While the lock's pairing prompt is on, `#` after exactly 6 digits gives the pairing code.
- **AK1037:**
  - The keypad runs on the OZKIE thread: `oz_gatt_ak1037_key()` posts one key per event.
  - The lock's `pair_prompt` hook switches the keypad into pairing mode.
  - New shell command for bring-up: `oz.key 4826#`. Results are logged until the panel and buzzer
    exist.
- **Tests:**
  - `keypad` (new suite, also in the self-test image): the full doubling sequence to the cap;
    300 further misses stay at the cap; a miss during a lockout changes nothing; success resets;
    save and load (remaining time restarts at boot, level kept, four malformed records fail
    closed, over-cap values clamped); the keypad state machine (clear, idle drop across the
    32-bit ms wrap, 17-digit cap, pairing codes with leading zeros, other lengths dropped).
  - `lock`: anti-peep (inside, at the end, 16 vs 17 digits); 3-digit PIN refused; a spent one-use
    PIN is a miss; 5 misses give a 60 s lockout in which the right PIN isn't checked; the lockout
    survives a reboot; an owner unlock clears it; a bad record gives 30 min.
  - `ak1037_gatt`: the pairing code is now typed via `oz.key`. Over the mocked stack: 4 misses, a
    60 s lockout on the 5th, the next entry not checked, an owner app unlock clears it, the idle
    drop, `*`, `#` on nothing.
- **Mutation-checked, all 10 caught:** no lockout check; no persistence load; no owner reset;
  exact-length match only (no anti-peep); no 16-digit cap; no 4-digit minimum; no doubling; a
  bad record fails open; 4 free misses instead of 5; a spent PIN not counted as a miss.
- **Not in P5-B:** buzzer and panel feedback, and the real keypad driver (touch IC, scan); those
  need the board. An "owner unlock" means the app's `lock/unlock` with the admin bond; a member
  unlock does not clear the lockout. The server/app is not told about lockouts (no event yet).

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
