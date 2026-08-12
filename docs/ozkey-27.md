# ozkey-27 — The OZKIE Protocol

**Status: 🟡 PROPOSAL, FOR REVIEW. Nothing here is built. Nothing here is
decided.** Written 2026-08-12 by firmware, at the system architect's request,
as the discussion artifact for **three teams**: firmware, server
(`ozlockserv`/`ozlodgeserv`/`ozpmsserv`), and app (ftpos — BANOI/MAOI).

Source material read for this document:

- `docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf` — Tuya
  低功耗通用串口接入协议 for **Smart Lock DS013-T3**, PID `vr4iiuqtyh0q4nix`,
  generated 2026-08-11. The first real supplier DP list we have ever held.
- `docs/DPSuppliers/T3_Final_Customer_Version_EN.docx` — the **T3-U module**
  customer integration document. Not a second supplier: a different *layer* of
  the same solution. See §2.0.
- `blelock/common/ozdoorlock_core.h`, `blelock/common/oztime.h`,
  `locksim/lib/tuya.ts` — what we actually implement today.
- `docs/ozlodge_v2.2.pdf` — the commercial-tier topology this protocol has to
  survive (app → local server → cloud → NEXUS).

---

# 1. Why this document exists

Today "the protocol" is three different things that happen to interoperate:

- **App ↔ lock** — a sealed envelope carrying a *Tuya DP frame*, hand-built by
  the app, with DP numbers baked into `envelope.dart` and into
  `ozdoorlock_core.h`.
- **Server ↔ lock** — `payload_hex`: the same Tuya frame, relayed verbatim.
- **ESP32 ↔ DL MCU** — the real Tuya serial protocol.

So the *lock manufacturer's wire format has leaked all the way up into the
phone*. Every party in the system speaks Tuya. That was survivable while there
was one lock and we made the DP numbers up. It stops being survivable the
moment there are a dozen suppliers, four servers and two apps.

**The proposal: OZKIE is a JSON verb protocol spoken by every party, and the
translation to Tuya DP happens in exactly one place — the OZKIE MCU (our
ESP32) — driven by a per-PID profile that is data, not code.**

---

# 2. 🔴 What the supplier documents actually say

## 2.0 The two documents are different LAYERS, not two suppliers

| | `T3_Final_Customer_Version_EN.docx` | `protocol_vr4iiuqtyh0q4nix…pdf` |
|---|---|---|
| What it is | **Module** integration guide — **T3-U / T3-U-IPEX** | **Serial DP protocol** for one lock PID |
| Layer | the Tuya radio module — **the part WE REPLACE** | the ESP32↔MCU wire — **the part WE MUST SPEAK** |
| Silicon | Beken **BK7236** (`bk7236_video_lock_ty`), 320 MHz, 4 MB flash, 640 KB SRAM, Wi-Fi 6/4 + BLE 5.4, ¥30 debug | n/a |
| Category | **Video Lock — AI High-end Visible Intercom Door Lock** | **Smart Lock — DS013-T3** (no video) |
| Protocol named | *MCU General Interface Protocol* | *低功耗 (low-power) general serial access* |

The `-T3` in "Smart Lock DS013-T3" is the module: **DS013 is a lock board built
around the T3 module.** So the two files describe the same solution from
opposite ends of the UART.

**What the module doc is worth to us:**

- It is the datasheet for the thing our ESP32-C6 displaces. Keep-alive
  **150–200 µA @ DTIM10** is the number our FTD `rx_on=1` Thread design is
  competing with — directly relevant to the **UNMEASURED** battery rows
  (ozlodge v2.2 **C9/L7**). *(DP 156 `wifi_dtm20`/`wifi_dtm10` in the PDF is
  the same DTIM knob — the two documents corroborate.)*
- Pin-compatible with the older **WXU** module, which is the footprint
  constraint on our replacement board (`doorlock-pcb-spec`).
- 🔴 It names the **four documents we are missing** — see §2.6 and §7 Q2.

## 2.0.1 The wake-up handshake — and why we do NOT inherit it

§2.2 of the module doc specifies a **two-wire GPIO wake scheme**:

| Direction | Signal |
|---|---|
| Module → MCU | **300 ms HIGH pulse on P32** |
| MCU → module | **≥1 ms HIGH pulse on P13, rising-edge triggered** — *or* UART wake on MCU TX data |

> 🟢 **CORRECTED 2026-08-12, system architect.** An earlier draft called this a
> hard requirement on us. **It is not.** That scheme exists because the Tuya
> module is a **SLAVE** on that UART. **We are specified as CO-MASTER, on a
> 5-wire interface: `TX / RX / SRDY / MRDY / GND`** — and *"we can demand the
> change"*: we are the customer, and the lock-board interface is ours to
> specify.
>
> **Either side can wake the other** — we wake the DL MCU, the DL MCU wakes us.

The whole asymmetry in the Tuya documents follows from slave-hood, and it all
goes away under co-master:

| Under Tuya slave (their docs) | Under co-master (ours) |
|---|---|
| Timing-based wake pulses (300 ms / ≥1 ms) — must be characterised, races on the first byte | **Level handshake on SRDY/MRDY.** Deterministic; no pulse widths to tune, no lost first byte |
| Module polls, MCU reports. 1 s timeout, 3 retries | **Either side initiates, any time** |
| MCU **power-cycles the module** when it fails to answer (低功耗 §5/§6: 强制给WIFI模块断电) | no such regime — a stall is a de-asserted ready line, not a reset |
| Credentials **pulled** by the MCU (`0x13`, §2.3) | **push OR pull.** Push on arrival for latency, pull to reconcile — exactly §4.4 R2 |
| The module must stay listening | **both sides may sleep** — which is what makes the C9/L7 battery question answerable at all |

So §2.3's pull-inversion is a *capability we keep*, not a constraint we submit
to, and the wake scheme above is **the fallback** if a given supplier refuses
the 5-wire change — not the baseline.

Still true regardless: **User UART0** is the MCU link; **UART1 is the log
port** (TX1 = module pin 7 → IC P0/pin 18, RX1 = module pin 6 → IC P1/pin 17).
Worth lining up against the ROM-fixed-UART0 problem in `doorlock-pcb-spec`.

## 2.0.15 The hardware already commits to co-master — `DLBASIC V1.0`

`docs/DoorLockHW/DLBASIC_V1.0_Combined.pdf` (ESP32-C6-MINI-1-N8, minimal
variant, 42 × 23 × 10 mm pocket). **J2 is exactly the interface in §2.0.1:**

| J2 pin | Net | ESP32-C6 | Note from the spec |
|---|---|---|---|
| 1 | GND | — | |
| 2 | TX | GPIO0 | free — no bootloader conflict on this variant |
| 3 | RX | GPIO1 | " |
| 4 | **SRDY** | **GPIO2** | **LP_GPIO — deep-sleep wake-capable**, R4 10 k pull-up |
| 5 | **MRDY** | **GPIO7** | "plain GPIO, **no wake requirement**" |

Programming is fully isolated on a 6-pad POGO cluster using the true ROM UART
(GPIO16/17), so J2 never doubles as the flashing interface — that closes the
unplug-before-flashing trap the Premium variant had, and it is the fix to the
ROM-fixed-UART0 problem in `doorlock-pcb-spec`.

**This settles a question in §2.0.2 before it is asked.** J2 is *not* a
WXU/T3-compatible module footprint — it is our own 5-wire connector into the
lock MCU. We are not dropping into the Tuya module's socket, which is precisely
why co-master is available to us: **the P32/P13 slave wake scheme has no
landing pad on this board at all.** "We can demand the change" is already
built.

### 🔴 Three engineering points, while the board is still open

**(a) Wake convention: pulse vs level does not matter — make it a profile
field.**

> **CORRECTED 2026-08-12, system architect: *"whether they use pulse or H/L
> signal for waking up does not matter, ESP32-C6 can detect both."*** An
> earlier draft argued SRDY had to be level-held or the wake could be missed.
> That is wrong, and the HW spec §2.2 already said so: EXT1 latches in the RTC
> domain, `ANY_HIGH`/`ANY_LOW` are a firmware config, and a pulse of a few
> hundred ms registers reliably against a level-configured input. **No hardware
> risk either way.**

The residual distinction is not about waking; it is about what the line means
**while both sides are awake**:

| | wake | in-session |
|---|---|---|
| **Level-held** | ✅ | doubles as back-pressure — "still have something for you". A dropped byte self-recovers: the line is still asserted, so we re-poll |
| **Pulse** | ✅ | edge-only, carries no state — recovery needs its own timeout/retry, i.e. the regime we left behind with Tuya's slave model |

A mild preference for level-held on the handshake, and nothing more. Since a
dozen suppliers may each arrive with their own convention, **this belongs in
the per-PID profile alongside the DP map (§4.5), not in a demand**:

```json
"link": {
  "srdy": { "pin": "GPIO2", "assert": "low",  "style": "level" },
  "mrdy": { "pin": "GPIO7", "assert": "low",  "style": "level", "pulse_ms": null }
}
```

Firmware implements both styles once and reads which to use. That closes HW
spec §8's "confirm SRDY/MRDY wake convention" as **"we accept either — tell us
which, we configure"**, which is a far cheaper conversation with twelve
vendors than a specification handed down.

R4's 10 k pull-up on SRDY still argues for **idle-HIGH / assert-LOW** as the
default, mostly for the failure mode: an unpowered or unplugged lock MCU idles
the line safely rather than asserting it.

**(b) ⚠️ MRDY has no pull resistor, and it should.** R1/R2/R3/R4 cover EN,
GPIO8, GPIO9 and SRDY. **GPIO7 has none**, so MRDY floats through every ESP32
reset and boot — during which the DL MCU may read it as an assertion or a
glitch. Every time we reboot we would be poking the lock MCU. One 0402 to its
idle level, added now, costs nothing; found later it is a respin. *This is the
one thing in the HW spec I would change.*

**(c) Deep sleep + no 32.768 kHz crystal meets the credential clock.** The
spec notes "no external 32.768 kHz crystal", so deep sleep runs on the internal
RC. We are the calendar authority for the DL MCU (`ozkey-21` T2/T3), and
`ozkey-21 §9` established that a wrong clock is a live security defect, not a
cosmetic one. If both sides now sleep (§2.0.1), **RC drift across a sleep
interval lands directly on credential expiry windows.** Needs a stated drift
budget and a rule — my instinct is the MCU re-requests time on every wake
rather than trusting either side's free-run. See Q13.

*(Power, for the C9/L7 rows: AP63203 Iq is 22 µA, so the board's floor is
~22 µA + a slept C6, comfortably under the T3 module's 150–200 µA keep-alive.
**The hardware can hit a low floor; it is the FTD `rx_on=1` firmware choice
that spends it.** That is the real shape of the battery answer.)*

## 2.0.2 🔴 Co-master is leverage — spend it on the transport, not the semantics

"We can demand the change" is real, and at launch volumes
(`ozkey-retail-economics`: 20,000 locks) it has teeth. But it is not free, and
**the bill lands on the axis we just found was working in our favour.**

§3 establishes that the DP catalogue is a Tuya *standard* — which is the only
reason a dozen suppliers is tractable at all. Every custom thing we demand
becomes a **per-supplier firmware fork on their side**, and a dozen forks is a
dozen bug tails. Demanding a bespoke protocol would spend the standardisation
we just discovered we had.

So split the ask by what it costs *them*:

| Ask | Their cost | Recommend |
|---|---|---|
| **5-wire co-master transport** (`TX/RX/SRDY/MRDY/GND`) | wiring + a ready-line state machine. No application logic. Testable in an afternoon | ✅ **demand it** |
| **Keep the standard DP catalogue** for everything it already covers | zero — they keep their Tuya MCU SDK, handlers and tests | ✅ **keep** |
| **One vendor `raw` DP that tunnels OZKIE** — bonds, sealed envelopes, `counter_floor`, everything the catalogue has no concept of | one more `raw` DP handler, a shape their MCU already implements a dozen times | ✅ **demand it** |
| Replace the DP layer with native OZKIE on the UART | new protocol stack per supplier, new tests, new bug tail ×12 | ⚠️ **the clean option — see below** |

The first three get ~90% of a native-OZKIE UART for a few percent of the
supplier effort, and they keep the multi-supplier story tractable. That is my
recommendation.

⚠️ **But if we go native, one thing in §4 has to change: not JSON on the UART
leg.** §4.2 specifies JSON, which is right for app↔server↔lock where parsers
are free. Pushing JSON down to a lock-board MCU with tens of KB of RAM is a
cost we would be imposing on twelve vendors for no benefit. **The OZKIE verb
set should have two encodings of one model** — JSON on the network, a compact
TLV on the UART. Same verbs, same `id`, same semantics, different bytes. That
is a flaw in the current §4 draft, and it bites the moment co-master tempts us
into pushing OZKIE all the way down.

## 2.1 The DP map we use is invented, and it collides

`DS013-T3`'s allocation against ours:

| DP | OZKIE today (`locksim/lib/tuya.ts`, `ozdoorlock_core.h`) | DS013-T3 actual |
|---|---|---|
| 1 | `UNLOCK_CHANNEL` — PIN entry out, remote unlock in | *not allocated* |
| 2 | `RFID_CARD` raw | *not allocated* |
| 3 | `FINGERPRINT` bool | *not allocated* |
| 5 | `BATTERY_ALARM` bool | *not allocated* |
| 8 | `ACCESS_RESULT` enum granted/denied/expired | *not allocated* |
| **21** | **`ADD_TEMP_PIN`** `[slot 2B][PIN][from 4B][to 4B]` raw | **导航音量 navigation volume**, ENUM `mute/low/normal/high` |
| **22** | `DELETE_PIN` `[slot 2B]` | *not allocated* |
| **23** | **`ADD_TEMP_RFID`** | **自动落锁 auto-lock**, BOOL |
| **24** | `DELETE_RFID` | **落锁延迟 auto-lock delay**, VALUE 5–1800 s |
| **60** | `PAIRING_REQUEST_PROPOSED` — "only LockSim emits it" | **门锁告警 alarm**, ENUM, 18 values incl. `pry`, `low_battery`, `unlock_attempt`, `hijack` |
| 101/102/103 | `bond_revoke` / `invite_cancel` / `list_bonds` — in-lock, never forwarded | *manufacturer space, unallocated here* — these three remain safe |

And what DS013-T3 *does* allocate that we have no concept of:

| DP | Function | Type |
|---|---|---|
| 9 | configure password-free remote unlock | raw 128 |
| **10** | **remote unlock** | raw 128 |
| 11 | connection mode `keep/sleep/lock_keep/lock_sleep` | enum |
| **13/14/15** | add / delete / modify **large-capacity unlock method** (fingerprint, card) | raw 128 |
| **16/17/18** | add / delete / modify **large-capacity password** | raw 128 |
| **19** | **sync** large-capacity unlock methods | raw 128 |
| 42 | Bluetooth control switch | bool |
| **45** | **battery level** −1..100 | value |
| 47 | latch/bolt state | bool |
| 52 | door opened from inside | bool |
| 53 | doorbell | bool |
| 54 | device info | raw |
| **61/63/64/69/72/73/76** | unlock **records** — password / fingerprint / card / temp-password / remote / remote-voice / Bluetooth. **Value = the credential's ID** | value |
| 74 | combination unlock record (large capacity) | raw |
| **86/87/88/89** | offline-password param issue / single clear / all clear / offline unlock report | string 255 / raw |
| **98** | **duress (胁迫) alarm** | bool |
| 156 | Wi-Fi connection strategy | enum |

### Consequences, stated plainly

1. **`ozDpForwardable()` is inverted on real hardware.**
   `ozdoorlock_core.h:2009` allows `dp == 1 || (dp >= 21 && dp <= 24)`. On a
   DS013-T3 that allow-list permits the *settings* DPs (volume, auto-lock,
   delay) and permits nothing that grants or revokes access. Every credential
   we have ever "provisioned" would be a settings write or a type/length
   rejection.

2. **DP 60 is the alarm channel, and we open the BLE window on it.**
   `ozdoorlock_core.h:1808` calls `openBleWindow()` on inbound DP 60. On this
   supplier that fires on `pry`, `low_battery`, `unlock_attempt`,
   `network_error` — 18 unrelated events. It is gated on `provisioned`, and
   since `doorlock-1.57` the *primary* gesture is the failed-entry path, so
   this is an unintended trigger rather than a new exposure class. **It must be
   deleted before our firmware meets a real MCU.** The in-code comment "no
   shipping DL MCU emits one; only LockSim does" is true of LockSim and false
   of the supplier — `ozkey-22 §7`'s allocation request is answered, and the
   answer is *that number is taken*.

3. **`ACCESS_RESULT` (DP 8) does not exist.** Our entire door-event path — the
   `granted`/`denied`/`expired` publish, the bolt mirror, *and the failed-entry
   pairing gesture that `doorlock-1.57`/`1.58` are built on* — hangs off a DP
   this supplier never allocated. Real unlock reporting is one DP **per
   credential class** (61/63/64/69/72/73/76), each carrying the credential ID,
   with failures arriving separately on the DP 60 alarm enum.

4. **LockSim has been validating us against our own fiction.** This is why
   `ozkey-21 §8.3` could never be answered at the bench: we were asking our own
   emulator whether it agreed with us. Every ✅ in the credential path of the
   ozlodge v2.2 status register (**L2 "DP 21/22 path, bench-verified"**, **L4
   "revocation is lock-side (DP 22 / DP 101)"**) is verified against the
   emulator only, and L2's DP numbers are now known wrong.

## 2.2 There are two command-word variants in play

> **Softened 2026-08-12 after reading the T3 module doc.** An earlier draft said
> flatly that we implement the wrong variant. Two of three signals now point at
> the **general** protocol — which is what we implement — so this is an
> unresolved ambiguity to close with the supplier, not a confirmed defect.
>
> | Signal | Points at |
> |---|---|
> | DP PDF title + body (§5 `0x05`, §6 `0x08`, §7 `0x09`) | low-power |
> | DP PDF's own final 功能协议 table (`0x06`/`0x07`) | **general** |
> | T3 module doc §3.3, naming *"MCU General Interface Protocol"* | **general** |
>
> If it is general, §2.2 costs us nothing and we were right by luck. If it is
> low-power, every command word below has to change. It is a one-question fix
> and it gates real work, so ask before building (§7 Q1).

This PDF is the **低功耗 (low-power)** serial protocol. We implement the
**general** one.

| Function | Ours (general) | This doc (low-power) |
|---|---|---|
| DP issue, module → MCU | `0x06` | **`0x09`** |
| DP report, MCU → module | `0x07` | **`0x05`** realtime / **`0x08`** record-with-time |
| get local time | `0x1C` | **`0x06`** |
| get GMT time | `0x0C` | **`0x10`** |
| product info | `0x01` | `0x01` ✅ |
| network status | `0x03`-ish | `0x02` |
| MCU OTA | — | `0x0c`/`0x0d`/`0x0e` |
| **cloud temp password, single** | — | **`0x11`** |
| **dynamic password verify** | — | **`0x12`** |
| **cloud temp password, multi-group** | — | **`0x13`** |

`forwardHexToMcu()` rejects anything where `frame[3] != 0x06`. Under low-power
semantics `0x06` from the module is not a DP write at all.

> ⚠️ **The PDF contradicts itself.** The body describes the low-power variant
> (§5 `0x05`, §6 `0x08`, §7 `0x09`). The final 通讯协议-功能协议 table lists
> 模块发送 `0x06` / MCU上报 `0x07` for *every* DP — the general variant. This
> is a **supplier question**, not something to infer. See §7 Q1.

## 2.3 🟢 The credential flow runs the other way — and that is good news

We push credentials at the MCU. This module family expects the MCU to **pull**.

**Cmd `0x13` — 请求云端临时密码 (multi-group).** MCU asks; the module answers
with the *entire* password set, up to 10 groups, each carrying:

- index `1..50` (transmitted with a +900 offset)
- password length, and the password itself in ASCII
- use count — `0` unlimited within the window, `1` one-shot
- status — `0` valid, `1` deleted at the panel
- **effective datetime, GMT** (y/m/d/h/m/s)
- **expiry datetime, GMT** (y/m/d/h/m/s)

And, quoted: *密码每次服务端都是全量下发，门锁需要每次根据服务端返回的所有密码
和状态进行更新* — the server sends the **full set every time**, and the lock
replaces its local state from it.

Three things fall out of this:

1. **It is idempotent full-state sync**, which is the operator's own standing
   ruling ("we ship idempotent STATE, so epoch+poll beats ARQ",
   `feedback-use-the-mechanism-that-exists`). The module protocol already
   implements the thing we were designing. §4.4 below adopts it wholesale.
2. **`ozkey-21 §8.3` is answered for this supplier: the MCU DOES hold a
   per-credential expiry window** — but only via a command we do not
   implement. Temp-PIN expiry is enforceable today, through `0x13`.
3. **`ozkey-21 §8.4` is confirmed by the supplier.** The doc states the lock
   must obtain **GMT** (cmd `0x10`) to run the password service. Serving UTC
   to the MCU was correct; do not "fix" it.

**Cmd `0x12` — 动态密码校验.** The MCU sends the typed password, its current
GMT, and all admin password groups; **the module returns pass/fail**. So for a
class of credentials *we are the authority*, in-door, offline of any network.
That is directly useful for OZLODGE guest PINs — see §6.3.

## 2.4 Offline record replay, which we do not model

Cmd `0x08` (记录型状态上报) buffers **up to 20 records on the MCU** when the
network is down and replays them on reconnect, oldest overwritten past 20. Each
carries a time-basis flag: `0` = no time attached, `1` = device local time,
`2` = GMT.

So door events are **backdated, may arrive out of order, and may duplicate**.
`publishLog()` today has no event id and no occurrence timestamp — only arrival
time at the server. The server cannot dedupe or order them. OZLODGE's **L8
audit-trail** claim depends on exactly this.

## 2.5 What this PDF does NOT tell us

The large-capacity credential DPs (13–19, 74) and the offline-password DPs
(86–89) are all `raw`/`string` with a max length and **no payload layout given
in this document**. We cannot write a codec for them.

## 2.6 🟢 The missing documents now have names

§2.5 says we cannot write a codec for the RAW credential DPs. The module doc's
§3.3 reference table **names the documents that contain them**, so this stops
being "we're stuck" and becomes a purchase-order-shaped request:

| Document | Why we need it |
|---|---|
| **MCU General Interface Protocol** | the authoritative command-word set — settles §2.2 |
| **Visible Intercom Door Lock DP Reference** — *"includes detailed content description for each DP point"* | 🔴 **this is the §2.5 blocker.** The RAW layouts for 13–19, 74, 86–89 |
| **General Interface Auxiliary Document for Visual Intercom** | *"specific data interaction logic and concrete protocol examples from each functional perspective"* |
| Log-capture guide | bench diagnostics |
| **T3-U Design Guide.zip** + T3-U / T3-U-IPEX datasheets | the wake-up timing and pinout in §2.0.1 |

⚠️ Note the two named DP documents are **Visible Intercom** (Video Lock)
documents, while DS013-T3 is a plain Smart Lock. Whether the catalogue subsets
differ enough to matter is §7 Q9.

Also from §3.3: *"Protocol Difference: **Command 0x84 is not supported**."*
`0x84` appears in neither document we hold — further evidence that the real
command set is larger than what we have, and that we are working from an
excerpt.

---

# 3. "95% the same" — the architect was right, and I was wrong

> ⚠️ **This section reverses an earlier draft of this document.** On the DP
> list alone I concluded that DP numbers are allocated per-PID by each
> manufacturer, and pushed back on the "95% the same" premise. The **T3 module
> document overturns that**, and the corrected version matters because it
> changes how much work §4.5 is.

The module doc's §3.1 walks through PID creation, and it talks about DPs like
this: *"select **DP76 – unlock_ble**"*, *"do not select **DP149**"*, *"**DP42**
controls whether Bluetooth transmission is enabled"*, *"do not select **DP212**
if audio/video entries should be hidden"*.

Cross-check those against the DS013-T3 PDF — a **different product, different
category**:

| DP | T3 module doc (Video Lock) | DS013-T3 PDF (Smart Lock) | |
|---|---|---|---|
| 42 | Bluetooth transmission enable | 蓝牙控制开关 Bluetooth control switch | ✅ same |
| 76 | `unlock_ble` | 蓝牙解锁 Bluetooth unlock | ✅ same |
| 212 | `initiative_message` (proactive push, raw) | not selected | consistent |
| 149 | "do not select" | not selected | consistent |

So the numbers carry **catalogue names** (`unlock_ble`, `initiative_message`)
and mean the same thing across two different products. The corrected model:

| Layer | Common? |
|---|---|
| Frame — `55 AA`, ver, cmd, 2-byte BE len, payload, sum-mod-256 | ✅ stable |
| DP unit — `[dpid][type][len 2B][value]` | ✅ stable |
| **DP numbers and semantics** | ✅ **a STANDARD Tuya catalogue, per product category.** The manufacturer *selects a subset* at PID creation — it does not invent numbers |
| Which subset is selected | ⚠️ per-product — this is the real "5%" |
| Command words | ⚠️ per-variant — see §2.2 |
| RAW payload layouts inside a DP | ❓ per category doc, and **we don't have it** (§2.6) |

**Two consequences, pulling in opposite directions:**

1. 🟢 **§4.5 is much cheaper than I first wrote.** Not N unrelated profiles —
   **one canonical catalogue profile, plus a thin per-PID subset/override
   file.** A new supplier is a short overlay, not a porting exercise. That is
   the architect's original instinct, and it holds.
2. 🔴 **The collision in §2.1 is WORSE, not better.** If 21/23/24/60 are the
   *standard catalogue*, then our invented numbers do not clash with one
   supplier — **they clash with every Tuya lock supplier there is.** There is
   no vendor we could have picked where our map would have worked.

The design rule is unchanged and, if anything, firmer: **no DP number in
firmware source.** But it is now firmware reading a published standard rather
than firmware absorbing a dozen dialects.

**We should obtain the category DP references and adopt the catalogue names
verbatim** (`unlock_ble`, not `BLE_UNLOCK`). Renaming a published standard is
how we got here.

---

# 4. The OZKIE protocol — proposed shape

## 4.1 Layering

```
  L4  APPLICATION      booking, roster, check-in        PMS / BANOI / MAOI
  ────────────────────────────────────────────────────────────────────────
  L3  OZKIE MESSAGE    verb + args, JSON                every party speaks this
  ────────────────────────────────────────────────────────────────────────
  L2  ENVELOPE         AES-256-GCM, AAD=device_id,      app ⟷ lock only;
                       counter_floor                     relays are blind
  ────────────────────────────────────────────────────────────────────────
  L1  TRANSPORT        BLE GATT | Thread UDP | MQTT |   interchangeable
                       HTTPS REST
  ────────────────────────────────────────────────────────────────────────
  L0  DEVICE PROFILE   OZKIE verb → Tuya DP frame       OZKIE MCU ONLY,
                       per-PID table, DATA not code      one place in the system
```

L2 and L1 exist today and are unchanged. **L3 is the new work. L0 is the new
work.** The single most important property: *the Tuya wire format stops at L0*.
No phone, no server, no bridge ever sees a DP number again.

## 4.2 Message shape

Request:

```json
{
  "v":    1,
  "id":   "01JB2R7QK8ZC3F4H5N6P7Q8R9S",
  "ts":   1786000000,
  "exp":  1786000600,
  "src":  "app:cd6cfe55…",
  "dst":  "lock:ozk-acebe639f8c4",
  "seq":  41,
  "verb": "cred.put",
  "args": { … }
}
```

| Field | Meaning |
|---|---|
| `v` | protocol version. Bump only on a breaking change |
| `id` | ULID. **Idempotency key** — the whole reliability story. Echoed in every response and every log line, end to end |
| `ts` | issuer's clock, **UTC epoch seconds, always** |
| `exp` | do not execute after. Replaces every ad-hoc TTL |
| `src`/`dst` | `role:identity`. Roles: `app`, `lock`, `bridge`, `pms`, `cloud`, `nexus` |
| `seq` | the existing per-bond `counter_floor`. Unchanged semantics |
| `verb` | `noun.action` — so dispatch is a table, not a switch |
| `args` | verb-specific. **Inside the sealed envelope** |

Response:

```json
{
  "v": 1, "re": "01JB2R7QK8ZC3F4H5N6P7Q8R9S",
  "verb": "cred.put", "ok": true, "code": "OK",
  "state_epoch": 17,
  "data": { "slot": 3 }
}
```

`code` is a stable string, never a bare number: `OK`, `DENIED`,
`BOND_EXPIRED`, `MEMBER_EXPIRED`, `REPLAY`, `NO_SLOT`, `UNSUPPORTED`,
`MCU_TIMEOUT`, `PROFILE_MISSING`, `NOT_PROVISIONED`.

## 4.3 The verb namespace (first cut)

| Verb | Direction | Notes |
|---|---|---|
| `lock.unlock` | → lock | maps to DS013-T3 DP 10 |
| `lock.state` | → lock | bolt (47), battery (45), door-inside (52) |
| `lock.settings.set` | → lock | volume (21), auto-lock (23), delay (24) — the **real** meanings of those numbers |
| `cred.put` | → lock | idempotent upsert. `kind: pin \| rfid \| fp \| passport` |
| `cred.delete` | → lock | by `cred_id`, not by slot |
| **`cred.sync`** | → lock | **full-state replace.** The heart of §4.4 |
| `cred.list` | → lock | returns `{cred_id, kind, from, to, status}[]` |
| `bond.invite` / `.accept` / `.revoke` / `.list` | → lock | today's DP 101/102/103, unchanged semantics |
| `time.set` / `time.get` | ↔ | UTC only, on the wire and to the MCU |
| `net.provision` / `net.reset` | → lock/bridge | `pan_id` stays a 4-char **hex string** (ozkey-26 §5.1) |
| `device.info` | → lock | adds `pid`, `profile_id`, `profile_rev` |
| `device.ota` | → lock | ESP32 today; MCU OTA via `0x0c`–`0x0e` later |
| `event.access` | ← lock | `{result, kind, cred_id, occurred_at, time_basis}` |
| `event.alarm` | ← lock | the DP 60 enum, mapped to stable strings |
| `event.duress` | ← lock | DP 98. **Must not be collapsed into `event.alarm`** — it needs a different escalation path |
| `event.battery` | ← lock | level, not just "alarm" |
| `event.doorbell` | ← lock | DP 53 |

## 4.4 The three rules that make this survive four hops

**R1 — Every verb is idempotent and carries `id`.** Replay is free, so any
party may retransmit without coordination. This is what lets us keep *not*
building ARQ.

**R2 — State is versioned, and reconciled by full replace.** Each lock keeps a
monotonic `state_epoch`, bumped on any credential/bond change, and reports it
in every response and heartbeat. A party that sees an epoch it does not
recognise issues `cred.sync` with the complete intended set; the lock replaces
its state and returns the new epoch. **Poll beats ACK.** This is not our
invention — it is what cmd `0x13` already does at the MCU boundary (§2.3), so
the same shape runs at both ends of the stack.

**R3 — Relays are content-blind and MUST NOT interpret `args`.** Only the
routing header (`v`, `id`, `ts`, `exp`, `src`, `dst`, `verb`) travels outside
the envelope; `args` travels inside it. A relay may route and rate-limit on the
header. It may not read, log, or rewrite `args`. This is what makes NEXUS,
`ozlockserv` and the on-prem server *the same kind of thing* — a courier — and
it is mechanically enforced, not promised.

> ⚠️ R3 has a cost worth naming: **`verb` in the clear is metadata**. A relay
> learns that a credential was granted for a door, and when — just not to whom
> or what. That is a deliberate trade (relays need it to route and rate-limit)
> and it should be stated in the sovereignty claims rather than discovered by a
> reviewer. See §7 Q5.

## 4.5 L0 — the device profile

A profile is a JSON file, one per PID, shipped alongside firmware and
selectable at provisioning:

```json
{
  "profile_id": "tuya-ds013-t3",
  "pid": "vr4iiuqtyh0q4nix",
  "rev": 1,
  "variant": "lowpower",
  "cmd": { "dp_issue": "0x09", "dp_report": "0x05", "dp_record": "0x08",
           "time_local": "0x06", "time_gmt": "0x10",
           "pw_pull_multi": "0x13", "pw_verify": "0x12" },
  "verbs": {
    "lock.unlock":        { "dp": 10, "type": "raw",   "codec": "unlock_v1" },
    "lock.settings.set":  { "volume": {"dp":21,"type":"enum","map":{"mute":0,"low":1,"normal":2,"high":3}},
                            "autolock": {"dp":23,"type":"bool"},
                            "autolock_delay": {"dp":24,"type":"value","min":5,"max":1800} },
    "cred.put.pin":       { "dp": 16, "type": "raw", "codec": "bulk_pw_v1" },
    "cred.put.rfid":      { "dp": 13, "type": "raw", "codec": "bulk_method_v1" },
    "cred.sync":          { "service": "pw_pull_multi" }
  },
  "events": {
    "61": {"verb":"event.access","kind":"pin"},
    "63": {"verb":"event.access","kind":"fp"},
    "64": {"verb":"event.access","kind":"rfid"},
    "69": {"verb":"event.access","kind":"temp_pin"},
    "72": {"verb":"event.access","kind":"remote"},
    "76": {"verb":"event.access","kind":"ble"},
    "45": {"verb":"event.battery"},
    "47": {"verb":"lock.state","field":"bolt"},
    "53": {"verb":"event.doorbell"},
    "98": {"verb":"event.duress"},
    "60": {"verb":"event.alarm","enum":{"0":"wrong_finger","1":"wrong_password",
           "2":"wrong_card","3":"wrong_face","4":"tongue_bad","5":"tongue_not_out",
           "6":"unclosed_time","7":"unlock_attempt","8":"key_in","9":"too_hot",
           "10":"low_battery","11":"wrong_finger_vein","12":"wrong_hand",
           "13":"stay_alarm","14":"pry","15":"network_error","16":"network_recovery",
           "17":"system_lock"}}
  }
}
```

Rules for L0:

- **No DP number in `.h`, `.dart`, or `.js`.** Ever. A grep for `dp == ` outside
  the profile loader is a defect.
- Unknown DP in → `event.unknown`, logged locally with dpid + length only,
  never published with payload (the rule `ozdoorlock_core.h:1818` already
  established — keep it).
- Unmapped verb out → `UNSUPPORTED`, never a blind forward.
- `device.info` reports `profile_id`+`rev` so server and app can tell what a
  lock can actually do, per lock, at runtime.

**LockSim becomes a profile-driven emulator.** It loads the same JSON and
emulates *that* MCU. Its current five DPs are our invented ones and should be
retired with the map they came from.

---

# 5. What changes, per team

**Firmware (us)**
1. Delete the DP 60 pairing handler. Re-home the pairing gesture — §7 Q3.
2. Replace the `ozDpForwardable()` allow-list with profile-driven dispatch.
3. Implement `0x13` full-set password service and `0x12` verify.
4. Implement `0x08` record replay: carry `occurred_at` + `time_basis` into
   `event.access`, and stamp an `id` per event so the server can dedupe.
5. Profile loader + `tuya-ds013-t3.json`, blocked on §7 Q2.

**Server**
1. Stop treating `payload_hex` as the interface. Route on the OZKIE header,
   relay `args` sealed and untouched (R3 — already the C2 posture, now
   structural).
2. Store `state_epoch` per lock; reconcile with `cred.sync` instead of trusting
   delivery. `likely_delivered` becomes a hint on top of epoch truth, not the
   evidence (ozkey-26 §5.2).
3. Dedupe door events on `id`; order on `occurred_at`, not arrival.
4. Serve `time.set` in **UTC**, unchanged.

**App (ftpos — BANOI/MAOI)**
1. **Stop building Tuya DP frames.** `envelope.dart` seals an OZKIE message; it
   no longer knows what a dpid is. This is the largest single change and the
   one that pays for itself across a dozen suppliers.
2. Read `device.info.profile_id` and grey out what a given lock cannot do,
   rather than assuming a uniform feature set.
3. Render `event.duress` distinctly from `event.alarm`.

---

# 6. Look-ahead: OZLODGE / OZPMS, and the four-party path

`ozlodge_v2.2` §2.1 has, for one guest check-in:
**BANOI → NEXUS → on-prem local server → bridge → lock**, with MAOI PMS on the
same LAN as the local server. OZKIE has to be one protocol across all of it.
It can be — R1/R2/R3 were chosen for exactly this. But the spec as written has
five things to resolve.

## 6.1 🔴 The trust model forks, and v2.2 does not say so

§3.1.2 claims *"the public server never obtains these key secrets… it is
mathematically impossible for the cloud vendor, a subpoenaed server, or a
malicious employee to decrypt control envelopes or forge commands."*

§4.2.2 then says *"the local server generates a unique time-bound digital
passport (X25519 member bond)."*

**If the local server mints the guest's member bond, the local server holds a
key that opens the door.** Both statements can be true only if "public server"
is read narrowly as "the cloud relay" and the on-prem server is understood as a
*credential issuing authority* — a fundamentally different role from the
residential model, where only the owner's phone can mint.

That is a legitimate design for hospitality — a hotel *is* the authority for
its own doors, and there is no owner-phone in the loop at 2am. **But it is a
fork, not an inheritance**, and C1/C4/L1 must not be presented as covering it.
Concretely, hospitality needs its own claim row: *"the on-prem server is a
credential authority; the cloud and NEXUS are not."*

This is the single most important thing in this document to rule on, because
the whole OZKIE authority table (§6.2) derives from it.

## 6.2 Authority is a table, and it must be explicit

Who may **originate** which verb, per tier. Proposed starting point — the `?`
rows are the operator's call:

| verb | owner app | member app | on-prem PMS | cloud relay | NEXUS |
|---|---|---|---|---|---|
| `lock.unlock` | ✅ | ✅ (in window) | ? | ✗ | ✗ |
| `cred.put` | ✅ | ✗ | ✅ hospitality / ✗ residential | ✗ | ✗ |
| `cred.sync` | ✅ | ✗ | ✅ hospitality | ✗ | ✗ |
| `bond.revoke` | ✅ | ✗ | ? | ✗ | ✗ |
| `net.reset` | ✅ | ✗ | ? | ✗ | ✗ |
| `lock.settings.set` | ✅ | ✗ | ✅ | ✗ | ✗ |
| `event.*` (receive) | ✅ | own only | ✅ | ✗ blind | ✗ blind |

The lock enforces this from the bond role in the envelope — the same mechanism
that already enforces owner-vs-member. No new machinery, one new table.

## 6.3 The guest-arrival gesture is unsolved for hotels

§4.2.2: *"the guest wakes the lock by touching the keypad and performs an
instant local BLE unlock."* Since `doorlock-1.57` the BLE window opens on a
**failed** entry attempt, for the privacy reason recorded in
`ozdoorlock_core.h:1747`. Asking a hotel guest to deliberately mistype a PIN to
make their door discoverable is not a shippable arrival experience.

Two options, and they are not exclusive:

- **(a) Use the supplier's real credential path instead of BLE.** Push the
  guest PIN via `cred.sync`/`0x13`. The guest just types it. No window, no
  gesture, no BLE, works with a dead phone — and per §2.3 the MCU enforces the
  expiry itself. **This is the option I'd recommend for hospitality**; it also
  makes the kiosk-printed-PIN flow in §4.1.2 the *primary* path rather than the
  fallback.
- **(b) Keep BLE for in-stay convenience**, entered through (a) — the guest's
  first PIN entry is a *successful* one, which we could allow to open the window
  for a bounded post-check-in period without recreating the presence leak,
  because it is scoped to a booking's first use rather than to every entry.

## 6.4 Naming: v2.2 is written against a dead name

The spec says **OZKEYSERV** throughout for the on-prem local server (§2.1.1,
§2.2.3, §4.1.1, §4.2.2). Per `product-topology-and-names`, `OZKEYSERV` is a
retired name and the current topology is **OZLODGE** on-prem, **ozlodgeserv**
our global cloud, **ozlockserv** residential cloud. Three teams reading one
spec with a fourth name for a component is a coordination hazard. Recommend a
naming pass on v2.3.

Also §2.1.2: *"OZLOCKSERV acts as a STUN/MQTT conduit"*. STUN is NAT traversal
for peer-to-peer media; it is not a message relay and we do not implement one.
What we built is an MQTT relay. Recommend dropping "STUN".

## 6.5 Two claims in the status register need re-marking

- **L2** *"PIN stored on lock MCU, works offline — DP 21/22 path,
  bench-verified"* — the DP numbers are wrong (§2.1) and the bench is our own
  emulator (§2.1 consequence 4). The *capability* may well hold via 16/17/18 +
  `0x13`; the *evidence* does not. Recommend ⚠️ pending real-MCU test.
- **L4** *"revocation is lock-side (DP 22 / DP 101)"* — DP 101 is ours and is
  genuinely verified. DP 22 is not this supplier's. Split the row.

Additionally, §3.2.1's *"the cloud… stores no transaction logs, no user names,
and no plaintext credentials"* is true of `ozlockserv` after the ozkey-23 §5(c)
scrub, but `ozlodgeserv` still stores plaintext PINs (the XF-47 exemption whose
premise is void). The claim should be scoped per server, not stated of "the
cloud".

---

# 7. 🔴 Open questions

**To the supplier (add to the ozkey-22 manufacturer list):**

- **Q1.** Which serial variant does the DS013-T3 MCU actually speak — low-power
  (`0x09` issue / `0x05`,`0x08` report) as the body of the protocol document
  describes, or general (`0x06`/`0x07`) as its own final 功能协议 table lists?
  They disagree.
- **Q2.** 🔴 **Blocking.** The RAW payload layouts for DP 13/14/15 (unlock
  methods), 16/17/18 (passwords), 19 (sync), 74 (combination record), and
  86–89 (offline passwords). The document gives type and max length only. We
  cannot write a codec without these.
- **Q2b.** Does this MCU implement `0x13` (multi-group cloud password) or only
  `0x11` (single)? The document flags `0x11` as legacy-firmware-1.0.
- **Q2c.** Does the MCU have its own RTC + backup cell, or does it depend
  entirely on the module for calendar time? *(This is `ozkey-21 §8.3`, still
  open — but §2.3 above narrows it: whatever the clock source, the expiry
  fields exist in the `0x13` contract.)*
- **Q2d.** Is the 20-record offline buffer (cmd `0x08`) per-lock configurable,
  and what is the behaviour past 20 — confirmed oldest-overwritten?
- **Q2e.** 🔴 **Send the four documents named in the T3 module doc §3.3** (§2.6),
  above all the **Visible Intercom Door Lock DP Reference** — it is the one
  holding the RAW payload layouts that block Q2. Also the **T3-U Design
  Guide.zip** and datasheets for the §2.0.1 wake-up timing.
- **Q2f.** What is command **`0x84`** (the T3 doc says it is "not supported")?
  It is in neither document we hold.
- **Q2g.** 🔴 **Co-master interface (§2.0.1).** Confirm the lock board can
  present **`TX / RX / SRDY / MRDY / GND`** with **bidirectional wake** — we
  wake the DL MCU, it wakes us — rather than the Tuya slave wake-pulse scheme.
  This is a demand, not a request; what we need back is their timing and
  electrical constraints, not their agreement.
- **Q2h.** Will they carry **one vendor `raw` DP** to tunnel OZKIE
  (bonds/envelopes/counters), keeping the rest of the standard catalogue
  untouched? See §2.0.2.

**To the operator / system architect:**

- **Q9.** 🔴 **Which Tuya product category is our lock registered under?** The
  module doc walks through **Video Lock / AI Visible Intercom**; DS013-T3 is a
  plain **Smart Lock**. The category picks the DP catalogue subset — and the
  two DP reference documents we are about to request are the *intercom* ones.
  Getting this wrong means requesting the wrong catalogue.
- **Q10.** §2.0.2 — do you agree with spending the co-master leverage on the
  **transport + one tunnel DP**, and keeping the standard catalogue? The
  alternative (native OZKIE on the UART, no DP layer) is cleaner but forks
  twelve suppliers' firmware. If you want native, §4.2 needs a second, compact
  TLV encoding for the UART leg — JSON does not belong on a lock-board MCU.
- **Q11.** ~~Demand a wake convention~~ — **withdrawn**, §2.0.15(a). The C6
  detects either, so we accept whatever each vendor uses and carry it as a
  profile field. What we still need per supplier is only: assert level,
  pulse or level, push-pull or open-drain, and whether the lines carry
  ZNP-style transaction framing or wake-plus-flow-control only.
- **Q11b.** 🔴 **Add a pull resistor to MRDY (GPIO7)** before fab —
  §2.0.15(b). It floats through every ESP32 reset today.
- **Q13.** Deep-sleep clock drift vs credential expiry — §2.0.15(c). No
  32.768 kHz crystal on `DLBASIC`. What is the drift budget, and does the DL
  MCU re-request time on each wake?
- **Q14.** 🔴 **§8.3 — is the DL MCU link master/slave or co-master?** SIMLOCK
  v2.1 says N8-master / DL-MCU-slave; §2.0.1 says co-master. If the test kit is
  meant to validate the production interface, they must agree.
- **Q15.** Phase 2 (§8) — *"2 doorlocks to compare"*: confirm the intent is one
  doorlock against LockSim and one against a real C6 DevKit MCU, differentially.
  The comparison only means something if both run the **same profile**.
- **Q16.** ~~Which two 1.9" boards?~~ — **resolved by §8.0.** Use the spare
  1.9" as the DL MCU emulator and a free C6 DevKit as the doorlock; DoorA,
  DoorB and the bridge stay untouched. `announce-before-hw-resets` still
  applies to the spare.
- **Q12.** With both sides now able to sleep (§2.0.1), what is the target
  quiescent budget? The T3 module holds **150–200 µA @ DTIM10**; our FTD
  `rx_on=1` design has never been measured (C9/L7). Co-master makes this
  answerable for the first time.

- **Q3.** With DP 60 taken by the alarm enum, where does the pairing gesture
  live? The failed-entry path (§6.3) still works *if* an equivalent of
  `ACCESS_RESULT` exists — but on this supplier failures arrive on DP 60 as
  alarms. Do we (a) map `wrong_password`/`wrong_card` from the DP 60 enum as
  the gesture, (b) request a dedicated DP allocation after all, or (c) drop the
  gesture for commercial and rely on §6.3(a)?
- **Q4.** 🔴 **§6.1 — does the on-prem server mint member bonds?** Yes forks the
  trust model and needs its own claim rows. No means guest keys must be minted
  by BANOI or by a hardware authority on site.
- **Q5.** §4.4 R3 — is `verb`-in-the-clear an acceptable metadata disclosure to
  relays? The alternative (encrypting the verb) costs us relay-side routing and
  rate-limiting.
- **Q6.** Version and profile distribution: profiles shipped **in** the firmware
  image, or fetched over MQTT and cached in NVS? The second lets us support a
  new supplier without an OTA; it also makes the DP map remotely mutable, which
  is a new attack surface on a door lock. My instinct is **in-image, signed**,
  but it is a real trade.

**To the app team (ftpos) — proposed XF-99:**

- **Q7.** Scope and appetite for retiring DP-frame construction in
  `envelope.dart` in favour of OZKIE JSON. This is the biggest ask we have made
  of them; it wants a staged path, not a flag day.
- **Q8.** Does BANOI's UI model tolerate per-lock capability (`profile_id`), or
  does it assume every lock does everything?

---

# 8. The bench roadmap — system architect's four phases

**Adopted as the plan** (system architect, 2026-08-12), with one phase inserted
in front and two corrections. Each phase adds exactly one kind of realism,
which is the right shape.

| Phase | Setup | What it proves | Blocked on |
|---|---|---|---|
| **0** 🔴 *inserted* | paperwork | **the real DP catalogue** | Q2 / Q2e — supplier |
| **1** | 1.9" Waveshare + **LockSim** (soft MCU) | DP semantics, credential lifecycle, app-driven control. *Chosen for hex-in/out visibility and the bigger LCD keypad* | DP work blocked on phase 0; link + plumbing are NOT |
| **2** | 1.9" + **ESP32-C6 DevKit as DL MCU** | real UART, real SRDY/MRDY, real sleep/wake. Two doorlocks side by side for differential test | **nothing — hardware is in hand** |
| **3** | **SIMLOCK N8 test kit** → manufacturer | remote/fleet verification over MQTT | kit does not exist yet; §8.2 first |
| **4** | DLBASIC prototype PCB → manufacturer | the product | PCB not confirmed |

### 8.0 Hardware actually in hand (2026-08-13)

| Board | Qty | Current role |
|---|---|---|
| Waveshare 1.9" LCD (C6) | 2 | **DoorA** = one of them (bond #0 BANOI, half the verified E2E) — one spare |
| Waveshare 1.47" LCD (C6) | 1 | **DoorB** (bench identity bond #0) |
| Bridge | 1 | `bridge32-1.34`, leader, in service |
| Waveshare GEEK | 2 | free |
| ESP32-C6 DevKit | 2 | free |
| LockSim | — | soft MCU |

**Phases 3 and 4 do not exist as hardware.** SIMLOCK has not been created and
the PCB is not confirmed — they are specifications. Only phases 0–2 are real,
and **phase 2 is buildable today** with the free boards.

**Recommended phase-2 rig, chosen to risk nothing:**

- **spare 1.9" = the DL MCU emulator.** Its LCD is the emulator's console
  (answering the visibility objection that made phase 1 come first) and its
  touch panel is the MCU's keypad — which is the *production* topology, where
  the keypad belongs to the DL MCU and our board has no panel at all. That is
  the one arrangement in which the pairing-gesture problem (§6.3) can be tested
  honestly.
- **ESP32-C6 DevKit = the doorlock.** Full GPIO breakout, so the link pins are
  free to move to whatever the profile says.
- **DoorA, DoorB and the bridge are untouched** — no reset, no re-bonding, the
  verified `ozkey-21 §10` setup stays intact.

*(A GEEK can also serve one end: per SIMLOCK §3.1 its GPIO3/4/5/6/9 breakout is
"sufficient for the DL MCU's 5-wire interface" — it was dropped only because it
left no pins for a camera, which phase 2 does not need.)*

## 8.1 🔴 Why phase 0 has to exist

Phase 1's goal is stated as *"confirm all DP working"*. **We cannot confirm the
DPs — we do not have them.** §2.1 shows the map we hold is invented; §2.5 shows
the RAW payload layouts for the real credential DPs are absent from every
document we have.

Run phase 1 as written and we would confirm the fiction *again*, with better
visibility, and then phases 2→3→4 would carry it outward to the manufacturer.
**Phase 3 is the point of no return** — see §8.2.

So split phase 1 into two tracks:

| Phase 1 track | Blocked? |
|---|---|
| Link layer, frame codec, OZKIE verb plumbing, profile **loader**, the L3/L0 split | 🟢 **no — start now** |
| DP semantics, credential lifecycle, expiry, `0x13` | 🔴 blocked on phase 0 |

The unblocked track is most of the engineering. The blocked track is a data
file.

## 8.2 A latent trap in `SIMLOCK N8 v2.1` — fix the doc, no fire

> **Re-scoped 2026-08-13, system architect:** *"SIMLOCK has not been created,
> PCB is still not yet confirmed, everyone still takes time to confirm."* An
> earlier draft called this urgent and said the fiction "ships to the
> manufacturer". **Nothing has shipped and nothing is agreed.** SIMLOCK and
> DLBASIC are both documents, not hardware, and phases 3–4 are not on a clock.
> This is a doc edit to make before the kit is built, not a recall.

`docs/DoorLockHW/SIMLOCK_N8_DLMCU_Technical_Specification_v2_1.pdf` §1.3:

> *"DPID — … **Only DPID 21 (Add Credential) and 22 (Delete Credential) are
> confirmed**; see Section 4.4."*

Those are the two we invented. On the real catalogue **21 is navigation
volume** and **22 is unallocated** (§2.1). This document is destined for the
door-lock manufacturer as a test-kit specification — i.e. **the fiction becomes
a requirement we hand to the vendor**, and they may well implement it.

Credit where due: the spec's hygiene is otherwise good — §4.4/§4.5/§4.6/§7.11
explicitly refuse to invent the DPID set, credential structure, error codes and
WebSocket schema, marking them *Reserved / To Be Defined pending confirmation
from the DL MCU firmware team*. That is exactly right. **The only defect is
that the two items it does assert as "confirmed" are the two that are wrong.**

**Fix before the kit is built: strike "confirmed" from 21/22 and move them into
the same Reserved block as the rest, pending phase 0.** Cheap now; expensive
once a vendor has implemented against it.

## 8.3 ⚠️ SIMLOCK says master/slave; we are co-master

SIMLOCK §1.3 and §3.3 define the handshake as **"N8 = master, DL MCU = slave"**,
active-low. §2.0.1 of this document has us as **co-master**, per the system
architect.

For a test kit driving test cases, master/slave may genuinely be sufficient.
But if the kit is meant to validate the *production* interface, it has to
match — otherwise we validate an interface we are not shipping. **Decide which,
and say so in SIMLOCK §4.2.** See Q14.

## 8.4 ⚠️ Four artifacts, four pinouts

| Signal | `DLBASIC V1.0` (product) | `doorlock19.ino` (bench 1.9") | `SIMLOCK N8` (test kit) |
|---|---|---|---|
| TX | GPIO0 | GPIO16 | GPIO5 ⚠️ strap |
| RX | GPIO1 | GPIO17 | GPIO6 |
| SRDY | GPIO2 | GPIO20 | GPIO3 |
| MRDY | GPIO7 | GPIO21 ⚠️ *"not schematic-confirmed"* | GPIO4 ⚠️ strap |

Every one of these is defensible in isolation — GPIO7 is `LCD_CS` on the
Waveshare, GPIO16/17 are the reserved ROM-download UART on DLBASIC, and
SIMLOCK v2.1 already flags its own GPIO4/5 strapping-pin hazard. But the
manufacturer receives **SIMLOCK's** numbers while the product ships
**DLBASIC's**.

This is the same lesson as the DP map, arriving for the third time: **pin
assignment is per-board configuration, not a constant.** It belongs in the
`link` block of the profile (§2.0.15a), and the test kit should state that the
pinout is illustrative for that board rather than normative for the interface.

## 8.5 Work order inside the phases

1. **Freeze the invented DP map** — mark `DpId` in `locksim/lib/tuya.ts` and
   `ozDpForwardable()` as PROVISIONAL-FICTION so nobody builds further on
   them. *(Cheap, do first.)*
2. **Delete the DP 60 handler** — known-wrong, and the one item with a live
   behavioural consequence.
3. **Correct SIMLOCK §4.4** before the document leaves the building (§8.2).
4. **`ozlink` abstraction** — `VirtualLink` (LockSim) / `GpioLink` (phase 2),
   one state machine, transport swapped underneath. Includes the co-master
   collision semantics, which are ours to specify and blocked on nobody.
5. **L3 message + verb table**, agreed across three teams. Paper only.
6. **L0 profile loader**, then `tuya-ds013-t3.json` once phase 0 lands.
7. **LockSim re-pointed at the profile**, so the bench stops validating fiction.
8. **`0x13` full-set sync** — closes ozkey-21 §8.3 for real.
9. **`0x08` record replay** → `id` + `occurred_at` — closes OZLODGE L8.
10. App migration, staged, per XF-99.

---

---

# 9. 🔴 REQUEST TO THE SERVER TEAM — the `utc` push is edge-triggered

**Raised 2026-08-13 by firmware. Please reply in this section.**

## 9.1 The symptom, from the bench

The operator reported the bridge LCD showing *"a red dash line"* where the clock
should be, and read it as *"no network / the clock is wrong"*. Diagnosis on
live hardware today:

| Checked | Result |
|---|---|
| Broker (`127.0.0.1:1883`) | ✅ up |
| Bridge → broker | ✅ connected, publishing `presence` **and** `liveness` |
| `ozlockserv` | ✅ running, pid 88987, `:4200` |
| Topic match | ✅ both `ozkie/lab/bridges/ozb-98a316a7e638/command` |
| `SITE_ID` | ✅ `lab` both sides |
| **`utc` on the command topic** | ❌ **never arrived** |

A manual `mosquitto_pub` of `{"utc":…}` to that exact topic was accepted by the
bridge immediately, so the receive path is fine. Nothing was ever sent.

## 9.2 Why we think it is structural, not a one-off

`ozlockserv/server.js:1149`:

```js
if (state === 'online') {
  const nowUtc = Math.floor(Date.now() / 1000);
  mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), { utc: nowUtc });
```

The push fires **only on the presence transition to `online`** — once, with no
retry and no periodic re-send. Any of these loses the clock for the rest of the
bridge's uptime:

- the server restarts while the bridge is already online (no new transition),
- the presence message is missed or arrives before the subscription is ready,
- a reconnect the server does not classify as a fresh `online`.

The bridge has no fallback: **NTP was removed in `bridge32-1.36`** (UDP 123 is
blocked here and on the hotel/office networks we target, so it was a second
clock writer that never worked and had to be refereed). The server is now the
*only* time source, by design — which makes a one-shot push the single point of
failure for every temporary PIN and RFID window on the mesh (`ozkey-21 §9`: an
unenforced expiry is a live security defect, not a cosmetic one).

## 9.3 What we are asking for

1. **Make the `utc` push idempotent rather than edge-triggered.** The bridge
   publishes `liveness` on a timer already — re-push `utc` when the server has
   no recent confirmation from that bridge, or simply on a slow timer. This is
   the same "use the mechanism that already exists" discipline as everywhere
   else; the mechanism is the liveness stream.
2. **Push `tz` on the same topic.** Timezone reaches the bridge *only* through
   the BLE ceremony (`bridge32.ino` `validModePayload` → `cfgTzMin` → NVS).
   There is no way to correct it without a re-ceremony — no DST, no site move,
   no fixing a bad value. `{"utc":…, "tz":<signed minutes>}` would close that.
   ⚠️ Firmware does **not** consume `tz` from MQTT yet; say the word and we add
   it in the same release. Panel-only, as ever — we still serve **UTC** to the
   MCU (`ozkey-21 §8.4`, do not "fix" that).

## 9.4 What firmware already did on its side

- `bridge32-1.36` — **the LCD now shows the broker.** The panel previously
  displayed WIFI and THREAD (the two transports that are *not* the time source)
  and nothing about MQTT (which is). A healthy bridge with no server time was
  indistinguishable from a broken one. Row 2 is now `TH:LEADER MQ:UP`, and the
  clock row reads `NO SERVER TIME` when the broker is up but no `utc` has
  arrived — i.e. it now points at §9.3(1) by name.
- NTP and its arbitration removed; one clock source.
- Fixed a firmware bug where two renderers drew the clock row with different
  timezone arguments, so the panel alternated between local and UTC once a
  second.

**No action needed from you on those — they are noted so you can see the panel
now diagnoses this condition rather than hiding it.**

---

*Firmware team, 2026-08-12 (roadmap 2026-08-13, §9 2026-08-13). Companion docs: `ozkey-21`
(time + expiry), `ozkey-22` (manufacturer questions), `ozkey-26` (firmware
capabilities reference), `ozlodge_v2.2` (commercial topology),
`DoorLockHW/DLBASIC_V1.0` (product board),
`DoorLockHW/SIMLOCK_N8_…_v2_1` (test kit).*
