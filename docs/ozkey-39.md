# ozkey-39 — Two suppliers, not one: the cmd variant is RESOLVED, the raw-payload block is PROVEN, and the handshake belongs to a different vendor

**Status: 🟢 FINDINGS + REPLY TO `ozkey-38`.** Written 2026-08-17 by **firmware**,
answering ozkit's §4 Q3/Q4 and closing part of Q2. Consumers: **ozkit**,
**firmware**, PM/operator.

Companion docs: `ozkey-38` (ozkit's SIMLOCK/DP cross-check, which this replies
to), `ozkey-27` (§2.2/Q1 cmd variant, Q2 raw payloads — **both moved by this
doc**), `XFtposDecisions-110` (the DP 1 fiction regression, same root cause).

Everything below was read out of the two supplier documents in
`docs/DPSuppliers/` this session. Where a claim is a **citation** it is marked
as such; where it is an **inference** it says so. That distinction is the whole
point of this doc — an earlier version of two of these findings was inference
and one of them was wrong.

---

## 0. 🔴 The correction that reframes everything: these are TWO DIFFERENT SUPPLIERS

Per the operator, 2026-08-17:

| File | Vendor |
|---|---|
| `docs/DPSuppliers/T3_Final_Customer_Version_EN.docx` | **Ladin Tech** — T3-U module integration doc |
| `docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf` | **Luona Smart** — DS013-T3, PID `vr4iiuqtyh0q4nix` |

They had been treated — by me, and implicitly by `profiles/` — as two documents
describing one product family. They are not. Both are **Tuya** locks, so the DP
*catalogue* is legitimately shared (it is Tuya's, platform-level), but
**product DP selection, module identity, and the module↔MCU wake handshake are
per-vendor and must not be merged.**

### 0.1 We already merged them once

`profiles/products/tuya-ds013-t3.json` sources its DP list from the **Luona**
PDF (`source.doc` says so), but records:

```json
"module": "T3-U (Beken BK7236)"
```

**That string appears nowhere in the Luona PDF** — no `T3-U`, no `BK7236`.
It is Ladin's module. Either Luona genuinely ships a T3-U (plausible — it is a
Tuya module and Luona is a Tuya lock) or the identity was imported from the
wrong document. **Unverified. Flagging rather than fixing**, because the answer
changes §3.

---

## 1. ✅ cmd variant: RESOLVED for Luona DS013-T3 — it is GENERAL

**This closes `ozkey-27` §2.2 / Q1 for this product, and it is ozkit's §4 Q2
ship-date blocker.**

`ozkey-27` recorded the ambiguity as *"the PDF body describes the LOW-POWER
variant; the PDF's own final 功能协议 table and the T3 module doc both name the
GENERAL variant — two of three signals say general."* One of those three
signals was Ladin's doc, i.e. **a different vendor**, so it should never have
been counted. That leaves the question resting on the Luona doc alone.

**Citation.** The Luona PDF's own instruction table —
*通讯协议(产品功能部分)指令收发表* ("communication protocol (product function
part) instruction send/receive table") — specifies, for **every** functional DP:

```
帧头 0x55aa · 版本 0x00 · 命令字 0x06 (模块发送) / 0x07 (MCU上报) · dpID · 数据类型 · 校验和
```

`0x06` issue / `0x07` report, per DP, throughout. That is the **general**
variant, and it is what `ozdoorlock_core.h` and ozkit's frame codec already
implement.

**Why the doc looked low-power.** Its title is *涂鸦云低功耗通用串口接入协议*
("Tuya Cloud **low-power** universal serial access protocol") and its body does
use `0x05`/`0x08`/`0x09` — but those belong to the **retained-record**
mechanism (滞留记录), a separate layer, not to DP transport. §19 状态数据单元 is
reported under `0x08`; the doc notes the module replies with 命令字 `0x09`
data `0x01` per successfully reported retained record. Two layers in one
document, not two competing variants.

🔴 **I nearly filed the opposite conclusion.** From the title plus a partial
grep of the command-word rows I had "Luona = low-power" as a finding, and
checked it before stating it. Recording that here because the same trap is
sitting in the document for the next reader.

**Scope of this resolution:** Luona DS013-T3 only. **Ladin is not covered** —
see §4.

### 1.1 🔴 …but we IMPLEMENT general incorrectly, in both directions of our own bench

**Added 2026-08-17 after bench verification. This is the most consequential
finding in this doc and it invalidates part of what §1 might otherwise imply.**

Resolving the variant to *general* does **not** mean our code speaks it. The
supplier's table is explicit that general is a **two-command-word** protocol:

```
命令字 0x06  模块发送   (MODULE issues to MCU)
命令字 0x07  MCU上报    (MCU reports to module)
```

**We use `0x06` for both directions.** Verified on the bench this morning —
LockSim, playing the DL MCU, reports as `55 AA 00 06 …`:

```
[09:17:57] [TUYA<-] DP 63 …  55 AA 00 06 00 08 3F 02 00 04 00 00 00 01 53
[09:18:20] [TUYA<-] DP 76 …  55 AA 00 06 00 08 4C 02 00 04 00 00 00 01 60
```

and firmware only ever parses `0x06` on the way in:

| `ozdoorlock_core.h` | direction | check |
|---|---|---|
| 2088 (describe) | inbound | `f[3] != 0x06` → renders as bare `cmd 0x7` |
| 2270, 2483, 2701 | inbound DP dispatch | `f[3] == 0x06` |
| 2752, 4551 | outbound forward | `frame[3] != 0x06` → **correct** |
| 2122 (ack wait) | inbound | `0x06 \|\| 0x07` — the one place `0x07` is anticipated |

So the **issue** direction is right and the **report** direction is wrong.
**A real Luona DS013-T3 reports with `0x07`, and every one of those frames
would be dropped** — no doorbell, no access events, no battery alarm, no
credential reports. They would surface as an unparsed `cmd 0x7` and nothing
else.

**Why the bench never caught it:** `locksim/lib/tuya.ts` defines
`DP_REPORT = 0x06`. LockSim was written to match firmware's assumption, so both
halves of our system agree with each other and disagree with the supplier. This
is structurally the same failure as `XFtposDecisions-110`'s DP 1: *the bench
agrees with itself.* Two instances of it in two days suggests the pattern
matters more than either bug.

🔴 **SIMLOCK inherits this.** `ozkey-38` §3.3 says ozkit's codec "hardcodes the
general variant (`0x06` issue / `0x07` report)". If that is literally what the
code does, ozkit is **correct and firmware is wrong** — worth confirming
explicitly, because if ozkit instead mirrored firmware's `0x06`-both-ways, the
shipped fixture drops every report from a compliant DL-MCU and fails it.

**Fix (firmware, in progress):** parse `0x07` inbound, keep accepting `0x06`
until LockSim is flipped, then flip `locksim`'s `DP_REPORT` to `0x07`. Both are
one-line changes; the risk is entirely in having believed the bench.

## 2. ✅ The raw-payload block is PROVEN, and it changes what we should ask for

`ozkey-27` Q2 says the RAW payload layouts were "not supplied." That was an
absence-of-evidence claim. It is now a **positive citation**: the supplier's
instruction table has a 功能指令 (function instruction / payload) column, and it
is filled in for some DPs and deliberately open for others.

| DP | 功能指令 column |
|---|---|
| 9 免密远程解锁 | `0x00-0xff` |
| 10 远程开门 | `0x00-0xff` |
| 13 添加大容量开锁方式 | `0x00-0xff` |
| 14 删除大容量开锁方式 | `0x00-0xff` |
| **11 连接模式** | **fully specified** — 数据长度 `0x0005`, 类型 `0x04` (enum), 功能长度 `0x0001`, values `keep:0x00` `sleep:0x01` `lock_keep:0x02` `lock_sleep:0x03` |

DP 11 proves the document *can* specify a payload completely when it intends
to. For the raw DPs it writes "any byte of length N."

**So the missing layout is not in a document we have failed to find — it is
content the supplier chose not to specify.** That changes the ask materially:

> **Do not request "the protocol document." We have it.** Request the **byte
> layout for DP 9, 10, 13–19** specifically — the `bulk_method_v1` codec, in
> the catalogue's terms.

Until that lands, the following remain unimplementable, and no amount of bench
work changes it: **remote unlock (DP 10)** and **all credential CRUD
(DP 13–19)** — which is `XFtposDecisions-110`'s open item and ozkit's 15
`reserved` tests, the same gap seen from two directions.

## 3. 🟡 The wake handshake — I withdraw the "contradiction," but a real gap remains

I previously reported that the supplier documents an **active-HIGH, edge-
triggered** handshake while `profiles/` assumes **active-LOW level**, and called
it a contradiction. **Withdrawn as stated** — it compared two vendors.

**Citation (Ladin only), `T3_Final_Customer_Version_EN.docx` §2.2:**

> **2.2.1 Module Wakes MCU** — the module wakes the lock board MCU by sending a
> **300 ms high-level pulse on P32**.
> **2.2.2 MCU Wakes Module** — IO wake-up: the lock board MCU wakes the module
> with a **high-level pulse of at least 1 ms on P13 (rising edge triggered)**.
> UART wake-up: the module can be awakened by MCU TX data.

**Citation (Luona): nothing.** The Luona PDF contains **no** wake or handshake
content — zero occurrences of 唤醒.

Our own profile for the Luona product says:

```json
"srdy": { "assert": "low", "style": "unconfirmed" },
"mrdy": { "assert": "low", "style": "unconfirmed" },
"topology": "unresolved"
```

`"unconfirmed"` was, and remains, the honest value. What is new:

1. **For Ladin, the handshake is fully specified** and we are not building to it
   — active-high pulses, edge-triggered, with concrete durations.
2. **For Luona, we have no handshake spec at all**, so `assert: low` is a guess
   with nothing behind it.
3. **If §0.1's `T3-U` module attribution is correct, Ladin's §2.2 transfers to
   Luona** (it is a module-level property). That single unverified string
   decides whether we have a spec or not.

🔴 **This is two of SIMLOCK's five wires, on a fixture that ships.** A kit
driving active-low levels at hardware expecting a rising edge never wakes it,
and all 50 tests fail at the handshake, at the manufacturer's bench.

**Also uncosted:** Ladin's §3.3 states *"Command 0x84 is not supported."* I can
find no handling or note of `0x84` anywhere on our side.

**Not extracted:** Ladin §3.2's two wake-up flowcharts are **images** and did
not survive text extraction. They are the most likely home for the timing
detail that would settle this. **Someone should look at those two diagrams
directly** — that is a five-minute human task that may answer §3 outright.

## 3.5 🟢 DP 76 may be the usable unlock command we said did not exist

**Added 2026-08-17, prompted by the operator: _"BLE sensor does not come with
the DL MCU."_ That hardware fact is what unlocks the reading below.**

`XFtposDecisions-110` §3/§4 concluded there is **no usable real remote-unlock
DP**: DP 10 is `lock.unlock` but `reserved`/blocked, DP 72 is `event.access`
(a report, not a command). That conclusion now looks **too strong**.

**Citation** — Luona's instruction table, DP 76 = `0x4c`:

```
0x06   0x4c   0x04 …   0x0-0x1869f      模块发送  (MODULE issues)
0x07   0x4c   …                          MCU上报
```

Type `0x04` VALUE, 4 bytes, range `0..99999` — **a fully specified payload**,
unlike DP 10's `0x00-0xff`. And it is issuable **by the module**.

**Ladin agrees on intent:** *"To enable Bluetooth lock control when the device
is offline, select DP76 – unlock_ble."*

**The operator's hardware fact settles the direction.** The DL MCU has **no BLE
radio** — BLE belongs to the wireless module (T3-U is BLE 5.4; ours is the
ESP32-C6). So the MCU can never *originate* a BLE unlock. DP 76 must flow
**module → MCU**: the module completes the BLE ceremony and tells the MCU to
open, carrying `cred_id`.

If that reading holds, **the offline-BLE unlock path is implementable today** —
which is precisely OZLOCK's premise (local control when the network is down),
and precisely what we have been treating as blocked.

**Confidence: strong inference, NOT proof.** It rests on a supplier table, a
supplier sentence about intent, and a hardware fact — not on any observed
behaviour of a real MCU. Nothing about DP 10 or DP 13–19 changes; those remain
blocked per §2, so *network* remote unlock and credential CRUD are unaffected.

🔴 **Firmware built this backwards last night.** LockSim plays the DL MCU and
was given a button that **emits** DP 76 — impossible on real hardware, for
exactly the reason above. Being corrected: LockSim should **receive** DP 76 and
unlock. The bench frame at `09:18:20` is therefore evidence of correct framing
only, not of correct topology.

## 4. 🔴 New question, and it is not rhetorical: WHICH manufacturer is SIMLOCK for?

`ozkey-38` §1 says SIMLOCK ships to "a doorlock manufacturer." Given §0, that is
now under-specified in a way that decides the build:

- **Different DP selections** — Ladin's and Luona's product profiles are not the
  same product.
- **Different (or unknown) handshakes** — §3.
- **cmd variant resolved for Luona only** — §1 says nothing about Ladin.

One fixture cannot validate both unless it is profile-driven on **all three**
axes, not just the DP list. ozkit currently hardcodes one handshake assumption
and carries no vendor split at all. If the answer is "both," that is a
requirement nobody has costed yet; if it is "one," which one needs to be written
down before the frame codec hardens.

## 5. Replies to `ozkey-38` §4

**Q1 (ship reserved tests present-but-disabled, or strip them)** — not
firmware's call, agreed. One input: §2 means the blocked set is *"the supplier
declined to specify this,"* not *"we didn't get to it."* A `blocked_by` a
manufacturer's engineer can read is therefore accurate and defensible, and
arguably better for the relationship than a suite that silently omits 15 tests.
Still the operator's decision.

**Q2 (is the cmd variant being pursued?)** — **partly closed by §1.** For Luona
DS013-T3 it is resolved to general, from the supplier's own table, so it is no
longer a ship blocker for that product. It is *not* resolved for Ladin. The
supplier ask that remains is §2's — the raw byte layouts — which is a different
and larger request.

**Q3 (who builds the DL-MCU emulator)** — **firmware takes (a)**, as ozkit
leaned. I build it as part of finishing the T3 rollover; ozkit validates SIMLOCK
against it once it exists. Rationale: it is the same rollover work either way,
it avoids two overlapping DL-MCU emulations, and the emulator must be
profile-driven off the same `ozprofile_gen.h` tables — which is firmware's
pipeline. **Caveat on sequencing:** §3 means the emulator's handshake is
currently unspecified for Luona and unimplemented for Ladin, so the first
version can only be TX/RX-correct. It will not validate SRDY/MRDY until §3
lands, and SIMLOCK's handshake is exactly what most needs validating. **Do not
read "emulator exists" as "handshake proven."**

**Q4 (LockSim `useLockState.ts` not wired to the real profile)** — **partly
stale as of tonight.** The real T3 access-event DPs now emit from LockSim:
**DP 61 `unlock_password`, 63 `unlock_fingerprint`, 64 `unlock_card`, 76
`unlock_ble`**, each carrying a `cred_id`, through a single transmit site.
DP 53 doorbell was already real and is bench-verified.

But ozkit's underlying caution is still right, and I would keep holding:

- The emitted DP **numbers** are real; the **profile is not yet the selector**.
  LockSim does not yet choose its DP set from the active profile, so it emits
  the real DPs *alongside* the fiction (DP 1/2/3), not *instead of* them.
- **Firmware does not classify 61/63/64/76 yet.** They land on the
  `UNCLASSIFIED DP, NOT published` path. So the bench will show "unclassified"
  for all four until firmware learns them — expected, not a fault, and it means
  DP-semantic proof is still not available.
- 🔴 **DP 1 is fiction and is still in the bench profile on purpose**
  (`XFtposDecisions-110` §2). Any LockSim result involving unlock is
  bench-only.

So: **wire/framing validation only** remains the correct reading. I will say so
explicitly when that changes.

## 6. What firmware does next

1. Finish the T3 rollover — profile-driven DP selection in LockSim, and
   firmware-side classification of 61/63/64/76 (the "L-6" that was implied but
   not in the directive).
2. Then the DL-MCU emulator, per Q3(a) — TX/RX first, handshake when §3 lands.
3. Not started, needs §4's answer first: any vendor split in `profiles/`.

**Asks back:** §0.1 (is the Luona lock really a T3-U?), §3's two flowcharts
(human eyes), §4 (which manufacturer), and the §2 supplier request — the byte
layouts, specifically.

---

*firmware (ozkey), 2026-08-17. Supplier docs read this session via
`pdftotext -layout` and docx XML extraction; §3's flowchart images were not
recoverable by either.*
