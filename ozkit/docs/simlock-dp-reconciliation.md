# SIMLOCK — DP reconciliation and architecture notes

**ozkit-internal.** Not part of the shared `docs/ozkey-NN.md` firmware/server
channel — kept here so ozkit's design notes stay in this namespace. Written
2026-08-17, first real build session after the 2026-08-15 planning-only
session.

> 🔴 **2026-08-21 addendum.** Firmware's fleet restructure (`XF-122`,
> 2026-08-20) renamed `profiles/products/tuya-ds013-t3.json` to
> `tuya-luona-ds013-t3.json` and deleted `ozkie-legacy-v0`/`tuya-generic-lock`
> entirely — every `tuya-ds013-t3` reference below is the **old id**.
> `simlock.ino` was silently falling back to the new default profile
> (`tuya-wifi-lock-pro`, a different Tuya product category with different DP
> meanings — e.g. its DP76 is `fill_light`, not `unlock_ble`) until this was
> caught and fixed the same day. Current handoff doc:
> `simlock-handoff-guide.md`. Also resolved since this was written: the cmd
> variant (`ozkey-39` — confirmed GENERAL, 0x06 issue/0x07 report, which is
> what this fixture already implements) and the vendor question in §102 below
> (Luona confirmed as SIMLOCK's real target; Ladin remains an unusable stub).

---

## 1. The generated spec is not a real protocol document

`ozkit/docs/simlock-test-execution-guide-v6.pdf` was produced by an LLM prompt
document that opened with *"You are a technical documentation expert.
Regenerate the SIMLOCK_GEEK_DLMCU_Technical_Specification.docx with the
following complete specifications."* Both that prompt document and the prior
generated spec version have since been deleted by the operator as stale —
neither exists in the repo anymore, and neither should be cited or looked
for going forward.

The v6 PDF's protocol-level content is internally self-contradictory:

| Where | DP21 is claimed to mean |
|---|---|
| Ch.4 DPID table | "Add Credential (PIN/RFID/Fingerprint)" |
| Ch.11 JSON schema worked example | Open Door / Unlock (`payload_hex: "01"`) |
| Ch.6/7 UI screenshots' hex mockups | Neither — those use a third ad-hoc numbering (`55AA 01/02/03/15/16/17/18/0A`) not matching Ch.4 or Ch.11 |

It also specifies a 5-wire **MRDY/SRDY active-low hardware handshake**
(N8=master, DL-MCU=slave) between the N8 and the DL-MCU. The real Tuya
low-power UART protocol (below) has no such handshake — it's plain UART
(TX/RX/GND) with software framing, checksums, and request/response timeouts.

**Conclusion: do not build against the v6 PDF's DPID table or its MRDY/SRDY
handshake description.** Its non-protocol content — MQTT topic shape, REST
endpoint inventory, portal/provisioning UX flow, 50-test traceability
structure — is architecturally sound and independently corroborated (see §4),
safe to build against.

## 2. The real DP catalogue

Two real supplier documents, supplied this session (`docs/DPSuppliers/` at
repo root, not ozkit-local):

- **`protocol_vr4iiuqtyh0q4nix_20260811.pdf`** — Tuya 低功耗通用串口接入协议
  (low-power general serial access protocol) for **Smart Lock DS013-T3**,
  PID `vr4iiuqtyh0q4nix`. A genuine, internally-consistent spec: frame =
  `0x55 0xAA` + version + command byte + 2-byte-BE length + payload + checksum
  (sum mod 256); DP unit = `[dpid 1B][type 1B][len 2B BE][value]`. Full DP
  catalogue table included.
- **`T3_Final_Customer_Version_EN.docx`** — the **T3-U module** (Beken
  BK7236) hardware/software integration guide. Not a second DP list — a
  different layer of the same solution (the radio module vs. the serial DP
  protocol). Confirms a subset of the same DP numbers (DP76=`unlock_ble`,
  DP42=BLE-when-WiFi-down), cross-validating the catalogue as a real Tuya
  standard rather than one vendor's invention.

Firmware had already reconciled these into machine-readable form at repo
root — `profiles/tuya-lock-catalogue.json` (the shared DP catalogue) and
`profiles/products/tuya-ds013-t3.json` (this product's DP selection) — under
their own design docs `docs/ozkey-27.md` (rationale) and `docs/ozkey-28.md`
(normative OZKIE v1 verb spec). ozkit reads those files as data and reads
those docs as background, but keeps its own build notes here.

### Confirmed vs. reserved, DS013-T3 (34 selected DPs)

**19 `confirmed`** — known type and semantics, safe to build against:

| DP | Name | Type |
|---|---|---|
| 11 | connection_mode | enum (keep/sleep/lock_keep/lock_sleep) |
| 21 | navigation_volume | enum (mute/low/normal/high) |
| 23 | auto_lock | bool |
| 24 | auto_lock_delay | value (5–1800s) |
| 42 | ble_switch | bool |
| 45 | battery_percentage | value (−1..100) |
| 47 | bolt_state | bool |
| 52 | opened_from_inside | bool |
| 53 | doorbell | bool |
| 60 | alarm | enum, 18 values (pry/low_battery/unlock_attempt/etc) |
| 61/63/64/69/72/73/76 | unlock_password/fingerprint/card/temporary/remote/remote_voice/ble | value (cred_id) — one DP per credential class |
| 98 | hijack_alarm (duress) | bool |
| 156 | wifi_connection_strategy | enum (dtm20/dtm10) |

**15 `reserved`** — type/max-len known, **byte layout not supplied by the
supplier doc** — cannot write a codec, must not guess:

| DP | Name | Blocked on |
|---|---|---|
| 9 | remote_no_pw_unlock_setting | raw, layout unknown |
| 10 | remote_unlock | raw, layout unknown — **this is the literal "open door" command** |
| 13/14/15 | bulk_unlock_method_add/delete/modify | raw, layout unknown |
| 16/17/18 | bulk_password_add/delete/modify | raw, layout unknown — **this is the real add-PIN path** |
| 19 | bulk_unlock_method_sync | raw, layout unknown |
| 54 | device_info | raw, layout unknown |
| 74 | unlock_combination_record | raw, layout unknown |
| 86/87/88/89 | offline_password_params / clear / clear-all / unlock-report | string/raw, layout unknown |

All 15 are tracked as `ozkey-27` Q2 (a supplier question, not ours to guess
at). **This is the real scope boundary for the SIMLOCK test suite** — not "a
dozen DPs," but exactly these 19 vs. these 15.

## 3. This session's architecture decisions

- **Per-manufacturer separation.** Each N8 dev-kit build targets exactly one
  manufacturer's profile from `profiles/products/*.json` — no shared/merged
  DP map across boards. First target: `tuya-ds013-t3`.
- **In-house test target = LockSim**, not physical hardware, for now.
- **MQTT broker = `mqtt.ebizco.com.au`**, port 8883 TLS — a dedicated ozkit
  broker. (Not the lab broker `10.1.1.20:1883` that `ozlockserv`/
  `ozlodgeserv`/`ozpmsserv` use — deliberately kept separate.)
- **LockSim's `locksim/hooks/useLockState.ts` is still hardcoded to the old
  invented DP map** (`DpId.ADD_TEMP_PIN` etc.), not wired to the real
  profile's `dispositionFor()` — confirmed by reading the code this session.
  ozkit is **flagging this, not fixing it** — `locksim/` ownership between
  ozkit and firmware has never been settled, and this file lives in that
  shared directory. Practical consequence: pointing N8 firmware at LockSim
  today proves wire/framing correctness only, **not** real DP semantics,
  until whoever owns that file re-points it at the profile.
- **Frame codec reuse.** N8 firmware (`blelock/simlock/simlock.ino` +
  `tuya_frame.h`) mirrors `blelock/common/ozdoorlock_core.h`'s Tuya frame
  build/parse/checksum functions byte-for-byte (can't `#include` that file
  directly — it needs per-board macros this bare test-master doesn't have)
  and includes the generated `blelock/common/ozprofile_gen.h` PROGMEM DP
  tables directly (compiled from `profiles/` by the already product-agnostic
  `blelock/tools/gen_profile.py`) — one source of truth for the DP catalogue,
  a deliberately-mirrored-not-diverged copy of the frame codec.

## 4. What corroborates the MQTT/REST/portal design (kept, not fiction)

The v6 PDF's Ch.10 MQTT topic contract was independently corroborated by a
second design document (since deleted as stale, same topic names and payload
shapes) — genuine agreement between two documents tracing back to the same
design intent, not one hallucinating from the other's boilerplate. Topics:
`test/boards/{id}/status|test/start|test/result|
test/complete|log|alert|ota/status|connection`, inbound `+/command`. This
part is safe to build as specified.

## 5. Full build plan

See the session plan for the phased build (firmware skeleton → REST/WS +
test-suite runner → MQTT telemetry → backend server → LockSim validation →
handoff docs). This doc is the reference the plan and all future ozkit work
should cite for "which DP, which status" instead of the v6 PDF's Ch.4 table.
