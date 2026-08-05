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

### 9.7 Resume point for the next session

1. **Run the BANOI #1-without-BOOT-press diagnostic** (§9.3) to settle whether the
   bridge's owner record is actually intact or actually lost. Do not assume either
   answer going in.
2. If the owner record really is gone: figure out why (brownout? a reboot between
   the claim and now? something in the reset-refusal path that shouldn't mutate
   state but does?) before trusting `bridge32-1.4` as "done."
3. **Flash `doorlock-1.12`** and hardware-verify `list_bonds` (DPID 103): request
   from an admin bond, confirm the chunked JSON reassembles correctly on a real
   BLE client (a bench script is probably faster to stand up than waiting on
   ftpos's reconciliation UI), confirm a member-role sender gets `LIST_DENIED`.
4. Once `list_bonds` is verified, ftpos can build the `pending_invite → active`
   reconciliation pass (XF-65 (BD)) against a real wire capability instead of a
   proposed one — then the admin-revoke-a-member role-gate row (still untested)
   becomes reachable.
5. `[BOOT] reset reason` — still open, still lowest priority.
3. Thread relay latency (§8.6) — flagged, not investigated, operator says it's fine.
