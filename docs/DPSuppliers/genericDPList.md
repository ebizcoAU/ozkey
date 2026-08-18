# Generic Tuya smart-lock DP list

**Status: 2026-08-18 — firmware team.** Supersedes nothing; this is the first
version. The machine-readable form of this document is
`profiles/tuya-lock-catalogue.json` + `profiles/products/tuya-generic-lock.json`,
and **those are authoritative** — this file explains them and records where each
row came from. If the two ever disagree, the JSON wins and this file is stale.

---

## 0. The premise

> *"90% of the makers talked about standard DP from the TUYA developer website.
> We already have 2 makers' DP lists; the rest seem likely to buy directly from
> TUYA pre-made, pre-flashed DL-MCU."* — operator, 2026-08-18

**That premise is correct**, and it is the single most useful thing anyone has
said about this problem. It is why `profiles/` is a *catalogue plus short
selection files* and not one DP list per supplier (`ozkey-27 §3`): a maker who
buys a pre-flashed Tuya DL-MCU does not invent DP numbers, they tick boxes on
the Tuya IoT platform at PID creation, and Tuya generates their protocol
document from those ticks. Both documents we hold are visibly machine-generated
that way — the Luona PDF even carries its generation timestamp
(`协议生成时间：2026年08月11日 16:42`).

**But there is one correction, and it is load-bearing.** See §1.

---

## 1. 🔴 "Standard" is per CATEGORY, not per Tuya

Tuya does not publish *one* smart-lock instruction set. It publishes a different
standard instruction set **per product category**, and the categories reuse the
same low DP numbers for completely different things. Tuya's own category list
names at least nine lock categories: `ms` (residential lock), `gyms` (business
lock), `jtmspro` (residential lock pro), `hotelms` (hotel lock), `jtmsbh` (smart
lock, keep-alive), `mk` (access control), `videolock`, `photolock`,
`ms_category` (accessories).

Concretely — the same number, three published Tuya meanings:

| DP | **Our family** (Wi-Fi/BLE keep-alive serial) | Zigbee residential lock | Wi-Fi Lock **Pro** |
|---:|---|---|---|
| 9 | `remote_no_pw_unlock_setting` (raw) | alarm / alert enum | — |
| 10 | `remote_unlock` (raw) | battery percentage | lock settings |
| 21 | `navigation_volume` (enum) | app unlock **with** password | — |
| 24 | `auto_lock_delay` (5–1800 s) | create temporary password | unlock record |
| **45** | **`battery_percentage` (−1..100)** | **palm-print unlock record** | — |
| 50 | *(not selected)* | — | remote unlock/lock (raw) |
| 61 | `unlock_password` (cred id) | one-time password (bool) | — |

**DP 45 is the whole lesson in one row.** In our family it is the battery
percentage. In Tuya's Zigbee residential lock it is a palm-print unlock record.
Both are "the standard Tuya DP list". A firmware that treats "Tuya standard" as
a single global table will read a palm print as a battery level.

So the useful sentence is not *"use the Tuya standard list"*. It is:

> **Use the Tuya standard list _for the category the module was provisioned
> under_, and confirm the category before trusting a single number.**

*(Fidelity note: the "our family" column is transcribed from supplier documents
we hold in this directory. The other two columns are from Tuya's public
developer pages, read through a summariser — they are good enough to prove the
collisions exist and are **not** good enough to build against. Do not copy them
into a profile.)*

---

## 2. Which family we are in

Both documents in `docs/DPSuppliers/` describe the same thing: a Tuya **Wi-Fi +
BLE module** talking over **UART** to a separate lock-board MCU (the "DL-MCU").
The Luona document's own title is *涂鸦云低功耗通用串口接入协议* — "Tuya Cloud
low-power general serial access protocol".

Evidence the two are the same category, from two independent vendors and two
different products (one is a video lock, one is not):

- **DP 42** `ble_switch` — identical meaning in both.
- **DP 76** `unlock_ble` — identical meaning in both. The Ladin T3 document says
  *"To enable Bluetooth lock control when the device is offline, select DP76 –
  unlock_ble"*, which is exactly how Luona's table defines it.
- **DP 149** and **DP 212** — the Ladin document warns *"Do not select DP149"*
  and *"do not select DP212 if audio/video entries should be hidden"*. Both
  numbers are meaningful in the same namespace.
- **They contradict each other on nothing.** Not one DP.

🔴 **Open:** the exact category slug is **unconfirmed**. The evidence points at
the keep-alive Wi-Fi/BLE lock family (`ms` or `jtmsbh`, with `videolock` as the
audio/video superset that adds DP 212). Neither supplier states it. **Ask
supplier #3 for their category slug** — it is one line in their PID definition
and it settles this permanently.

---

## 3. The generic DP list

This is the union of both suppliers, which is what
`profiles/tuya-lock-catalogue.json` holds. 34 of the 36 rows come from Luona's
auto-generated table; DP 149 and 212 are named only by Ladin.

🟢 **Independently cross-checked.** On 2026-08-18 the operator extracted the same
table from the same PDF by hand, without reference to the catalogue, which had
been built a week earlier. The two agree **row for row** — same 34 DPs, same
types, same ranges, no contradictions and no extra rows on either side. That
extraction has been merged into the tables below and its file deleted, so this
document is the single copy: the plain-English function descriptions are the
operator's, the codes, statuses and verbs are the catalogue's. Given how much of
this project's pain came from a map that only ever agreed with itself, two
independent readings landing identically is worth recording.

**Witness** — `L` = Luona *Smart Lock DS013-T3* (`protocol_vr4iiuqtyh0q4nix_20260811.pdf`,
PID `vr4iiuqtyh0q4nix`) · `T` = Ladin *T3-U module* (`T3_Final_Customer_Version_EN.docx`).

**Status** — `confirmed` = type **and** payload semantics documented ·
`reserved` = DP and type known, **payload layout not supplied**, firmware must
answer `UNSUPPORTED` rather than emit plausible bytes (`ozkey-27 §2.5`) ·
`unknown` = named but not defined.

### 3.1 Credential writes — every one of them `reserved`

| DP | Tuya code | What it does | Type | Status | Wit. | OZKIE verb |
|---:|---|---|---|---|---|---|
| 9 | `remote_no_pw_unlock_setting` | Configure password-free remote unlock — key, use count, expiry | raw ≤128 B | reserved | L | `lock.settings.set` |
| 13 | `bulk_unlock_method_add` | Add a credential (fingerprint / card / PIN), >255 users | raw ≤128 B | reserved | L | `cred.put` |
| 14 | `bulk_unlock_method_delete` | Delete a specific credential | raw ≤128 B | reserved | L | `cred.delete` |
| 15 | `bulk_unlock_method_modify` | Modify an existing credential | raw ≤128 B | reserved | L | `cred.put` |
| 16 | `bulk_password_add` | Add a large-capacity PIN | raw ≤128 B | reserved | L | `cred.put` |
| 17 | `bulk_password_delete` | Delete a large-capacity PIN | raw ≤128 B | reserved | L | `cred.delete` |
| 18 | `bulk_password_modify` | Modify a large-capacity PIN | raw ≤128 B | reserved | L | `cred.put` |
| 19 | `bulk_unlock_method_sync` | Synchronise the whole credential list | raw ≤128 B | reserved | L | `cred.sync` |
| 86 | `offline_password_params` | Deliver offline time-ranged passwords, cloud → MCU | string ≤255 B | reserved | L | `cred.put` |

Luona's own instruction table prints `0x00-0xff` in the 功能指令 column for DP
9/10/13–19 while fully specifying DP 11 two rows away. **The supplier did not
omit these by accident — they declined to specify them.** That is a supplier
gap, not a gap in our reading, and it is why the ask is *"send us the byte
layouts of DP 9/10/13–19"*, not *"send us the protocol document"* (we have it).

### 3.2 Commands

| DP | Tuya code | What it does | Type / detail | Status | Wit. | OZKIE verb |
|---:|---|---|---|---|---|---|
| 10 | `remote_unlock` | **Trigger a remote network unlock** | raw ≤128 B | **reserved** | L | `lock.unlock` |
| 11 | `connection_mode` | Report/set Wi-Fi keep-alive vs sleep vs lock modes | enum `keep` `sleep` `lock_keep` `lock_sleep` | confirmed | L | `lock.settings.set` |
| 21 | `navigation_volume` | MCU voice/beep volume | enum `mute` `low` `normal` `high` | confirmed | L | `lock.settings.set` |
| 23 | `auto_lock` | Toggle auto-lock (off = permanently-open mode) | bool | confirmed | L | `lock.settings.set` |
| 24 | `auto_lock_delay` | Delay from unlock to auto-relock | value 5–1800 s | confirmed | L | `lock.settings.set` |
| 42 | `ble_switch` | Enable BLE control. **Ladin:** specifically governs whether BLE transmission is on when Wi-Fi is down, in dual-mode | bool | confirmed | **L + T** | `lock.settings.set` |
| 156 | `wifi_connection_strategy` | Wi-Fi DTIM interval | enum `wifi_dtm20` `wifi_dtm10` | confirmed | L | `lock.settings.set` |

🔴 **DP 10 is the hole in the middle of the product.** It is the real
remote-unlock command, and it is `reserved` — the supplier never gave its
payload layout. **There is currently no usable real DP that opens a door.**
DP 72 is not a substitute: it is a *report* that a remote unlock happened, not
a command. Our shipping firmware papers over this with an invented DP 1
(`ozkie-legacy-v0`, and the bench-only `extra` in `ozsim-fullfeature`), which is
exactly the fiction this document exists to retire. **Closing DP 10's payload
layout is the highest-value question to put to any supplier.**

### 3.3 Access events — the ones that actually work

| DP | Tuya code | What it reports | Range | Status | Wit. | `access_kind` |
|---:|---|---|---|---|---|---|
| 61 | `unlock_password` | Id of the PIN that opened the door | 0–65535 | confirmed | L | `pin` |
| 63 | `unlock_fingerprint` | Id of the fingerprint that opened the door | 0–65535 | confirmed | L | `fingerprint` |
| 64 | `unlock_card` | Id of the card that opened the door | 0–65535 | confirmed | L | `rfid` |
| 69 | `unlock_temporary` | Id of the temporary PIN used | 0–65535 | confirmed | L | `temp_pin` |
| 72 | `unlock_remote` | Id of the remote app user who unlocked | 0–999 | confirmed | L | `remote` |
| 73 | `unlock_remote_voice` | Id of the voice-assistant user who unlocked | 0–999 | confirmed | L | `remote_voice` |
| 76 | `unlock_ble` | Id of the BLE credential used | 0–99999 | confirmed | **L + T** | `ble` |
| 74 | `unlock_combination_record` | Combined unlock records, for id ranges >255 | raw ≤128 B | reserved | L | — |
| 89 | `offline_password_unlock_report` | An unlock by offline password | raw ≤128 B | reserved | L | — |

The value is a **credential id assigned by the lock hardware**, not a count —
"fingerprint #7 opened the door". Select DP 74 instead of the `value` DPs only
if credential ids must exceed 255.

### 3.4 State and alarms

| DP | Tuya code | What it reports | Type / detail | Status | Wit. |
|---:|---|---|---|---|---|
| 45 | `battery_percentage` | Battery level; re-reported on every re-join | value **−1**–100 (−1 = not yet read) | confirmed | L |
| 47 | `bolt_state` | Bolt thrown / withdrawn, detected on the lock body | bool | confirmed | L |
| 52 | `opened_from_inside` | Door opened by the interior handle | bool | confirmed | L |
| 53 | `doorbell` | Someone pressed the doorbell | bool, 1 = pressed | confirmed | L |
| 54 | `device_info` | Core device information | raw ≤128 B | reserved | L |
| 60 | `alarm` | Alarm events — 18 of them, listed below | enum, **report-only** | confirmed | L |
| 98 | `hijack_alarm` | **Duress / panic unlock** | bool | confirmed | L |
| 87 | `offline_password_clear_single_report` | One offline password cleared at the lock | raw ≤128 B | reserved | L |
| 88 | `offline_password_clear_all_report` | All offline passwords cleared at the lock | raw ≤128 B | reserved | L |

**DP 60 `alarm` enum, verbatim and in order** — the wire value is the index:

`0 wrong_finger` · `1 wrong_password` · `2 wrong_card` · `3 wrong_face` ·
`4 tongue_bad` · `5 tongue_not_out` · `6 unclosed_time` · `7 unlock_attempt` ·
`8 key_in` · `9 too_hot` · `10 low_battery` · `11 wrong_finger_vein` ·
`12 wrong_hand` · `13 stay_alarm` · `14 pry` · `15 network_error` ·
`16 network_recovery` · `17 system_lock`

**DP 98 must not be collapsed into DP 60.** It is duress — it needs its own
escalation path (`ozkey-28 §3.4`).

### 3.5 Do not select

| DP | Tuya code | Status | Wit. | Why |
|---:|---|---|---|---|
| 149 | *(unnamed)* | unknown | T | Ladin §3.1: *"Do not select DP149."* No reason given. |
| 212 | `initiative_message` | unknown | T | Video/audio lock only; requires the IPC skill. Ladin: omit it to hide audio/video entries. |

---

## 4. What this list is NOT

**It is not the complete category standard.** It is the union of two witnesses.
There is direct internal evidence of DPs we have never seen a number for:
**DP 60's alarm enum contains `wrong_finger_vein` and `wrong_hand`**, so this
category supports finger-vein and palm credentials — and therefore has
`unlock_*` DPs for them that neither of our suppliers selected. Same for face
(`wrong_face`). Treat an unrecognised DP from a new supplier as *"a standard DP
we have not met yet"*, not as *"that vendor invented something"*.

**It is not a payload specification.** 14 of 36 rows are `raw`/`string` with the
layout undisclosed. A `reserved` row tells you the DP exists and its type — it
does not let you build a frame.

**It is not the transport.** These are all per-vendor and none of them are in
this list:

| Concern | Luona DS013-T3 | Ladin T3-U |
|---|---|---|
| Command words | `0x06` module-issues / `0x07` MCU-reports (general variant, by citation) | general variant; `0x84` unsupported |
| Wake handshake | **not documented at all** | active-**HIGH**, edge-triggered: module→MCU 300 ms on P32, MCU→module ≥1 ms on P13 |
| Module identity | Beken BK7236 class | T3-U / T3-U-IPEX |

🔴 **We parsed `0x06` in both directions until `doorlock-1.92`.** A real MCU
reports on `0x07`, so every real report — doorbell, access events, battery alarm
— would have been dropped as an unparsed `cmd 0x7`. It was invisible because
LockSim was written to match the firmware, so both halves of the bench agreed
with each other and disagreed with the supplier. **Our profile still guesses
`assert: low` for the wake handshake, and Ladin documents active-HIGH.** That
guess has never met real hardware.

---

## 5. Onboarding supplier #3 (and #4, and #12)

**A new supplier should cost about twenty lines of JSON.** That is the entire
point of the catalogue split. The procedure:

1. **Ask for the Tuya-generated protocol document, and for the PID.** Every
   maker who buys a pre-flashed DL-MCU has one — Tuya generates it from their
   PID definition. If they cannot produce it, they are not on a standard Tuya
   module and none of this applies.
2. **Run the fingerprint test.** Three questions, answerable in 30 seconds by
   looking at their table:
   - Is battery on **DP 45**? (Zigbee lock says 10 — wrong family.)
   - Are unlock records on **61/63/64**? (Zigbee lock says 1/2/5 — wrong family.)
   - Is `unlock_ble` on **DP 76**?
   Three yeses ⇒ same family, our catalogue applies.
3. **Write the selection file** — `profiles/products/<vendor>-<model>.json` with
   a `selects` array. Only genuinely product-specific DPs go in `extra`.
4. **Any DP in their table that is not in our catalogue is a catalogue addition**,
   not a product quirk. Add it to `tuya-lock-catalogue.json` **once**, with its
   witness recorded, and every future product can select it.
5. **Regenerate and test:** `python3 blelock/tools/gen_profile.py` then
   `npm test --prefix locksim`.

### The five questions our two suppliers left open

Put these to every new supplier — they are the same five every time:

1. **The byte layout of DP 10 `remote_unlock`.** Without it there is no real
   remote unlock. *(Highest value. Ask first.)*
2. **The byte layouts of DP 9 and DP 13–19** — every credential write.
3. **The wake handshake**: assert level, level-vs-pulse, minimum width, on which
   line, in each direction.
4. **The command-word variant**: general (`0x06`/`0x07`) or low-power
   (`0x09`/`0x05`/`0x08`)?
5. **The Tuya category slug** the PID was created under (§2).

---

## 6. How this drives LockSim and the firmware

```
profiles/
  tuya-lock-catalogue.json          ← §3 of this document, machine-readable
  products/
    tuya-generic-lock.json          ← NEW: "a standard pre-flashed Tuya DL-MCU"
    tuya-ds013-t3.json              ← Luona, selects 34
    tuya-t3-videolock.json          ← Ladin, partial
    ozsim-fullfeature.json          ← bench PID-discovery target (fictional)
    ozkie-legacy-v0.json            ← our invented map. Still the default.
```

`tuya-generic-lock` selects **every catalogue DP except 149 and 212** — the two
Tuya itself says not to select. It is therefore the maximal honest lock: the
`confirmed` DPs behave, and the nine `reserved` credential DPs are present and
**refuse loudly**, which is the behaviour a real supplier's lock would show us
today. Running LockSim on this profile is how we find out what breaks *before* a
real DL-MCU is on the wire, instead of after.

Both consumers read the same JSON — LockSim directly, the firmware via
`blelock/tools/gen_profile.py` → `blelock/common/ozprofile_gen.h` (flat PROGMEM
tables, no runtime parse). Neither can hold a different idea of what a DP number
means.

🔴 **What this document deliberately does NOT do: switch the default.**
`OZ_PROFILE_DEFAULT_ID` is still `ozkie-legacy-v0`, because BANOI and ozlockserv
still seal raw **DP 1** frames and flipping the default would break every
unlock on the next flash. That migration is tracked as **L-4** and is the
operator's call, not a side effect of writing a DP list. Related: an unknown PID
currently **keeps the current profile** (`ozProfileByTuyaPid()` returns
`nullptr`, deliberately). Making `tuya-generic-lock` the unknown-PID fallback is
a genuine firmware change and is not in this change.

---

## 7. Sources

| | |
|---|---|
| `docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf` | **Luona Smart** — Smart Lock DS013-T3, PID `vr4iiuqtyh0q4nix`, Tuya-generated 2026-08-11. 34 DPs with types, ranges and enums. The primary witness. |
| `docs/DPSuppliers/T3_Final_Customer_Version_EN.docx` | **Ladin Tech** — T3-U module integration document. No DP table; names DP 42/76/149/212 and documents the wake handshake. |
| *(merged, file deleted)* | The operator's independent hand extraction of the Luona table, 2026-08-18 — the source of the English function descriptions in §3. Agreed with the catalogue row for row; merged into this document and removed so there is only one copy to keep current. |
| Tuya developer platform, public pages | Category list; Zigbee residential-lock and Wi-Fi Lock Pro DP references — used **only** to establish §1's per-category collisions. |
| `docs/ozkey-27.md` | The findings behind the catalogue/selection design; §2.5 on `reserved`. |
| `docs/ozkey-28.md` | OZKIE v1 verbs — the `verb`/`field` columns. |
| `docs/ozkey-39.md` | 🔴 The two suppliers were previously conflated as one. Module identity, DP selection and the wake handshake are **per-vendor**. |
| `profiles/README.md` | Resolution order, entry shape, the `status` field. |

🔴 **The standing caveat.** No real Tuya DL-MCU has ever been on our wire.
Everything here is read off supplier paper and cross-checked between two
vendors; nothing in it has been confirmed against hardware. "Verified on the
bench" means verified against our own assumptions until that changes.
