# OZKEY-12 — M4 Status Consolidated, and Two Bench-Tooling Defects Found Chasing It

> **Written 2026-08-05, after a full re-read of `XFtposDecisions-42.md` through `-61.md`**
> (the complete ozkey↔ftpos↔NexusPM record) prompted by the operator pointing out that a
> session picking up from local memory alone had drifted behind that record. This doc is
> the correction: where M4 actually stands, two real bugs found in the bench tooling
> while trying to verify it, and the resume point for the next session. It does not
> change any contract — `blelock/CONTRACT.md` stays canonical for the wire; this is
> status, matching the discipline XF-61 established for the app side.
>
> **UPDATE 2026-08-06 — §7's resume point is DONE, and then some. `doorlock-1.11`
> (committed `af3fdb6`, pushed) fixes the real BLE connect bug.** §8 below is the full
> story, including a wrong theory (§8.1) found and corrected within the same session
> before it shipped. M4's remaining gates (3/4) are now also hardware-verified — see
> §8.4. `§3`'s "NOT yet hardware-verified" for `1.10` is superseded: `1.10` itself
> stayed broken (see §8.1's real diagnosis), `1.11` is the build that actually works.

---

## 1. Why this doc exists

Local session memory (this assistant's own note-taking, outside the repo) had gone
stale: it described `doorlock-1.10` as "flashed but uncommitted" and M4 as "COMPLETE,
proven end-to-end through BANOI 01:35" — both wrong by the time this session started.
`doorlock-1.10` was already committed (`96571bc`), and M4's actual state per the XF
record is considerably more partial than "complete." The operator's correction —
*"you have done all these works.. and not save the result anywhere in the
documentation"* — is the reason this doc exists: XF replies are the cross-team source
of truth, and a private note that isn't cross-checked against them will drift.

## 2. M4 status, corrected (source: XF-59, XF-60, XF-61)

`doorlock-1.9` is the last **hardware-verified** M4 build:

| Gate / feature | Status | Evidence |
|---|---|---|
| Gate 1 — `app_id` → bond lookup (malformed + valid-unbonded) | ✅ verified | XF-59 §7.4, both branches, 2026-08-04 |
| Gate 2 — envelope opens under the matched bond's key | ✅ verified | XF-60 §9, 2026-08-04 22:56 — correctly refused a stale-secret envelope after a factory reset |
| Gate 3 — challenge freshness | ❌ never run | needs one successful envelope open |
| Gate 4 — counter floor | ❌ never run | same prerequisite |
| DPID 101 `bond_revoke` | firmware-complete, unreachable | never written to `…0006` |
| DPID 102 `invite_cancel` | firmware-complete, unreachable | never written to `…0006` |
| DP dispatch split (101/102 refused over MQTT) | ✅ verified | XF-59, module `tx` counter never moved |
| `control` reassembly + 400ms idle backstop | ✅ verified | XF-59 |
| Boot self-test 11/11 (incl. `dp-allow-list`, `dp-frame-101/102`) | ✅ verified | XF-59 |

ftpos's own honest tally, XF-60: **"of fifteen delivered asks, two have ever met a
lock."** Both sides had been building well ahead of what either could bench-verify,
which is not a defect in itself but means "built" and "hardware-verified" must not be
conflated in status reporting — see §4.

## 3. `doorlock-1.10` — what it actually fixes, and its real status

Committed `96571bc`, 2026-08-05 03:38. Two changes:

1. **The SCAN_IND ordering bug** — the actual cause of "sees the lock, cannot connect"
   on a commissioned lock's touch window, which had been (wrongly) attributed to
   ftpos's BLE scan for most of the 2026-08-04 evening. Root cause: `bleSetBusy()` and
   `bleSetConnectable()` both called `esp_ble_gap_start_advertising()`'s completion
   callback before the advertising-type struct was updated, so a queued restart could
   fire with the previous `SCAN_IND` while the struct said `IND` — Bluedroid ignores
   adv-param changes while advertising is already running. Fixed with a single
   `bleRearmAdvertising(connectable, why)` choke point (stop → settle → set type →
   scan response → settle → start); `bleSetConnectable()` deleted as the trap itself.
2. **`[BOOT] reset reason` logging** via `esp_reset_reason()`.

**Status: committed, NOT yet hardware-verified.** The commit message says so explicitly
— the two-unlocks-with-no-reboot acceptance test has never been run, and the bench
session that would have run it hit two tooling failures instead (§5). Do not treat
`doorlock-1.10` as proven until that test passes and is recorded here or in a
`session-state` memory with a timestamp.

## 4. Adopting the XF-61 ledger discipline for firmware-side status too

XF-61 (operator directive, quoted there: *"ask app team to update the XF-xx doc to
record which tasks have been implemented, otherwise we are chasing our own tail"*)
made ftpos add a status/commit/hardware-verified table to every reply, and BANOI now
shows its build SHA (`-dirty` suffix if uncommitted) on-screen and in every unlock log
line. The asymmetry XF-61 named — `doorlock.ino`'s `FW_VERSION` has always answered
"what firmware is this," nothing on the app side did until then — is worth restating
as a standing rule for this repo too:

> **State the build every status claim is about, and mark hardware-verified
> separately from compiled/merged.** "M4 is done" is not a claim anyone can act on;
> "M4 gates 1-2 verified on `doorlock-1.9`, gates 3-4 unrun" is.

This doc follows that discipline for §2 above and should be updated (or superseded by
`ozkey-13.md`) rather than left to go stale the way the private session memory did.

## 5. Two bench-tooling defects found while trying to verify `doorlock-1.10`

Neither is a firmware or protocol defect — both are in `blelock/bench/`, the untracked
tooling directory referenced in earlier session notes. Recording them here since they
cost most of a session and would cost the next person the same time if rediscovered
from scratch.

### 5.1 `duallog.py` busy-spins at ~100% CPU on a stale file descriptor, producing silent empty output

The read loop:

```python
try:
    data = os.read(fd, 4096)
except (BlockingIOError, OSError):
    continue
if not data:
    continue
```

When the underlying USB-CDC device re-enumerates (e.g. after any board reset) while
`duallog.py` still holds the old file descriptor, `select()` can continue reporting
that fd as "ready," and `os.read()` returns `b''` (EOF) without raising — which falls
straight into `if not data: continue`, a **tight loop with no sleep**. Observed: one
core pinned at 99%+ CPU for over 90 minutes, `serial.log` completely empty the entire
time, while the lock itself was fully healthy and heartbeating normally over MQTT the
whole time (confirmed independently via `mosquitto_sub` direct to the broker). This
produced a false impression of dead/hung hardware and drove a long, unnecessary
hardware-debugging detour.

**Not yet fixed.** The right fix is: on `not data`, close the stale fd and attempt a
bounded reopen-with-backoff rather than `continue`. Until fixed, the diagnostic is
`ps aux | grep duallog` — if it's burning CPU with an empty or stalled log, kill and
restart it before concluding anything about the hardware.

### 5.2 The actual boot banner is very hard to capture at all, independent of the above bug

Even with a clean `duallog.py` process started immediately after a triggered reset
(via `arduino-cli upload` or `esptool --after hard-reset`), the very first ~300ms of
serial output — including `[BOOT] reset reason`, which exists specifically to answer
"why did this board just restart" — was lost to the race between USB re-enumeration
and the reader re-attaching, in every attempt made this session. This is a real,
unresolved capture gap, not (as far as could be determined) a firmware issue. No fix
attempted yet; flagging as an open bench-tooling gap.

### 5.3 A process note, not a tooling bug: don't trigger resets on hardware someone is watching without saying so first

Several `esptool` resets run while chasing §5.2 were mistaken by the operator for the
board self-resetting on its own (a real, previously-documented failure mode — see the
2026-08-04 brownout/swap-exhaustion history). They were not; they were this session's
own diagnostic commands, run without announcing them first. No repo change follows
from this, but it's recorded because the confusion cost a real round of
troubleshooting and is easy to repeat.

## 6. Where the XF corpus actually lives, for the next person who needs to re-check status

`~/Documents/Dev/ftpos/ftposDecisions/XFtposDecisions-NN.md`, sequential across both
correspondents (ozkey and ftpos raise numbers from the same counter; NexusPM also
participates on the doorlock-sync-adjacent threads, XF-50/51/54). As of this doc the
highest is XF-61, next free ask-letter is `(BA)`. `blelock/CONTRACT.md` is the
canonical wire record; the XF docs are status and negotiation, not a second source of
truth for the protocol itself — where they disagree, CONTRACT.md wins (this rule is
stated inside CONTRACT.md itself per XF-58 §10.1's reasoning about configuration vs
transient fact).

## 7. Resume point (as of 2026-08-05 — superseded, see §8)

1. ~~Run the `doorlock-1.10` two-unlocks-with-no-reboot acceptance test~~ — run, and it
   **failed** (see §8.1). This is what led to finding the real bug.
2. ~~If it passes, send ftpos the owed correction in XF-60/61~~ — never applicable; the
   SCAN_IND theory turned out to be real but insufficient (§8.1 explains).
3. Exercise M4 gates 3/4 and DPID 101/102 — gates 3/4 done, §8.4. 101/102 still open,
   §8.5.
4. `duallog.py` per §5.1 — fixed in-session (EOF now triggers a bounded reopen instead
   of a busy-spin), but a *second*, more fundamental capture problem was never solved
   — see §8.2. The fix in §5.1 is still correct and still in the file; it just wasn't
   the whole story.

## 8. 2026-08-06 — the real bug, found, fixed, verified. `doorlock-1.11`.

### 8.1 First theory (wrong): an async BLE config race

Running §7 item 1 above, `doorlock-1.10` failed to accept a BLE connection — on *every*
attempt, including the very first one against a lock factory-reset seconds earlier, on
two unrelated BLE stacks (BANOI/`flutter_blue_plus`, a bench script using `bleak`).
Neither ever saw `connectionState` reach `connected` (confirmed by ftpos adding proper
instrumentation on their side, `XFtposDecisions-62.md` §8 — `trace=[]`, i.e. rejected
at the link layer, not a dropped connection).

The first diagnosis read `BLEAdvertising.cpp` (the installed Arduino BLE library,
core 3.3.11) and found what looked like a real race: `bleRearmAdvertising()` triggers
what appeared to be two independent async GAP config chains (advertising data +
scan-response data) that could both call `esp_ble_gap_start_advertising()`
independently. A fix was written — a custom GAP event observer plus
wait-for-real-completion logic — and it compiled clean.

**It was diagnosing dead code.** The functions read (`BLEAdvertising::start()`,
`stop()`, `handleGAPEvent(esp_gap_ble_cb_event_t, ...)`) sit inside
`#if defined(CONFIG_BLUEDROID_ENABLED)` (`BLEAdvertising.cpp:639`) through `#endif`
(`:1434`). A two-line `#ifdef` probe sketch (`arduino-cli compile --verbose`, checking
which of `CONFIG_BLUEDROID_ENABLED` / `CONFIG_NIMBLE_ENABLED` is actually defined)
confirmed this exact ESP32-C6 target compiles **NimBLE**, not Bluedroid. None of that
code runs on this board. The observer-based fix was reverted in full before flashing
anything — caught by checking the backend before trusting the analysis, not by testing
on hardware.

### 8.2 Second theory (right): the two backends' advertising-type enums don't mean the same thing

`doorlock.ino` defines:
```c
#define OZ_ADV_TYPE_IND      0x00
#define OZ_ADV_TYPE_SCAN_IND 0x02
```
— Bluedroid's `esp_ble_adv_type_t` numeric values. `BLEAdvertising::setAdvertisementType
(uint8_t)` writes that raw byte straight into whichever backend's own struct field is
active, with **no translation**:
```c
#ifdef CONFIG_BLUEDROID_ENABLED
  m_advParams.adv_type = (esp_ble_adv_type_t)adv_type;
#endif
#if defined(CONFIG_NIMBLE_ENABLED)
  m_advParams.conn_mode = adv_type;
#endif
```
NimBLE's `conn_mode` (`ble_gap.h:2156-2158`, verified directly against the installed
header) is a **different enum at the same numbers**:
```c
#define BLE_GAP_CONN_MODE_NON  0   // non-connectable
#define BLE_GAP_CONN_MODE_DIR  1   // directed-connectable
#define BLE_GAP_CONN_MODE_UND  2   // undirected-connectable
```
So `connectable=true` (passing `0x00`) was setting `conn_mode=BLE_GAP_CONN_MODE_NON` —
genuinely **non-connectable** — and the busy state (`0x02`) was setting
`BLE_GAP_CONN_MODE_UND`, genuinely **connectable**. Exactly backwards, as a constant,
on every boot, from the very first BLE attempt — which is the whole symptom, with no
timing component needed to explain any of it.

Also resolved as a side effect: **NimBLE's `start()`/`stop()` call
`ble_gap_adv_start()`/`ble_gap_adv_stop()` directly and synchronously** (no async
completion events for configuration at all, unlike Bluedroid) — so the §8.1 fix's
entire premise (needing to wait for async completion) was doubly wrong: wrong backend,
and the right backend doesn't even have the class of problem being solved for.

### 8.3 Fix, and why bridge32.ino was never affected

```c
#if defined(CONFIG_NIMBLE_ENABLED)
  #define OZ_ADV_TYPE_IND      BLE_GAP_CONN_MODE_UND
  #define OZ_ADV_TYPE_SCAN_IND BLE_GAP_CONN_MODE_NON
#else
  #define OZ_ADV_TYPE_IND      0x00
  #define OZ_ADV_TYPE_SCAN_IND 0x02
#endif
```
`bleRearmAdvertising()` reverted to its original simple form (stop → settle → set type
→ scan response → settle → start) — the §8.1 observer machinery is gone; nothing in
it was needed.

`bridge32.ino` was checked by source read (`grep` for `setAdvertisementType`,
`OZ_ADV_TYPE_*` — no hits) and found to never call `setAdvertisementType()` at all; it
relies on the library's default `conn_mode`, which is `BLE_GAP_CONN_MODE_UND`
(connectable) under NimBLE already. It was never exposed to this bug. Confirmed on
real hardware anyway via a full bridge + Thread-mode-lock commissioning + remote
unlock run (§8.6), not just by inspection.

Firmware version bumped to `doorlock-1.11` per the project's own "bump the minor on
every flashed change" rule — what was flashed and tested is not the same code as the
committed `doorlock-1.10`.

### 8.4 Hardware verification

Flashed onto the bench lock (`ozk-b0a6048b5fd8`) already carrying an active bond —
survived the flash (NVS untouched, `enrolled_at` unchanged). Then:

- **4 consecutive successful BLE connections from BANOI, zero reboots between any of
  them** — the exact acceptance test `doorlock-1.10` was originally meant to pass and
  never did.
- A real BLE unlock exercises M4 gates 3 (challenge freshness) and 4 (counter floor) —
  both now hardware-verified alongside the already-known-good gates 1/2. **All four
  `control` gates are hardware-verified as of this session.**

### 8.5 Still open for M4

DPID 101 `bond_revoke` / 102 `invite_cancel` + the role-gate matrix
(`XFtposDecisions-59.md` §3's table) — firmware-complete, never written to `…0006`.
Needs a member bond enrolled first (QR invite ceremony from bond #0, the owner). Not
started this session; blocked on getting an invite payload from BANOI to a bench
script (`blelock/bench/ozctl.py enroll`) — no established handoff path yet for getting
QR-encoded text out of the phone app and into a terminal.

### 8.6 Bridge + Thread — also verified, on `doorlock-1.11`

Operator-run: create bridge → create Thread-mode lock → remote unlock via BANOI, full
cycle, working. Latency observed 5-8s (vs. a single historical measurement of ~1s for
the same relay path); operator-accepted as good enough, not investigated further —
no serial and no Thread uplink/ack path (a known pre-existing gap, `ozkey-11` §4) made
it impractical to get a precise server-side latency breakdown this session.

### 8.7 Committed and pushed

`af3fdb6` — `blelock: doorlock-1.11 — the real cause of 1.10's BLE connect failure`,
pushed to `main`. Full narrative (including the reverted wrong theory, stated
plainly) also lives in `XFtposDecisions-62.md` §9 (wrong) → §10 (M4 gates) → §11
(correction + fix + verification), which is the cross-team copy of this section.

### 8.8 Updated resume point

1. Member bond enrollment + DPID 101/102 + role gates (§8.5) — the one remaining M4
   surface.
2. `[BOOT] reset reason` — still never captured. Serial was broken most of this
   session too (3+ independent tools, zero bytes, board provably alive throughout via
   MQTT); never diagnosed. This session's real bug was found entirely by source
   reading + empirical hardware tests instead — worth remembering as a working
   fallback method when serial isn't cooperating, not just a workaround of last
   resort.

## 9. 2026-08-06 (continued session) — member ceremony verified, a real bridge
   security hole found and fixed, `list_bonds` built, and one open mystery

> This section is written carefully after the operator flagged that parts of the
> live discussion earlier tonight were asserted with more confidence than the
> evidence actually supported (the bridge-ownership sequence, §9.3). Confirmed
> facts and open questions are labeled explicitly below — do not silently upgrade
> an "UNCONFIRMED" to a fact in a future session without re-checking it.

### 9.1 Member ceremony (XF-63/64) — CONFIRMED working end-to-end

Real hardware, `doorlock-1.11`: BANOI #1 invites → BANOI #2 uses ftpos's new
`showInviteScanFlow()` (XF-63 fix, build `8f4720a`) to scan the QR → `redeemInvite()`
succeeds → at-the-door BLE ceremony completes (`MEMBER_OK`) → BANOI #2 unlocks for
real. Repeat-enrollment after a self-revoke also confirmed working (XF-64 fix, build
`5c53e9d`, the `is_deleted` filter).

### 9.2 Doorlock re-paired after the bridge's Thread network changed — CONFIRMED

Sequence: BANOI #2 exploited the (then-unfixed) bridge to factory-reset it, which
wiped its Thread dataset. The doorlock still held the old dataset and self-formed as
an orphaned "Thread: LEADER". Fix required no factory reset on the doorlock — bond #0
(owner identity) lives independently of Thread state (`ozcrypto.h` `g_bonds[0]`,
`OZ_BOND_SAME` is idempotent on re-provision by the same `app_id`). Recovery: short
BOOT press (or the '*'-then-'5' touch gesture) opens the doorlock's 60s BLE
maintenance window; BANOI #1 re-added the lock as if new; same `app_id` matched
`g_bonds[0]`, so the firmware treated it as `OZ_BOND_SAME` and applied the bridge's
current Thread dataset. **Confirmed working** — doorlock rejoined as a Child.

### 9.3 Bridge ownership guard (`bridge32-1.4`) — PARTIALLY confirmed, one open mystery

The fix (`bridgeOwnershipCheck()` in `bridge32.ino`, gating both the `reset`
sentinel and normal Wi-Fi/broker provisioning in `applyProvision()`) was written,
compiled, and flashed earlier this session (before this doc's §9 was written) —
see the summarized history for the diff; **not yet committed to git as of this
writing** (fixed below, §9.6).

**Confirmed tonight:**
- BANOI #2 attempted a remote factory-reset of the bridge → refused ("Reset bridge
  failed — can not reset bridge").

**NOT confirmed, stated as fact earlier tonight in error:**
- That the refusal above proves the bridge was *owned* at that moment. It doesn't —
  `bridgeOwnershipCheck()` returns `false` (and the write is refused) for **two**
  different reasons: `ownerAppId` is empty and the claim window is closed
  (`BRIDGE_CLAIM_REQUIRED`), or `ownerAppId` is set and doesn't match the caller
  (`BRIDGE_DENIED`). Both look identical from the outside ("reset failed"). This was
  the actual mistake — treating "it failed" as confirmation of the more specific
  claim ("it failed *because* it's owned") without checking which status code fired.
- A second attempt, BANOI #2 → "Ghép Bridge qua Bluetooth" (reconfigure) → got
  `BRIDGE_CLAIM_REQUIRED` ("Bridge chưa có chủ. Nhấn nhanh nút BOOT...") — the
  **unowned** message, not the owned-by-someone-else one. ftpos's app code was
  checked and does correctly distinguish the two
  (`banoi_doorlock.dart:3556-3564`) — this is not an app bug, the app is accurately
  reporting what the bridge itself said.

**What this means, and what's still open:** the bridge's current `ownerAppId`
appears to be empty, which contradicts the earlier belief that BANOI #1's
re-provision (after the fix was flashed) had claimed it. Re-reading
`bridgeOwnershipCheck()`/`loadConfig()`/`saveConfig()` found no logic bug — a
claim, once made, is written to NVS (`prefs.putString("owner", ...)`) inside the
same `saveConfig()` call the rest of provisioning already relies on, and an
existing owner can never be overridden by a later claim regardless of the BOOT
window (the check is unconditional once `ownerAppId.length() != 0`). So the code
*read* is consistent with "an owner, once set, cannot be displaced" — which is the
security property that actually matters and held up under BANOI #2's attempts
either way (unowned-and-locked or owned-and-denied both refuse a non-owner
identically). But **whether BANOI #1's earlier claim actually persisted, and if
not why, was never directly verified** — no serial capture, no NVS dump, no
firmware-version check on the bridge to confirm which build was running at the
time of that claim. The proposed next step (not yet run): have BANOI #1 attempt to
reconfigure the bridge again **without** pressing BOOT first — if it succeeds
without the claim window, ownership is intact and the `BRIDGE_CLAIM_REQUIRED`
BANOI #2 saw is unexplained; if BANOI #1 *also* gets asked to press BOOT, that
confirms the owner record really is gone, and the next step is figuring out why
(bench brownout on this exact setup has been suspected before, see
[[doorlock-brownout-suspicion]] in memory — unconfirmed whether that applies to
bridge32's board too).

### 9.4 XF-65 raised and answered — a redeemed invite never becomes revocable

Found live testing the admin-revoke role-gate row: BANOI #1's member list only ever
offers "Huỷ lời mời" (cancel invite), never "Thu hồi" (revoke), for a member who
already has a fully working bond. Root cause, confirmed by grep across
`doorlock_service.dart`: `pending_invite → active` is a documented (`:298`, `:317`)
but never-implemented status transition — nothing in the codebase ever writes
`status='active'` from `'pending_invite'`. Raised as `XFtposDecisions-65.md`.
ftpos independently hit the same symptom the same night and confirmed the same root
cause. They flagged a real gap in the ask: BANOI has no wire capability to learn a
member's pubkey/bond-add either, so the fix needs a new read path first.

### 9.5 DPID 103 `list_bonds` — reserved, BUILT, compiled clean, NOT YET FLASHED

Operator's call: reserve the DPID ourselves for testing rather than wait on
process — same "ours, invented" pattern as 101/102 (`CONTRACT.md:303`).

- **Request:** envelope-encrypted `control` write, DP 103, empty payload,
  admin-only (same ordering as `handleBondRevoke`'s admin check, so a member can't
  enumerate other members' pubkeys).
- **Response:** the same `{slot, label, floor, pub}` array the boot serial dump
  already builds, JSON-serialized, sent via a new `ozNotifyChunked()` helper —
  reuses `chrMember` (…0007) for the reply, now `WRITE | NOTIFY`, mirroring the
  *existing* inbound chunked-JSON reassembly convention (`memberBuf`) in reverse
  rather than inventing a second mechanism. 180 B/chunk, 30 ms between
  notifications.
- Wired into `ozControlDispatch()` (`dp == 103 → handleListBonds()`), described in
  `describeDpid()`, `FW_VERSION` bumped to `doorlock-1.12`.
- **Compiled clean** (`esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB`,
  62% flash, 24% RAM). **Not flashed to hardware yet** — compiling ≠ verified;
  nothing about `list_bonds` has been exercised on a real board or against real
  BANOI code yet. Documented back to ftpos in `XFtposDecisions-65.md` §6 so their
  reconciliation-pass side has a concrete shape to build against.

### 9.6 Git status as of this write-up

`bridge32.ino` (ownership guard, `bridge32-1.4`) and `doorlock.ino` (`list_bonds`,
`doorlock-1.12`) are both modified-but-uncommitted in the working tree. Committing
both now, in separate commits, per the operator's explicit instruction before
ending this session. `blelock/bench/` (the `duallog.py` CPU-spin fix from earlier
in the session) and this doc are also being committed at the same time for the same
reason — nothing else in the working tree is touched.

### 9.8 2026-08-06 (continued) — the §9.3 mystery is CLOSED, not open

Root cause, found by reading the actual diff of `60d2b82` rather than guessing
further from live behavior: `ownerAppId`, `prefs.getString("owner", ...)`, and
`prefs.putString("owner", ...)` are **all new lines in that exact commit** —
nothing pre-existing was modified. `bridge32-1.3` and everything before it had
no owner concept in NVS at all. So "the owner record didn't appear to
persist" was the wrong framing — **there was never anything to persist.**
Whatever BANOI #1 did earlier under 1.3 was ordinary WiFi/broker provisioning
with nowhere to write an owner even if it had wanted to. Every "unowned"
response the guard gave tonight, to both BANOI #1 and #2, was correct.

This is also provable purely from app-level behavior, independent of the git
diff: if BANOI #1 had ever successfully claimed the bridge since ownership
tracking existed, BANOI #2 (a different `app_id`) would see `BRIDGE_DENIED`
(mismatch), never `BRIDGE_CLAIM_REQUIRED` (unowned). Both saw unowned. Only
one explanation fits both identities getting the same answer.

**Resolution test, run for real tonight, all three branches now hardware-verified:**
1. BANOI #1, no BOOT press → `BRIDGE_CLAIM_REQUIRED` (already recorded, §9.3).
2. BANOI #1, BOOT pressed (claim window open) → **claim succeeded.** (One loose
   end: BANOI #1's app showed this as adding a bridge under a new name rather
   than recognizing the existing one — an app-side bookkeeping question, not a
   firmware one; not yet followed up.)
3. BANOI #2, no BOOT press, immediately after step 2 → `BRIDGE_DENIED`
   ("Bridge already has owner. Connected with another phone") — the owner
   guard now actively rejects a non-owner with no window needed, exactly per
   `CONTRACT-BRIDGE.md`'s two-clause rule. **`bridge32-1.4`'s ownership guard
   is fully confirmed end-to-end, not partial.**

Serial corroboration was attempted for both step 2 and step 3 and got nothing
either time — consistent with `bridge32.ino`'s own comment (line ~73) that
this board's USB-CDC serial has been unreliable to capture all along; not
treated as contradicting the app-level result, which is unambiguous on its
own for the reason above.

**Two bench-tooling items opened chasing this, both in the working tree,
uncommitted as of this writing:**
- `duallog.py` had a **second**, previously-undiscovered reconnect bug
  (distinct from the 2026-08-05 EOF-spin fix in §5.1): the reconnect-scan
  only ran when `fds` was completely empty, so once any one port
  reconnected, a still-dead *other* port was never retried again. Fixed by
  checking for missing ports every pass (throttled to 1/s).
- `bridge32.ino`: `LCD_IDLE_OFF_MS` temporarily disabled (operator request,
  bench-only) — a BOOT press to wake the idle-blanked screen was found to
  also open the 60s claim window as a side effect, contaminating exactly the
  kind of ownership test this section is about. Bumped to `bridge32-1.5`.
  **Compiled? No — edited but not yet built or flashed.** Revert note is
  inline at the `#define`.

### 9.7 Resume point for the next session — updated post-§9.8

1. ~~Run the BANOI #1-without-BOOT-press diagnostic~~ — done, §9.8. Bridge
   ownership guard fully confirmed end-to-end, not just partially.
2. ~~If owner record really is gone, find out why~~ — done, §9.8: it was never
   set, not lost (owner tracking is new in `1.4`). No further digging needed.
3. **Flash `doorlock-1.12`** and hardware-verify `list_bonds` (DPID 103): request
   from an admin bond, confirm the chunked JSON reassembles correctly on a real
   BLE client (a bench script is probably faster to stand up than waiting on
   ftpos's reconciliation UI), confirm a member-role sender gets `LIST_DENIED`.
4. Once `list_bonds` is verified, ftpos can build the `pending_invite → active`
   reconciliation pass (XF-65 (BD)) against a real wire capability instead of a
   proposed one — then the admin-revoke-a-member role-gate row (still untested)
   becomes reachable.
5. **New, small:** figure out why BANOI #1's claim-under-BOOT-window showed up
   in the app as adding the bridge under a new name instead of recognizing the
   existing entry (§9.8 step 2's loose end) — app-side, not firmware, low
   priority.
6. Compile + flash `bridge32-1.5` (LCD idle-off disabled for bench work) if the
   claim/deny testing above is considered done for now; revert the `#define`
   once serial capture reliability on this board is no longer needed for active
   debugging.
7. `[BOOT] reset reason` — still open, still lowest priority.
8. Thread relay latency (§8.6) — flagged, not investigated, operator says it's fine.

## 10. 2026-08-07 — shared-core refactor, keypad+hex UI, XF-66/67, latency
    investigation opened (not closed)

Long session, several distinct threads. Recorded here so the next session
doesn't have to reconstruct it from git log alone.

### 10.1 `doorlock.ino`/`doorlock19.ino` unified onto a shared core

`blelock/common/ozdoorlock_core.h` — new. Extracted the ~3260 lines that were
byte-identical between the two boards (crypto, BLE GATT, bond table, MCU
forwarding, self-tests, Wi-Fi/Thread transport, dispatch) into one shared
header, `#include`d by both boards' now-thin `.ino` files (~150 lines each:
pins, palette, display/touch driver, coordinate transform). `ozcrypto.h`
consolidated the same way (was byte-identical duplicate, confirmed via
`diff` before merging).

**Real snag, now fixed**: Arduino's auto-prototype generator only scans the
primary `.ino`, not `#include`d headers, so every forward-reference call
(working "for free" in the old monolithic file) broke once the code moved
into a header. Fixed with 67 explicit forward declarations at the top of
the shared header, verified against the actual function list, not guessed.

**Version scheme unified** (operator directive — "no point to keep them
separated"): both boards now share one `FW_VERSION`/`FW_DISPLAY_VERSION`
string (`doorlock-1.21` as of this write-up) instead of two diverging
counters. Full per-version changelog lives in each board's own `.ino` file
header (bumped together on every flashed change, per the existing "bump the
minor" rule).

**`blelock/Makefile`** — new, wraps `arduino-cli`. `BOARD` names the
hardware (`147`, `19`, `GEEKC6`, `dlock19`), not the software role — bridge32
firmware runs on `GEEKC6` today, `P4C6COMBO` is a reserved-but-unconfigured
slot for the planned future hardware (P4+C6 combo, 32MB flash/32MB PSRAM/
128GB SD/768KB RAM, meant to also host a hotel PMS server). Every board here
needs `CDCOnBoot=cdc` in its FQBN or native USB serial never appears at all
(confirmed the hard way on all three — 1.47", 1.9", and the bridge). Bridge
is `FlashSize=16M` (confirmed via `esptool flash_id`, not assumed).

**USER_BUTTON made explicit on all three boards** (doorlock, doorlock19,
bridge32) — was silently inheriting the ESP32-C6 toolchain's generic
`BOOT_PIN` default with zero board-specific verification. The 1.9" board's
BOOT-hold factory reset "not working" turned out to be the operator holding
the wrong physical button (board was upside down on the bench, BOOT/RESET
swapped from expected) — GPIO9 is confirmed correct once the actual BOOT
button is used. `ozdoorlock_core.h` now `#error`s at compile time if a board
forgets to define it (tested: temporarily removed the define, confirmed the
guard fires, restored it).

**Hardware-verified**: both 1.47" and both 1.9" units flashed and boot
clean on the final build of the night (`doorlock-1.21` firmware string, LCD
layout at `1.21`/`v1.21` on-screen) — self-tests PASS, bonds survived every
reflash (NVS untouched by app-partition writes, as expected), Thread
rejoined as Child on the healthy units.

**One casualty**: the 1.9" unit `ozk-acebe63acab8` hit three consecutive
`esptool` write-timeout/serial-noise failures on one USB connection,
corrupting its app partition into a genuine "no bootable app partition"
boot loop (bootloader itself still responded fine — confirmed via
`flash_id`). Recovered cleanly once the operator swapped to a known-good
cable; NVS/bonds were untouched throughout since the corruption was
app-partition-only, never a full chip erase.

### 10.2 LCD UI — keypad simplified, hex-command readout, iterated live on
    real hardware

Multiple real bugs found and fixed only by looking at the actual screen —
recording the mechanism-level lessons, not just the end state, since the
end state will keep changing:

- **The `#define` ordering bug**: layout macros (`BADGE_TOP`/`HEX_TOP`/
  `KP_TOP`/etc.) were originally defined near `drawKeypad()`, far below
  `drawOperational()` which uses them first — C preprocessor macros only
  apply after their definition point in the file, so this silently used
  stale/undefined values until moved earlier. Compiles clean either way;
  only wrong at runtime. Worth remembering as a class of bug this codebase
  can hit again.
- **The real "big red line" bug**: `drawHexReadout()` kept using `STATUS_H`
  as its y-coordinate after the badge macros were renamed to `BADGE_TOP` —
  both pointed at the same pixel row, so the hex row's black background
  painted directly over the color bar on every redraw. Two macros meaning
  "the same row" by accident, not a typo in a single line.
- **Flicker**: the 3s periodic "keep IP/role current" tick originally set
  the same `screenDirty` flag a real state change does, triggering a full
  `fillScreen()` every 3s. Split into `drawStatusLine()`, a function that
  clears/redraws only its own thin row (deliberately inset 2px so it never
  touches the border's own pixels) — the periodic tick calls it directly,
  bypassing the full-redraw path entirely. Real state changes (lock/unlock,
  BLE window, a tap) still go through the normal full-redraw path.

**Current layout** (both boards, `1.21`): one status line — `OZLOCK
V1.21 THREAD CHILD LOCKNAME10 [BLE 20s]` (version/transport/role-or-IP/
10-char lock name/BLE countdown, countdown gets a filled amber badge when
the window's open) — then a 40px LOCKED/UNLOCKED color bar + hex-command
readout on line 2, then a 4×2 drawn keypad (`1234`/`*56#` — reduced from
4×3 since these taps only exercise touch zones, there's no real PIN
backend). LockSim/MCU-UART forwarding is unchanged and still real — the hex
readout is additive, not a replacement (operator correction after an
earlier wrong assumption on ozkey's part).

This layout is the product of ~10 iterations against real hardware feedback
in one session — treat it as current, not final. The next real screen
complaint should be trusted over this doc.

### 10.3 XF-66 — `DELETE /locks/:id` now reports delivery likelihood

Real finding, not hypothetical: the operator hit this live — deleted an
orphaned (Thread-partitioned) lock from BANOI, app showed it gone, physical
lock never factory-reset, because the `factory_reset` MQTT publish had no
path to a lock that wasn't on the bridge's mesh. Confirmed this was the
exact XF-66 gap ftpos had already flagged (fire-and-forget MQTT, no
delivery confirmation).

**Shipped** (`ozlockserv/server.js`, live via `node --watch`, not yet
independently re-verified against a real delete beyond the reload itself):
`DELETE /locks/:id` now returns `{ ok: true, id, reset: { attempted,
likely_delivered } }` — `attempted` from `mqttPublish()`'s own
connected-check (previously computed and silently discarded),
`likely_delivered` from a `last_seen_at`/`heartbeat_s` freshness heuristic
(2.5× grace window) computed before publishing. Honestly labeled as a
heuristic, not a delivery guarantee — ftpos's own `UnlockResult.delivery`
already has the same "we know it left, not that the door moved" property,
so this matches an existing convention rather than inventing a new one.

ftpos confirmed the shape and built their side (`DeleteLockResult{attempted,
likelyDelivered}`, three distinct outcome messages replacing one blanket
`catch`) — full exchange in `XFtposDecisions-66.md`.

### 10.4 XF-67 — `control`'s "no bond" vs "bad envelope" behavior

ftpos asked whether an unrecognized sender gets a distinct denial, the
generic denial, or silence. Read `ozControlTry()` directly
(`ozdoorlock_core.h:2504-2571`) rather than reasoning about it: **no bond at
all** answers immediately with `UNLOCK_DENIED` (fails at the `ozBondFind()`
lookup, no envelope-open attempted); **bond exists but envelope fails to
open** answers with the *same* `UNLOCK_DENIED`, but only after the idle
timer gives up waiting for more chunks. Same token both ways, different
timing — no wire change requested or made.

Side effect: this answer led ftpos to find and fix a real bug on their own
side — `_control()`'s outcome-wait only ever matched tokens starting with
`REVOKE_`, so a real, already-arrived `UNLOCK_DENIED` sat unrecognized until
their 10s timeout fired anyway, for every control verb, not just
self-revoke. Full exchange in `XFtposDecisions-67.md`.

### 10.5 Thread-relay unlock latency — investigation opened, NOT concluded
    (⚠ CLOSED in §12 — modem-sleep theory below was falsified by data)

Operator-reported: some remote unlocks take 5-15s to reach the door, wildly
variable, units 10-15cm apart (so not a signal-strength story). Traced the
send-side path end to end (server → MQTT → bridge → Thread) with real
correlated timestamps (`duallog.py` extended to N ports for this — it was
already generic despite the name, no code change needed — plus a new
`blelock/bench/mqttlog.py` for millisecond-timestamped MQTT alongside serial):
**one real capture showed the full send-side chain complete in under 130ms**
(MQTT receipt → bridge multicast+unicast send, all three delivery attempts
fired within 2ms of each other; a non-target node received the multicast in
67ms). Send-side is not obviously the bottleneck, at least not in that one
capture.

**Working theory, NOT verified**: `bridge32.ino` never calls
`WiFi.setSleep()`, and the installed Arduino-ESP32 core's own default for
ESP32-C6 is `WIFI_PS_MIN_MODEM` (confirmed by reading
`WiFiGeneric.cpp:385-389` directly — S2 defaults to `WIFI_PS_NONE`, every
other target including C6 defaults to modem-sleep ON). Modem-sleep means the
AP buffers packets for the bridge and only delivers at the next DTIM beacon
— which would explain variable, distance-independent delay that disappears
right after recent activity keeps the radio awake. Fits the reported
symptom shape well, but **the confirming experiment was never completed**:
the operator deliberately let the bench idle to test cold-start latency, a
power reset happened before the test command was sent, and the whole
capture was lost. Nothing was changed in code — `WiFi.setSleep(false)` is
proposed, not applied.

**Resume point**: redo the idle-then-command test with the same 4-port +
MQTT `duallog.py`/`mqttlog.py` setup (both scripts already exist in
`blelock/bench/`, both are keepers regardless of what this test finds). If
the pattern holds — slow after genuine idle, fast back-to-back — add
`WiFi.setSleep(false);` right after `WiFi.begin()` in `bridge32.ino` and
retest. If it doesn't reproduce, the multicast/MPL theory from earlier in
this same investigation (bridge32's own code comment calls its multicast
relay a "v0" stopgap pending real unicast-to-mesh-local-address routing) is
the fallback hypothesis, untested either way.

### 10.6 Not committed as of this write-up

`blelock/bench/ozctl_state.json` — contains a bench identity **private
key**, deliberately never committed, should stay untracked (add to
`.gitignore` if it keeps showing up in `git status`).
`docs/Sovereign-Edge-Paper-v4_5.pdf` — appeared in the working tree this
session, not part of any of the above work; left alone rather than guessed
at.

## 11. 2026-08-07 (continued) — M4's last surface closed: member ceremony +
    DPID 101/102, hardware-verified live, entirely from the bench

§8.5 left one thing open for M4: a member bond had never actually existed on
a lock, so DPID 101 (`bond_revoke`) and 102 (`invite_cancel`) — firmware-
complete since `doorlock-1.8` — had never been reachable. The blocker was
assumed to be a missing handoff path for getting a BANOI-generated QR's raw
text into `ozctl.py`. That assumption was wrong, and the real path turned
out to need no phone at all.

### 11.1 The invite doesn't need BANOI — bond #0 already has everything

The invite MAC (`CONTRACT.md` "Member-enroll lock-side algorithm") is keyed
on `mac_key = HKDF-SHA256(ikm = bond#0 pairing secret, ...)` — the SAME
X25519 secret `ozctl.py` already derives for every `control` write
(`build_control()`'s `our_priv(st).exchange(lock_pub)`). Whichever identity
holds bond #0 can mint a valid invite by pure local computation; no BLE
write, no BANOI, no QR. Added to `ozctl.py`:

- `invite <label> [--role] [--ttl]` — connects only to read `info`
  (device_id + lock pub), then builds `OZINV1:` + base64url JSON locally.
- `--state <path>` — lets one script act as two identities in the same
  ceremony (bond #0 mints, a second identity redeems), since a real
  invite/enroll round-trip needs two distinct keypairs.

**Verified byte-exact against ftpos's own frozen test vector**
(`packages/ozkey_commissioner/tool/gen_invite_vector.dart`) before trusting
it against real hardware: `pairing_secret=01..20 hex,
device_id="ozk-a4cf12879da7", issuer="aa"*32, label="Ba Ngoai",
nonce="42"*16, expires=1789000000` → both the dart tool and the new Python
implementation independently produced
`mac=e7780baea8feef5674c0ffecd1b83f35dfd9198db50cea6d0735c7a43d268aac`. This
is the same vector the firmware's own boot self-test (`invite-mac`,
`invite-b64url`, present since `v1.6`) already checks on every boot — so all
three independent implementations (firmware, ftpos's dart reference,
ozctl.py's new one) now agree.

### 11.2 Live hardware finding: the bench identity wasn't bond #0 on ANY
    currently-powered lock

Before minting a real invite, `list_bonds` against the bench's default
identity was run as a sanity check — and it failed, on **two different**
boards in a row: the 1.9" unit (`ozk-acebe639f8c4`) and the 1.47" unit
(`ozk-b0a6048b5fd8`, which had held a bench bond as recently as the M4
gate-3/4 verification session, §8.4). Serial confirmed directly both times:
`[CTL] 9d8a16dd1654fdc0… holds no bond on this lock` → `UNLOCK_DENIED`. The
operator had since paired all 3 bench locks to BANOI, and `BOND_DENIED`'s
atomic-refusal guarantee (XF-47) meant a later BANOI provision attempt on
`b0a6048b5fd8` would have been refused outright if the bench bond were still
there — so the most likely explanation is the lock was factory-reset (by the
operator, deliberately or as part of BANOI's own pairing flow) between that
session and this one. Point worth keeping: **`ozctl_state.json`'s identity
being bond #0 on a given lock is a claim that decays over time and must be
re-checked live, not assumed from a prior session's memory.**

**Resolution**: factory-reset the 1.47" board (`* 5` on the keypad — no
remote path exists, deliberately, XF-52) and re-provision it with the bench
identity as bond #0, dedicating that one unit to bench ceremony testing
going forward. `info.pub` changing (`de0e8149…` → `6ade7eec…`) confirmed the
reset on serial, matching `CONTRACT.md`'s own factory-reset acceptance test.

### 11.3 Two more bench-tooling defects found live (same pattern as §5 —
    tools untested against real hardware since they were written)

1. **`ozprov.py`'s `--server-ip`/`--server-port` write the WRONG field.**
   `build_payload()` only sets `broker_host`/`broker_tcp_port` — what
   firmware actually gates provisioning acceptance on
   (`ozdoorlock_core.h:1882`, `ENROLL_FAIL` if empty) — when `--broker
   HOST:PORT` is passed explicitly. `--server-ip`/`--server-port` populate a
   *different* pair of fields entirely (presumably the hotel/PMS server
   address, unused by the WiFi provisioning ladder). First provision attempt
   with only the (more prominent, better-defaulted) `--server-ip` flag
   produced `ENROLL_FAIL`; fixed by adding `--broker 10.1.1.20:1883`
   explicitly. `ozprov.py` was not changed — this is a usage gotcha, not a
   code defect, but one sharp enough to record: **the flag that looks like
   "where's the server" is not the one firmware checks.**
2. **`ozctl.py` had no way to pin a specific lock**, and `BleakScanner.
   find_device_by_name("OZLOCK")` matches whichever advertiser answers
   first. With 2+ boards genuinely in their touch window at once (a real
   condition on a bench with several units), a `list_bonds` aimed at the
   1.47" unit silently ran against the 1.9" unit instead — same generic
   name, no error, wrong device, and the only evidence was the returned
   `device_id` not matching. Harmless in this instance (a `UNLOCK_DENIED`
   either way) but a real correctness gap. **Fixed**: added `--addr <BLE
   address>` to `ozctl.py`, pinning `find_device_by_address()` instead of
   scanning by name when supplied. Every command after this fix targeted
   the lock's BLE address directly rather than trusting the advertised
   name to be unique.

### 11.4 Full ceremony, hardware-verified end to end, entirely from the
    bench (no BANOI, no QR)

Six-step run against `ozk-b0a6048b5fd8` (1.47", bench-owned per §11.2), each
step gated by a real keypad touch opening the lock's own 60s BLE window —
no shortcuts, no simulated input:

1. `list_bonds` (owner identity) — confirmed bond #0, `LIST_OK` with 0
   members (correct: `handleListBonds()` starts enumeration at slot 1,
   deliberately excluding the owner, `ozdoorlock_core.h:2408`).
2. `invite "Ba Ngoai"` (owner identity) — minted `OZINV1:...` locally, no
   write.
3. `enroll <invite>` with a fresh second identity (`--state
   ozctl_state_member.json`) — **`MEMBER_OK`**. First time this path has
   ever fired over real BLE.
4. `list_bonds` (owner identity) — confirmed the new bond: `slot=1
   label='Ba Ngoai' floor=0`, pubkey matching the member identity exactly.
5. `revoke <own pubkey>` (member identity, DPID 101 self-revoke) —
   **`REVOKE_OK`**. Serial: `[CTL] OPENED — bond 1, counter 1, DP 101`.
6. `invite "throwaway"` → `cancel <nonce>` (owner identity, DPID 102) —
   **`REVOKE_OK`**, then a third identity's `enroll` against that same
   invite string returned **`MEMBER_REPLAY`** (not `MEMBER_OK`), serial
   confirming the MAC itself still verified (`invite VERIFIED
   label='throwaway'`) but the burned nonce blocked redemption — proving
   102 is a real kill switch, not just an accepted-and-ignored write.

A `POWERON` reset landed on the 1.47" board mid-session (operator power-
cycled it deliberately) between steps 4 and 5 — bond table and self-tests
(`invite-mac`, `invite-b64url`, `dp-frame-101/102`, `M4 dispatch split
holds`) all came back clean afterward, confirming NVS persistence held
through it.

**M4 is now fully hardware-verified** — every row in §2's table plus DPID
101/102 and the role-gate matrix. Nothing remains open from the original M4
scope (`XFtposDecisions-59.md` §3).

### 11.5 BANOI's OWN ceremony cross-verified — phone-to-phone, no bench
    tooling involved at all

§11.4 verified `ozctl.py`'s new invite/enroll implementation against real
firmware. Immediately after, the operator ran the equivalent test with
BANOI's actual production implementation on two physical phones
(`banoi1`/`banoi2`) against `DoorA` (`ozk-acebe639f8c4`, a lock BANOI
already owned as bond #0 — no re-provisioning needed):

1. `banoi1` (bond #0) issued a member invite through BANOI's own UI —
   pure local computation, no BLE write, same as `ozctl.py invite`.
2. `banoi2` scanned/accepted it. Serial: `[MEMBER] invite VERIFIED
   label='M1' expires=... (not enforced)` → `nonce burned — cache 1/64` →
   **`MEMBER_OK`**.
3. `banoi2`, as the new member, immediately issued "Mở cửa" (open door).
   Serial: `[CTL] OPENED — bond 1, counter 1, DP 1` → `[CTL] unlock
   authorised by bond 1 ('M1', member)` → **`UNLOCK_OK`**.
4. `banoi1`, the owner, also tested "Mở cửa" — confirmed good, unaffected
   by the new member's bond existing alongside bond #0.

This is the first time BANOI's own member-ceremony implementation has been
verified end to end against real firmware — everything before this session
was either bench-tooling verification (§11.4, `ozctl.py` against
`ozk-b0a6048b5fd8`) or, per ftpos's XF-60 tally, simply never bench-
verified at all. Two independent app-side implementations (ftpos's dart
reference test vector, and now BANOI's own production code) and the bench's
third (`ozctl.py`) all agree with the firmware. Nothing in `blelock/`
changed to make this pass — it worked on the first attempt.

### 11.6 Committed this session

`ozctl.py` — `invite`, `--state`, `--addr`. No firmware changes; nothing
else in `blelock/` touched.

## 12. 2026-08-08 — Thread-relay unlock latency (§10.5) — CLOSED, modem-sleep
    theory falsified by data, real cause identified

Operator reported the felt latency had dropped to 1-3s and wondered if the
app team had fixed something on their side. Checked both possible causes
before trusting the observation:

- **Firmware**: `bridge32.ino` still has no `WiFi.setSleep()` call anywhere
  in the tree — the §10.5 fix was never applied. Whatever changed, it
  wasn't that.
- **App**: the only latency-adjacent ftpos commit in the window is XF-67's
  `_control()` outcome-wait fix (`af7443f`) — scoped to the member-ceremony
  `REVOKE_*`/`LIST_*`/`UNLOCK_DENIED` path in `member_ceremony.dart`, not
  the plain owner-unlock path (`bleUnlock`). Doesn't touch what a normal
  unlock button press does.

Neither explains a felt change, so the send-side relay path itself was
re-measured directly instead of guessing.

### 12.1 A capture was already running and covered both the "slow" and
    "fast" periods

A `duallog.py` (4 serial ports) + `mqttlog.py` pair from the §10.5
investigation was still running in the background, unattended, since
11:26 this morning through tonight — the same session that raised §10.5
never actually stopped it. That gave one continuous, millisecond-
timestamped record spanning the entire period in question, instead of a
fresh one-off sample.

Matched every `[MQTT] << .../command` line (server → bridge) to the
corresponding `[FWD] cmd -> MCU` line (bridge/lock → Tuya MCU, i.e. the
lock has fully received and is acting on it) by `target` device ID:

| Time | Target | MQTT publish → lock receives |
|---|---|---|
| 11:29:50 | `ozk-acebe63acab8` | 217ms |
| 11:29:56 | `ozk-acebe639f8c4` | 265ms |
| 11:31:35 | `ozk-acebe63acab8` | 278ms |
| 18:35:54 | `ozk-acebe639f8c4` | 215ms |
| 18:35:59 | `ozk-b0a6048b5fd8` | 265ms |
| 18:36:03 | `ozk-acebe63acab8` | 291ms |
| 19:16:56 | `ozk-b0a6048b5fd8` | 236ms |
| 22:16:49 | `ozk-acebe63acab8` | 282ms |
| 23:44:42 | `ozk-acebe639f8c4` | 309ms |
| 23:44:46 | `ozk-b0a6048b5fd8` | 291ms |

**Every clean delivery, this morning and tonight alike, is 215-310ms.**
There is no fast-period/slow-period split in the relay path itself — it
has been uniformly fast the entire time the capture ran, including the
exact hours when 5-15s delays were being reported. That rules out
modem-sleep (or any other bridge/relay-side explanation) as the dominant
cause: if it were real, it would show up in *some* of these deliveries,
not none.

### 12.2 The one real outlier traces to board connectivity, not the relay

One command (11:31:39, target `ozk-b0a6048b5fd8`) never produced a
matching `[FWD]` line within the normal window — the bridge sent its usual
three-way multicast/unicast attempt, but nothing came back from that
lock. Checked what that lock's own serial port (`port143101`, confirmed by
`[ID] device_id=` lines through the rest of the log) was doing at the
time: **silent for ~11 hours**, from 01:05 the previous night through
12:04 the next day — no `[MON]` heartbeat, nothing, spanning the whole
11:31 incident. That matches the already-documented flaky-USB-adapter-
cable / intermittent-reset issue on this exact board
([[doorlock-brownout-suspicion]]), not a firmware or relay defect — a
board that's mid-reset or hasn't rejoined the Thread mesh yet will produce
exactly this shape of multi-second-or-worse delay on the next command sent
to it, independent of anything `bridge32` does.

### 12.3 Conclusion

The relay path (server → MQTT → bridge → Thread multicast/unicast → lock)
was never the bottleneck, in either the slow period or now. The §10.5
`WiFi.setSleep()` theory is **falsified** by this data, not just
unconfirmed — dropped, no code change needed. The 5-15s reports are best
explained by the same bench-level connectivity flakiness already tracked
elsewhere (flaky USB adapter cable, intermittent `POWERON` resets); the
"now it's 1-3s" observation isn't a fix, it's the relay being what it
always measured as (sub-second) minus that flakiness happening not to hit
during observation. No firmware or app change is warranted from this
investigation. `duallog.py`/`mqttlog.py` remain in `blelock/bench/` as
general-purpose tooling, still worth keeping.

**Open only if it recurs**: if a multi-second delay is felt again, capture
which physical board it hit and check that board's own serial/connectivity
health at the time before re-opening any relay-side theory.

## 13. 2026-08-08 (continued) — XF-68 live session: touch-window root cause,
    then BANOI's first real owner-initiated admin revoke, hardware-verified

ftpos raised `XFtposDecisions-68.md` mid-session: a live 2-phone bench
(`banoi1` = owner, `banoi2` = already-enrolled member on DoorA,
`ozk-acebe639f8c4`) working through XF-65/66/67's remaining untested
surfaces, stuck on `banoi1`'s "Đồng bộ" (member-list sync) scan returning
zero devices for DoorA despite `banoi2` genuinely using touch-assist
unlock moments earlier.

### 13.1 Root cause (BJ): touch-window timing, not a bug — answered live in
    XF-68 §4

Watched DoorA's serial (port141101, `duallog.py`, already running from the
§10.5/§12 investigation) live. Every touch→connect cycle observed
succeeded cleanly — DoorA (per `CONTRACT.md` line 101-104, 143-153) only
advertises as BLE-connectable for 60s after a keypad/screen touch, closes
automatically, no separate signal distinguishing "outside the window" from
"out of range" (documented at `CONTRACT.md` line 457-461). `banoi1`'s
zero-device scans landed outside a window; nothing lock-side was wrong.
Answered in `XFtposDecisions-68.md` §4 with the live trace and a
recommendation (prompt the user to touch the keypad before scanning,
rather than a bare retry-or-fail).

### 13.2 First real BANOI-initiated admin revoke — hardware-verified live

Once inside a touch-opened window, `banoi1` revoked `banoi2` from DoorA's
member sheet ("Thu hồi") — the one XF-68 §1 item explicitly never
exercised through BANOI's own production code (§11.4/§11.5 of this doc
verified DP 101/102 via `ozctl.py` bench tooling and BANOI's
invite→enroll→unlock ceremony, but never a real owner-tap revoke):

```
02:17:10  [TOUCH] key '3' -> window OPEN 60s
02:17:11  connect — links=1
02:17:13  [CTL] OPENED — bond 0, counter 11, DP 101
02:17:13  [REVOKE] bond 1 ('M1') revoked by admin bond 0
02:17:13  STATUS: REVOKE_OK
02:17:13  disconnect — clean, ~2s round trip after connect
```

**Confirmed effective, not just logged**: a follow-up connect at 02:20:14
(a different pubkey, `63e371206add95f3…`, not bond 0 — banoi2) got
`[CTL] ... holds no bond on this lock` -> `UNLOCK_DENIED`. Operator
confirmed banoi2 could no longer open the door. Real end-to-end proof, not
just a lock-side status string.

**App-side note, resolved, not a bug**: banoi2's row kept showing `active`
in banoi1's member sheet immediately after the revoke. Traced the code
(`banoi_doorlock.dart:4053` `_runBondRevoke` -> `svc.markMemberRevoked` ->
`_refresh()` on `changed==true`) and found nothing wrong — `REVOKE_OK` was
already matched pre-XF-67 (`REVOKE_*` prefix), so the ceremony call itself
never had the UNLOCK_DENIED-style bug. Operator confirmed a full re-open
of the members sheet showed the correct revoked state. UI staleness only,
not a data or protocol defect — not worth a ticket unless it recurs
without the workaround.

### 13.3 Net effect on M4 scope

Closes the last realistic gap between "hardware-verified" (§8, §11) and
"verified through BANOI's own production code" for the owner-revoke path.
Combined with §11.5 (BANOI's own invite/enroll), the member ceremony's
full lifecycle — invite, enroll, unlock, admin-revoke — has now each been
exercised at least once through real BANOI app code against real
firmware, phone-to-phone, not just bench tooling.

## 14. 2026-08-08 (continued) — Next priorities, set by the Project Manager

A scan of `ozkey-09.md` through this doc's outstanding items (§10.5/§13 above now closed)
was reviewed by the Project Manager and returned this order. Recorded here as the
authoritative backlog going forward — supersedes any priority ordering implied elsewhere in
docs 09-12.

| # | Item | Owner | Notes |
|---|---|---|---|
| 1 | `ozlockserv` relay-opaque migration — sovereignty-breach fix (server stops storing plaintext credentials) | Server/Firmware (ozkey team) | Highest remaining architectural promise from the whitepaper. M4 is done; this is the next step to restore full sovereignty. To be scoped and started. |
| 2 | Bridge32 → MQTT uplink — lock heartbeat/logs back to server | Firmware (bridge32) | Enables lock state reporting, delivery confirmation, battery monitoring. Core product capability. |
| 3 | QR trust anchor — commissioning (lock side) and bridge provisioning | Firmware | Designed but not built (lock side); not even designed (bridge side). Lower priority than #1/#2, needed for production trust. |
| 4 | `bridge32-1.5` debug edit revert — `LCD_IDLE_OFF_MS` disabled | Firmware | Quick, non-blocking; revert in a spare moment. |
| 5 | Hotel-through-bridge sign-off | System Architect | Architectural decision still pending, not a firmware task. |
| 6 | Thread range stress test — residential RF validation | Firmware/QA | Important, not urgent; schedule after M4 is stable. |
| 7 | `[BOOT] reset reason` serial capture | Firmware | Lowest priority, can be deferred indefinitely, not product-critical. |
