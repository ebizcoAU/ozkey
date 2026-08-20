# ozkey-42 — Supplier request: the RAW byte layouts for DP 9, 10, 13–19, 54, 74, 86–89 (Luona Smart, DS013-T3)

**Status: 🔴 OPEN — ACTION REQUIRED BY PM/OPERATOR.** Written 2026-08-20 by
**firmware**. This is `XFtposDecisions-118` **P0**, the critical path for the
entire credential and remote-unlock feature set.

**Addressee: Luona Smart** — DS013-T3, PID `vr4iiuqtyh0q4nix`, source document
`docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf`.
**NOT Ladin Tech** — that is a different vendor and a different module
(`ozkey-39 §0`); sending this to the wrong one wastes a round trip we cannot
afford at this lead time.

Companion docs: `ozkey-39 §2` (which proves the block and reframes the ask),
`ozkey-27 §7 Q2` (where it was first raised as an absence), `ozkey-28`
(the OZKIE verb spec these DPs must satisfy), `XFtposDecisions-118` (the test
plan this unblocks).

---

## 1. 🔴 What NOT to ask for

**Do not request "the protocol document." We already have it**, and asking for
it will get us the same PDF back and cost us weeks.

`ozkey-39 §2` established this from a positive citation, not an inference. The
supplier's instruction table has a 功能指令 (function instruction / payload)
column. For **DP 11 连接模式** it is filled in completely — 数据长度 `0x0005`,
类型 `0x04` (enum), 功能长度 `0x0001`, values `keep:0x00` `sleep:0x01`
`lock_keep:0x02` `lock_sleep:0x03`. For the DPs we need, the same column reads
`0x00-0xff`.

DP 11 proves the document *can* fully specify a payload when it intends to.

> **The layout is not missing from a document we have failed to find. It is
> content the supplier chose to leave open.** So the ask is for *content*, not
> for a document.

## 2. What we are asking for, precisely

The **byte layout** of the RAW payload for each DP below — field order, width,
endianness, encoding, and the meaning of each value.

### 2.1 🔴 Priority A — credential CRUD (blocks every PIN, RFID and card feature)

| DP | name | why we need it |
|---|---|---|
| 13 | `bulk_unlock_method_add` | issue an RFID card / unlock method |
| 14 | `bulk_unlock_method_delete` | revoke one |
| 15 | `bulk_unlock_method_modify` | change validity window |
| 16 | `bulk_password_add` | **issue a PIN — the single most-used feature we have** |
| 17 | `bulk_password_delete` | revoke a PIN |
| 18 | `bulk_password_modify` | change a PIN's window |
| 19 | `bulk_unlock_method_sync` | reconcile after offline changes |

The catalogue calls this codec `bulk_method_v1`. One coherent answer for the
family is more useful than seven separate ones.

**Specific questions we cannot answer from the document:**

1. Slot identifier — width and range? Is it assigned by the module or by us?
2. Credential body — for a PIN, ASCII digits or BCD? For a card, UID byte order?
   (We have already lost time to exactly this: hex-decoding a PIN that was ASCII
   silently discarded every grant — `pin-grant-cred-encoding`.)
3. Validity window — the profile records per-credential effective + expiry
   datetime **in GMT**, use-count (`0` unlimited / `1` one-shot) and status.
   Confirm the field order and the epoch/format on the wire.
4. 全量下发 (full re-send): the profile records that the multi-group password
   service is re-sent **in full** every time. Confirm, and state the maximum
   number of entries in one frame and what happens on overflow.
5. What does the module report back on success and on failure, and on which DP?

### 2.2 🔴 Priority B — remote unlock

| DP | name |
|---|---|
| 9 | `remote_no_pw_unlock_setting` |
| 10 | `remote_unlock` |

**DP 10 is the real remote-unlock DP.** Our current implementation uses a
fabricated DP 1 BOOL that no shipping lock implements (`XFtposDecisions-110`),
so every remote unlock we ship today is wrong for real hardware.

Question: is DP 10 a bare trigger, or does it carry an authorisation token /
credential id / nonce? The `0x00-0xff` notation does not distinguish "one
arbitrary byte" from "a structure we must build."

> **Note for the supplier:** we may not need this at all if **DP 76
> `unlock_ble`** is issuable by the module — see `ozkey-39 §3.5`. If DP 76 is
> the supported path for app-initiated unlock, say so and Priority B drops to
> informational. **Please answer this question first; it is the cheapest one
> here and it may remove the rest of Priority B.**

### 2.3 🟡 Priority C — offline credentials and records

| DP | name |
|---|---|
| 86 | `offline_password_params` |
| 87 | `offline_password_clear_single_report` |
| 88 | `offline_password_clear_all_report` |
| 89 | `offline_password_unlock_report` |
| 54 | `device_info` |
| 74 | `unlock_combination_record` |

Offline PINs matter to us specifically: our locks must work with the network
down, so a credential the module can validate without us is worth more than it
would be in a cloud-first product.

For 86, we need the derivation — how an offline password is generated so that a
lock can validate it without ever having been told about it.

## 2.4 🔴 DEMONSTRATED ON HARDWARE, 2026-08-20 — issuing a PIN today

Not reasoning. Run on the bench: BANOI1 -> LockA (`doorlock-1.99`, Thread) ->
LockSim (Mode B, real Tuya UART) with the operator watching both ends.

**Result: the operator issued a PIN from the app. It did not work. The
hardcoded master PIN `123456#` did.**

The grant left the app and was accepted by the lock — visible on the wire as a
sealed envelope:

```
04:59:17  ozkie/lab/bridges/ozb-98a316a7e638/command
          {"msg_id":"ozl-499-…","target":"ozk-acebe639f8c4","envelope_hex":"0200…"}
```

and then stopped there. It never reached the DL-MCU, which is the thing that
actually reads the keypad. LockSim's credential registry still showed only
slots written on 13-15 Aug, all expired, and no new slot.

`123456` works because it is hardcoded IN the DL-MCU and needs no provisioning
— faithful to real hardware, where keypad and credential store both belong to
the MCU. That is precisely the contrast: **the credential that never has to
travel works; the one that must travel cannot.**

### 2.4.1 🔴 The part that matters commercially: the real DP map is a REGRESSION

Checked across every profile in `profiles/products/`:

| profile | legacy cred DPs (21-24) | real cred DPs (13-19) | issuing a PIN |
|---|---|---|---|
| `ozkie-legacy-v0` (our fiction) | **21, 22, 23, 24** | — | **works** |
| `ozsim-fullfeature` | — | — | silent no-op |
| `tuya-ds013-t3` (real supplier) | 21, 23, 24 → **volume / auto-lock** | 13-19, all reserved | silent **misfire** |
| `tuya-generic-lock` (real supplier) | 21, 23, 24 → **volume / auto-lock** | 13-19, all reserved | silent **misfire** |

Read that bottom row carefully. Firmware's only implemented credential path is
legacy DP 21. On the real supplier map **DP 21 is `navigation_volume`** — and it
IS selected by the profile, so nothing rejects it. Issuing a PIN against a real
DS013-T3 would push the credential onto the **volume control**, and both ends
would report success.

> **Moving from our invented DP map to the supplier's real one currently makes
> credentials WORSE, not better.** The fiction can express a credential write.
> The real map cannot — the DPs that could are the 15 reserved ones in §2.1, and
> the DPs we have implemented mean something else entirely on real hardware.

This is not a firmware defect we can engineer around. Reads are already correct
and complete on the real map (access events, doorbell, battery, bolt state all
verified flowing on the bench the same session). **Writes fall off a cliff the
moment we stop pretending**, and only the supplier can end that.

---

## 3. Why this is the critical path

Everything below is blocked, and **no amount of our own work unblocks any of
it**:

- Every PIN operation, for every product we ship.
- Every RFID / card operation.
- Correct remote unlock on real hardware (we currently ship a fiction).
- Offline passwords.
- The `XFtposDecisions-118` test plan's Tests 4 and 5 — currently untestable.

Of the 34 DPs our generic profile selects, **19 are `confirmed` and 15 are
`reserved`**, and all 15 are reserved for this one reason. The split is not
random: the confirmed 19 are the **reads**, the reserved 15 are the **writes**.
We can currently observe a lock in full and command it almost not at all.

## 4. What we are NOT blocked on — so nobody waits on this unnecessarily

The read path is complete and needs nothing from the supplier. Access events
(DP 61/63/64/72/73/76), doorbell (DP 53), battery (DP 45), bolt state (DP 47),
alarm (DP 60), hijack (DP 98) are all `confirmed` and already flow end to end
through firmware. Firmware work continues on those in parallel — see
`XFtposDecisions-118 §8`.

## 5. Ask

1. 🔴 **PM/operator** — send §2 to **Luona Smart**. Lead with §2.2's DP 76
   question; it is one line and may delete a third of the request.
2. **PM/operator** — while raising it, confirm whether the same request should
   go to **Ladin Tech** for the T3-U. `ozkey-39 §4`'s open question — *which
   manufacturer is SIMLOCK actually for* — is still unanswered, and it decides
   whether we need one codec or two.
3. **firmware (me)** — nothing here is blocking my current work. On receipt I
   own the `bulk_method_v1` codec in `profiles/` so LockSim and firmware cannot
   diverge on it, the way they currently have on DP 60.
