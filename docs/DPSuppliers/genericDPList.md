# The Tuya smart-lock DP catalogue — and why there is no *generic* one

**Status: 2026-08-20 — firmware team.** Rewritten. **Supersedes the 2026-08-18
version of this file**, which was written one day before we established that a
generic Tuya lock DP map cannot exist, and which described a profile layout
(`tuya-generic-lock`, `ozkie-legacy-v0`) that has since been **deleted**.

The machine-readable forms are `profiles/tuya-lock-catalogue.json`,
`profiles/products/*.json` and `profiles/models.json`, and **those are
authoritative** — this file explains them and records where each row came from.
If the two ever disagree, the JSON wins and this file is stale.

*(The filename still says "generic". It is kept so links and git history survive;
the word is the error this document exists to correct. §1 is why.)*

---

## 0. The premise, and the correction that outlived it

> *"90% of the makers talked about standard DP from the TUYA developer website.
> We already have 2 makers' DP lists; the rest seem likely to buy directly from
> TUYA pre-made, pre-flashed DL-MCU."* — operator, 2026-08-18

**The premise is correct and still load-bearing.** A maker who buys a pre-flashed
Tuya DL-MCU does not invent DP numbers — they tick boxes on the Tuya IoT platform
at PID creation, and Tuya generates their protocol document from those ticks.
Both documents we hold are visibly machine-generated that way; the Luona PDF even
carries its generation timestamp (`协议生成时间：2026年08月11日 16:42`).

That is why `profiles/` is a *catalogue plus short selection files* rather than
one hand-written DP list per supplier (`ozkey-27 §3`), and why onboarding
supplier #3 should cost about twenty lines of JSON (§5).

**But the scope of "standard" was wrong, and it was wrong in the direction that
opens doors by accident.** See §1.

---

## 1. 🔴 "Standard" is per CATEGORY, not per Tuya

Tuya does not publish *one* smart-lock instruction set. It publishes a different
standard instruction set **per product category**, chosen at PID creation, and
the categories reuse the same low DP numbers for completely different things.
Tuya's own category list names at least nine lock categories: `ms` (residential
lock), `gyms` (business lock), `jtmspro` (residential lock pro), `hotelms`
(hotel lock), `jtmsbh` (smart lock, keep-alive), `mk` (access control),
`videolock`, `photolock`, `ms_category` (accessories).

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
| **76** | **`unlock_ble` — opens the door** | — | **`fill_light` — turns on a lamp** |

**Read the DP 76 row twice. We ship remote unlock on DP 76.** On a Luona lock it
opens the door. On a lock built to Tuya's own Wi-Fi Lock Pro standard, the same
command switches on a light.

**DP 45 is the same lesson in a quieter register.** In our family it is battery
percentage; in Tuya's Zigbee residential lock it is a palm-print unlock record.
Both are "the standard Tuya DP list". Firmware that treats "Tuya standard" as one
global table will read a palm print as a battery level and never notice.

So the useful sentence is not *"use the Tuya standard list"*. It is:

> **Use the Tuya standard list _for the category the module was provisioned
> under_, and confirm the category before trusting a single number.**

### 1.1 What this cost, and what it killed

This finding is why, on 2026-08-20, three profiles were **deleted outright**:

| deleted | what it was | why it had to go |
|---|---|---|
| `tuya-generic-lock` | "a standard pre-flashed Tuya DL-MCU" | never generic — it was **Luona's map with the PID stripped out**. As the fallback for an unidentified lock it could have "unlocked" a door by turning on a lamp |
| `ozkie-legacy-v0` | our invented map — DP 1 unlock, DP 21–24 credentials | none of those DPs exist on any real product (`XF-110`) |
| `ozsim-fullfeature` | bench PID-discovery target | fiction, and it was silently reverting LockSim's profile on refresh |

**There is no `fiction` entry left anywhere in `profiles/`** — verified, zero
occurrences. The status value survives in the schema only so an old header cannot
quietly re-mean something else.

The 2026-08-18 version of this file diagnosed all of this correctly in this
section and then, four sections later, shipped `tuya-generic-lock` anyway. That
gap between the finding and the artefact is the thing to watch for.

*(Fidelity note: the "our family" column is transcribed from supplier documents
we hold in this directory. The other columns are from Tuya's public developer
pages, read through a summariser — good enough to prove the collisions exist,
**not** good enough to build against. Do not copy them into a profile. The one
exception is `tuya-wifi-lock-pro`, transcribed deliberately and completely as a
reference map; it is still not a product — see §6.3.)*

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

**This is what the catalogue is scoped to.** `tuya-lock-catalogue.json` is the
standard list *for this family*, not for Tuya at large. A product from another
category does not select from it — it gets a `standalone` profile with its own
entries (§6.1).

---

## 3. The catalogue — this family's DP list

The union of both suppliers, which is what `profiles/tuya-lock-catalogue.json`
holds: **36 DPs — 19 `confirmed`, 15 `reserved`, 2 `unknown`.** 34 rows come from
Luona's auto-generated table; DP 149 and 212 are named only by Ladin.

🟢 **Independently cross-checked, twice.** On 2026-08-18 the operator extracted
the same table from the same PDF by hand, without reference to the catalogue,
which had been built a week earlier. The two agree **row for row** — same 34 DPs,
same types, same ranges, no contradictions and no extra rows on either side. On
2026-08-20 a **live DP census over Tuya command `0x08`** against the bench MCU
returned **34/34 full match, DP 76 present**. Given how much of this project's
pain came from a map that only ever agreed with itself, two independent readings
and one hardware confirmation are worth recording.

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

🔴 **This is the whole credential story, and it got worse, not better, when we
adopted a real map** (`ozkey-42 §2.4.1`):

| profile | issuing a PIN |
|---|---|
| `ozkie-legacy-v0` (fiction, deleted) | **worked** — because we made the DP up |
| `tuya-luona-ds013-t3` (real) | **refused, with a reason** |

On the real map, the invented credential DP 21 is `navigation_volume`. Firmware
now refuses rather than writing a PIN onto the volume control. **Only the
supplier can end this** — `ozkey-42` P0, 15 RAW layouts, sent to **Luona Smart**.

### 3.2 Commands

| DP | Tuya code | What it does | Type / detail | Status | Wit. | OZKIE verb |
|---:|---|---|---|---|---|---|
| 10 | `remote_unlock` | **Trigger a remote network unlock** | raw ≤128 B | **reserved** | L | `lock.unlock` |
| 11 | `connection_mode` | Report/set Wi-Fi keep-alive vs sleep vs lock modes | enum `keep` `sleep` `lock_keep` `lock_sleep` | confirmed | L | `lock.settings.set` |
| 21 | `navigation_volume` | MCU voice/beep volume | enum `mute` `low` `normal` `high` | confirmed | L | `lock.settings.set` |
| 23 | `auto_lock` | Toggle auto-lock (off = permanently-open mode) | bool | confirmed | L | `lock.settings.set` |
| 24 | `auto_lock_delay` | Delay from unlock to auto-relock | value 5–1800 s | confirmed | L | `lock.settings.set` |
| 42 | `ble_switch` | Enable BLE control. **Ladin:** specifically governs whether BLE transmission is on when Wi-Fi is down, in dual-mode | bool | confirmed | **L + T** | `lock.settings.set` |
| **76** | `unlock_ble` | **The DP we open the door with** — see below | value 0–99999 | **confirmed** | **L + T** | `lock.unlock` |
| 156 | `wifi_connection_strategy` | Wi-Fi DTIM interval | enum `wifi_dtm20` `wifi_dtm10` | confirmed | L | `lock.settings.set` |

🟢 **A real DP opens a door. This changed on 2026-08-20.** The previous version
of this file said *"there is currently no usable real DP that opens a door"* —
that is **no longer true**. `lock.unlock` resolves to **DP 76 `unlock_ble`**,
which is `confirmed`, fully specified by the supplier (`0x4c`, VALUE, 4 bytes,
range 0..99999) and witnessed by both vendors. Verified end to end twice on the
bench: app seals `{"kind":"unlock"}` → firmware verifies the bond → DP 76 → the
door opened (`XF-120 §8`).

🔴 **DP 10 is still the hole, and it is still worth closing.** `remote_unlock` is
the DP Tuya *designed* for a network-originated unlock, and it is `reserved` —
the supplier never gave its payload layout. Using DP 76 instead works, but DP 76
is defined as *"the id of the BLE credential used"*, so a network unlock reported
through it **may distort the supplier's audit record**. That is the honest
caveat, recorded in `doorlock-2.11`'s DP-selection note and in `XF-120 §8.1`.
Consequence: `ozkey-42` Priority B (DP 10) drops from **blocking to
informational** — remote unlock ships without waiting on a vendor round trip.

**DP 72 is not a substitute for either.** It is a *report* that a remote unlock
happened, not a command.

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

**Seven DPs legitimately report `event.access`.** That is not ambiguity: upward,
the MCU hands us a DP number and we ask what it means, which has exactly one
answer per DP. Ambiguity is a *downward* concern only — see §6.2.

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

⚠️ **A DP report is not always safe to treat as an event.** During the 2026-08-20
census, replies to a *query* were processed as live events: DP 53 latched
`has_doorbell` and **opened the BLE pairing window**. A diagnostic with a
security side effect, found only by running it. Solicited replies and unsolicited
reports must be distinguished before dispatch.

### 3.5 Do not select

| DP | Tuya code | Status | Wit. | Why |
|---:|---|---|---|---|
| 149 | *(unnamed)* | unknown | T | Ladin §3.1: *"Do not select DP149."* No reason given. |
| 212 | `initiative_message` | unknown | T | Video/audio lock only; requires the IPC skill. Ladin: omit it to hide audio/video entries. |

---

## 4. What this list is NOT

**It is not "the Tuya DP list".** It is the standard list for **one category**
(§2), and the category slug is still unconfirmed. Applying it to a lock from
another category is the DP 76 / DP 45 failure in §1.

**It is not the complete category standard.** It is the union of two witnesses.
There is direct internal evidence of DPs we have never seen a number for:
**DP 60's alarm enum contains `wrong_finger_vein` and `wrong_hand`**, so this
category supports finger-vein and palm credentials — and therefore has
`unlock_*` DPs for them that neither of our suppliers selected. Same for face
(`wrong_face`). Treat an unrecognised DP from a new supplier as *"a standard DP
we have not met yet"*, not as *"that vendor invented something"*.

**It is not a payload specification.** 15 of 36 rows are `reserved` — `raw` or
`string` with the layout undisclosed. A `reserved` row tells you the DP exists
and its type; it does not let you build a frame.

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

**A new supplier should still cost about twenty lines of JSON** — provided they
are in this category. That is the point of the catalogue split, and §1 narrows
it rather than cancelling it. The procedure:

1. **Ask for the Tuya-generated protocol document, and for the PID.** Every
   maker who buys a pre-flashed DL-MCU has one. If they cannot produce it, they
   are not on a standard Tuya module and none of this applies.
2. **Ask for the category slug** (§2). One line in their PID definition.
3. **Run the fingerprint test.** Three questions, answerable in 30 seconds by
   looking at their table:
   - Is battery on **DP 45**? (Zigbee lock says 10 — wrong family.)
   - Are unlock records on **61/63/64**? (Zigbee lock says 1/2/5 — wrong family.)
   - Is `unlock_ble` on **DP 76**?
   Three yeses ⇒ same family, our catalogue applies. **Any no ⇒ different
   category: give them a `standalone` profile, do not let them select from this
   catalogue.**
4. **Write the selection file** — `profiles/products/<vendor>-<model>.json`, with
   a `selects` array and `supplier.pid` filled in. Only genuinely
   product-specific DPs go in `extra`. **Without a PID the profile cannot be
   paired** (`XF-122 §9`).
5. **Any DP in their table that is not in our catalogue is a catalogue addition**,
   not a product quirk. Add it to `tuya-lock-catalogue.json` **once**, with its
   witness recorded, and every future product can select it.
6. 🔴 **Re-verify DPs 101–103.** Our in-lock bond channel (§6.4) is squatting on
   numbers Tuya does **not** reserve. Check them against the new supplier's list
   before assuming they are free.
7. **Regenerate and test:** `python3 blelock/tools/gen_profile.py` then
   `npm test --prefix locksim`. The generator refuses to build an ambiguous
   command map, and `--check` fails the test run if the outputs are stale.

### The five questions our two suppliers left open

Put these to every new supplier — they are the same five every time:

1. **The byte layouts of DP 9 and DP 13–19** — every credential write.
   *(Now the highest-value ask: it is the only thing blocking PINs on real
   hardware. Ask first.)*
2. **The byte layout of DP 10 `remote_unlock`** — and whether reporting a
   network unlock through DP 76 distorts their audit record (§3.2).
3. **The wake handshake**: assert level, level-vs-pulse, minimum width, on which
   line, in each direction.
4. **The command-word variant**: general (`0x06`/`0x07`) or low-power
   (`0x09`/`0x05`/`0x08`)?
5. **The Tuya category slug** the PID was created under (§2).

---

## 6. How this drives LockSim and the firmware

### 6.1 One profile per real lock model

```
profiles/
  tuya-lock-catalogue.json          §3 of this document, machine-readable (rev 2)
  products/
    tuya-luona-ds013-t3.json        Luona Smart — selects 34. THE ONE WE SHIP ON
    tuya-wifi-lock-pro.json         Tuya's own published standard — standalone, 42
    tuya-ladin-f7-t3.json           Ladin — STUB, 4 DPs, unusable
  models.json                       GENERATED — the app's PID → model manifest
```

| profile | what it is | DPs | PID | pairable |
|---|---|---|---|---|
| `tuya-luona-ds013-t3` | Luona Smart DS013-T3 | 34 | `vr4iiuqtyh0q4nix` | 🟢 yes |
| `tuya-wifi-lock-pro` | Tuya's published standard map — a **category**, not a product | 42 | none | no |
| `tuya-ladin-f7-t3` | **STUB** — no DP reference ever supplied (`ozkey-42`) | 4 | none | no |

**Exactly one model is pairable today**, and that is correct rather than broken
(`XF-122 §9.1`). Both consumers read the same JSON — LockSim directly, the
firmware via `blelock/tools/gen_profile.py` → `blelock/common/ozprofile_gen.h`
(flat PROGMEM tables, no runtime parse). Neither can hold a different idea of
what a DP number means.

### 6.2 The DP map is pinned at BUILD time, and no DP number is left in C

```
make -C blelock flash BOARD=19 PROFILE=tuya-luona-ds013-t3
```

- **PID discovery CONFIRMS, never adopts.** An unknown PID no longer falls back —
  it keeps the pinned profile, says so, and lets the verb resolver refuse. **There
  is nothing safe to fall back to.**
- **`ozResolveVerb(verb, field, dir)`** reads a generated PROGMEM table.
  `if (isUnlock) { ozDpFind(76) ? 76 : 1 }` and `dp = grant_pin ? 21 : 23` are
  gone. A second supplier whose unlock DP is not 76 needs **no firmware change**.
- **Catalogue rev 2** adds `verb_down`/`field_down` for the 20 DPs a command may
  target. 32 of 36 entries are `dir: both` — that is what Tuya *permits*, not
  what OZKIE should send. **Absence of `verb_down` is meaningful.**
- **`gen_profile.py` refuses to build on an ambiguous command.** Ordering is
  policy, not luck: `(verb, STATUS, field)`, so a bare `lock.unlock` takes DP 76
  (`confirmed`) and never DP 10 (`reserved`).
- Uniqueness is enforced **downward only**. Upward, seven DPs may share
  `event.access` (§3.3); requiring those to be distinguishable by verb was a
  modelling error, not a catalogue defect.

### 6.3 🔴 The unpinned default is a configuration error, not a model

`OZ_PROFILE_DEFAULT_ID` is **`tuya-wifi-lock-pro`** — what a build falls back to
when flashed without `PROFILE=`. It was chosen as the *least-wrong* default when
the fiction was deleted: a real published standard beats an invented map. It is
**not** the map we ship on, and adopting it as one would put remote unlock on
`fill_light`.

An unpinned build warns loudly at boot, because with no fiction left, "unpinned"
means "we are guessing which lock is in the door". A lock that enrols reporting
`profile: "tuya-wifi-lock-pro"` is **unconfigured firmware**, and the app must
treat it as a pairing failure rather than a detected model (`XF-122 §9.3`).

*(Open, operator's call: the honest alternative is a default of no profile at
all, refusing every verb until pinned. It fails louder. Not changed.)*

### 6.4 `in_lock` — DPs 101/102/103 belong to every profile

`bond_revoke`, `invite_cancel`, `list_bonds`. These are **ours** — our BLE
bond-management channel, not Tuya datapoints — handled entirely inside the module
and **never forwarded to the MCU**. Being product-independent, they belong on
every profile.

🔴 **They lived only on `ozkie-legacy-v0`.** Deleting the fiction silently removed
bond revoke from every product; the tests caught it. They are now on all three
profiles. **101–103 are not reserved by Tuya**, so the allocation must be
re-verified against every new supplier's DP list (§5.6).

### 6.5 `models.json` — generated, for the app

`XF-122 §7` has the app show *"Detected: <model> — is this correct?"* at pairing,
so it must map a `tuya_pid` to a human model name. That is the same fact the DP
tables encode, so it is **generated from the same load pass** rather than
hand-copied into the app:

- carries **identity only** — PID, names, `pairable`, `dp_count`. No DP numbers,
  no capabilities: a specific lock's abilities come from the `verbs` array in its
  enrol payload (`XF-121`), from the lock itself.
- **`pairable` is derived** — a profile earns it by having a PID and a complete
  DP map, not by carrying a flag someone set.
- staleness is enforced: `gen_profile.py --check` covers both outputs and is the
  first thing `npm test --prefix locksim` runs.

---

## 7. Sources

| | |
|---|---|
| `docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf` | **Luona Smart** — Smart Lock DS013-T3, PID `vr4iiuqtyh0q4nix`, Tuya-generated 2026-08-11. 34 DPs with types, ranges and enums. The primary witness. |
| `docs/DPSuppliers/T3_Final_Customer_Version_EN.docx` | **Ladin Tech** — T3-U module integration document. No DP table; names DP 42/76/149/212 and documents the wake handshake. |
| *(merged, file deleted)* | The operator's independent hand extraction of the Luona table, 2026-08-18 — the source of the English function descriptions in §3. Agreed with the catalogue row for row; merged here so there is only one copy to keep current. |
| Tuya developer platform, public pages | [Wi-Fi Lock Pro DP Reference](https://developer.tuya.com/en/docs/iot/wifi-dp?id=K9fewhnlvk6by) · [Residential Lock DP Reference](https://developer.tuya.com/en/docs/iot/zigbee-doorlock-dp?id=K9fembhbeab0p) — §1's per-category collisions, and the transcribed `tuya-wifi-lock-pro` reference map. |
| `XF-120` | Sealed unlock, and §8: DP 76 opening a real door end to end. |
| `XF-122` | Lock-model selection at pairing; §9 the one-pairable-model finding and `models.json`. |
| `docs/ozkey-27.md` | The findings behind the catalogue/selection design; §2.5 on `reserved`. |
| `docs/ozkey-28.md` | OZKIE v1 verbs — the `verb`/`field` columns. |
| `docs/ozkey-39.md` | 🔴 The two suppliers were previously conflated as one. Module identity, DP selection and the wake handshake are **per-vendor**. |
| `docs/ozkey-42.md` | The supplier request. §2 is the Luona ask; P0 is the 15 RAW credential layouts. |
| `profiles/README.md` | Resolution order, entry shape, the `status` field. |

🔴 **The standing caveat, narrowed.** As of 2026-08-20 a **DP census over command
`0x08` has been run against the bench MCU and matched 34/34**, and remote unlock
on DP 76 has been driven end to end. That is real confirmation of the *catalogue*
and of the *unlock path*. It is **not** confirmation of the supplier's own
hardware: LockSim is still the MCU on our bench, and no real Tuya DL-MCU has ever
been on our wire. Every `reserved` row — every credential write — remains
unconfirmed by anything but supplier paper.
