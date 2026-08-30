# P6 — Rebuilding arduino-esp32 static libs to raise the BLE connection ceiling

**Why this exists.** Stock `arduino-esp32` caps concurrent BLE connections at
**3** on the ESP32-C6, which caps a bridge at *2 locks + 1 phone*. That is not
silicon: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` accepts **1–70** on the ESP NimBLE
controller family (C6/H2/C2), and the ESP-IDF *default* is 3. The value is
compiled into precompiled static libraries, so `arduino-cli` cannot override
it — the libs must be rebuilt.

Cost is ~1 KB DRAM per connection, so 10 connections ≈ 10 KB. For reference the
bridge idles around 174 KB free heap with nothing connected.

Directive: `XFtposDecisions-138` §P6. Firmware finding: `docs/ozkey-45.md`.

---

## 0. Confirm the ceiling you actually have

Do not trust a remembered number — read the sdkconfig you are *linking
against*, keyed to your installed core version and target:

```sh
F=~/Library/Arduino15/packages/esp32/tools/esp32c6-libs/<CORE_VER>/sdkconfig
grep -E '^CONFIG_BT_(NIMBLE_ENABLED|NIMBLE_MAX_CONNECTIONS|NIMBLE_ROLE_)' "$F"
```

On core `3.3.11` this reported `NIMBLE_ENABLED=y`, `MAX_CONNECTIONS=3`, and
both `ROLE_CENTRAL`/`ROLE_PERIPHERAL` — i.e. 3 shared across *both* roles.

## 1. Host prerequisites (macOS — this is where it bites)

`build.sh` assumes GNU userland. macOS ships BSD `sed`/`awk`, and the failures
are late and unhelpful (`ERROR: gsed not installed`, then `ERROR: gawk not
installed`, one per run).

```sh
pip3 install cmake          # avoids a system-wide install; ninja/python3 already present
brew install gnu-sed gawk   # provides gsed / gawk on PATH
```

`cmake` via `pip` is deliberate — it lands in the active Python env rather
than mutating system state.

## 2. Clone the builder

```sh
cd ~/Documents/Dev
git clone --depth 1 https://github.com/espressif/esp32-arduino-lib-builder.git
```

## 3. 🔴 Pre-clone `arduino-esp32` yourself — do not let build.sh do it

`build.sh` clones `arduino-esp32` with **full history**, which is large enough
to time out on an ordinary connection. Observed failure:

```
error: RPC failed; curl 56 Recv failure: Operation timed out
fatal: early EOF / fetch-pack: invalid index-pack output
```

Worse, the failure is not fatal to the script — it continues and every later
step fails with `cannot change to '.../components/arduino': No such file or
directory`, which looks like a different problem entirely.

A **blobless partial clone** carries a fraction of the data and keeps full
refs (196 MB vs a multi-GB full clone):

```sh
cd esp32-arduino-lib-builder
git config --global http.postBuffer 524288000
git config --global http.lowSpeedLimit 1000
git config --global http.lowSpeedTime 300
git clone --filter=blob:none https://github.com/espressif/arduino-esp32.git components/arduino
```

`build.sh` detects the existing checkout and skips its own clone.

## 4. The actual override

Target defconfigs live in `configs/defconfig.<target>`. The C6 file enables
NimBLE but never sets a connection count, so it inherits the IDF default.
Append:

```
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=10
CONFIG_BT_NIMBLE_MAX_BONDS=10
```

Raise `MAX_BONDS` alongside it — a bridge that can hold N links but only bond
3 of them is a trap that surfaces only under load.

## 5. Build (one target only)

```sh
./build.sh -t esp32c6
```

Restricting to one target matters: the default builds every chip Espressif
supports, which is a multiple of the time and disk.

Expect ESP-IDF plus the RISC-V toolchain to be fetched on first run. On a
machine with no prior IDF install this dominates the wall-clock time, and it
is the step most likely to fail on flaky network — rerun rather than debug,
git resumes what it can.

## 6. Install and verify

Built libraries land in `out/`. Copy the target's `lib/` and `sdkconfig` over
`~/Library/Arduino15/packages/esp32/tools/esp32c6-libs/<CORE_VER>/`.

🔴 **Back up the stock directory first** — this replaces vendor artefacts, and
you will want a way back that does not involve reinstalling the core.

Verify against the *installed* file, not the build output:

```sh
grep MAX_CONNECTIONS ~/Library/Arduino15/packages/esp32/tools/esp32c6-libs/<CORE_VER>/sdkconfig
```

Then raise `OZ_BLE_MAX_LOCKS` in `blelock/bridgeBLE/ozblecentral.h` to match,
recompile `bridgeBLE`, and **test with more locks than the old ceiling
allowed** — 3 is the number that used to fail, so 4+ concurrent is the real
proof. Watch `heap=` on the bridge's `[LIVENESS]` line across the whole run;
the per-connection cost should show as a step, not a slope.

## 7. Known consequence — this is now yours to maintain

The rebuilt libs are a **local artefact that any core upgrade will overwrite**.
After every `arduino-esp32` update: re-run this procedure, or the ceiling
silently reverts to 3 and the bridge starts refusing the 4th lock with no
obvious cause. That maintenance burden is the accepted cost of not migrating
to native ESP-IDF (`XFtposDecisions-138` §P6, option 2).
