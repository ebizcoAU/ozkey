# ozkey-47 — Credential store design: PIN / RFID / fingerprint, for our own in-house firmware

**Status: 🟡 Approved by System Architect .** Written 2026-09-02 by firmware, at the operator's
request, following a discussion of why `XFtposDecisions-128` blocks PIN
provisioning on the Tuya-based `tuya-luona-ds013-t3` product and why that
blocker does not carry over to the CH584M/OZKIE-native line, where we own
the firmware end to end.

## The one-line version

The crypto/authorization layer that decides *whether* a credential write is
allowed is already built, hardware-agnostic, and unchanged by any of this.
What's actually being designed here is small: a keyed table mapping a slot
to a credential value and a validity window, plus a per-credential-type
comparison function. PIN and RFID already exist in this exact shape,
live-tested on the CH584M bench (`dlch584m-0.18`+). Fingerprint is the one
genuinely new piece, and it is not a bigger version of the same table — it
needs a sensor peripheral and a different (probabilistic, not equality)
comparison, scoped separately below.

## Why XF-128 doesn't apply here

`XFtposDecisions-128` found that PIN provisioning was unimplementable on
`tuya-luona-ds013-t3` because the real credential-write DP (DP 16,
`bulk_password_add`) is `reserved` — the supplier documents that the DP
exists but never supplied the raw byte layout of its payload. That is a
**vendor-documentation gap**, not a design or engineering gap: there is no
correct DP frame to send because nobody outside the vendor knows what one
looks like.

CH584M/OZKIE-native has no DP catalogue in this path at all. Credential
writes are a semantic verb (`{"kind":"grant_pin","slot":N,"cred":hex,
"from":ts,"to":ts}`, and the `grant_rfid`/`delete_pin`/`delete_rfid`
mirrors) written straight into a table we define ourselves. There is no
external spec to be missing, because there is no external protocol —
`ozkey-27`/`ozkey-42`'s "RAW payload layout not supplied" question simply
does not exist on this line.

## What already exists, confirmed live (not proposed)

- **Wire shape**: `grant_pin`/`grant_rfid`/`delete_pin`/`delete_rfid`,
  sealed the same way every other OZKIE verb is (`XFtposDecisions-128 §12`
  — ftpos moved all four callers off hand-built DP frames onto this shape).
- **Encoding, settled after one real bug**: PIN `cred` is the literal ASCII
  digit string; RFID `cred` is hex of the raw UID bytes.
  `XFtposDecisions-140` found and fixed a mismatch where the app was
  hex-encoding the PIN's ASCII bytes on top of firmware's own ASCII
  expectation — a double-encode that both sides' own tests missed because
  neither exercised the other's real code. Fixed on the app side; keep this
  asymmetry (ASCII for PIN, hex for RFID) in mind for fingerprint too —
  don't assume every credential type wants the same encoding.
- **Storage**: a bench credential store, NVS-persisted on the CH584M
  simulator (`dlch584m-0.18`+, `XFtposDecisions-139`), read-only `creds`
  dump on the bench console, fail-closed on an expired validity window.
- **Authorization**: unchanged from every other OZKIE verb — sealed
  envelope, bond, challenge, counter floor, credential-issuance verbs
  additionally gated `viaBle` refused (`XFtposDecisions-139`'s XF-139
  fix) so a credential can only ever be granted over the authenticated
  server/MQTT relay path, never a bare BLE session holding a valid bond.
  This gate is **not** something this doc proposes changing.
- **Reporting**: `pin_sync`/`rfid_sync` in the lock's own `caps`/`verbs`
  report, relayed to the server via the same mechanism as audit/reset/time
  (`XFtposDecisions-139`), translated to the app-facing `cred_pin` name at
  the server's `effectiveCaps()` layer. Never inferred — device-reported
  only (`XFtposDecisions-128 §10`'s rule, already applied).

None of the above is being redesigned. It's listed so the actual proposal
below is legible against what's already load-bearing.

## The actual design surface: the slot table itself

```c
struct OzCredSlot {
  uint8_t  slot;                 // stable index, app/server address by this
  uint8_t  type;                 // OZ_CRED_PIN | OZ_CRED_RFID | (future) OZ_CRED_FINGERPRINT
  uint8_t  value[OZ_CRED_MAX_LEN]; // ASCII digits for PIN, raw UID bytes for RFID
  uint8_t  valueLen;
  uint32_t validFrom, validTo;   // fail-closed outside this window
  bool     revoked;
};
```

This is, deliberately, almost exactly the bench store that already exists
— the proposal is to promote it from a bench-only NVS table to the real
persistent store on production CH584M firmware, once real silicon is on
hand to test against. Nothing about the *shape* changes; what changes is
where it lives (real flash, not a dev-kit standing in for one) and that it
needs to survive the product's actual OTA/versioning story, which the bench
store never had to.

**Open question, not decided here**: slot count and total flash budget on
real CH584M silicon (512KB flash, 448KB user app per the original vendor
spec, `XFtposDecisions-138`'s directive doc) — needs a number once real
hardware is in hand to measure against, not guessed now.

## Comparison logic, per type

- **PIN**: exact match against `value`, plus the validity window. Already
  built, already tested.
- **RFID**: exact match against the raw UID bytes, plus the validity
  window. Already built, already tested.
- **Fingerprint — genuinely different, scoped separately, not designed
  here**: a fingerprint is not compared for equality against a stored
  value. It needs (a) a sensor peripheral to capture a scan — hardware not
  currently in any bench or product BOM discussed in this repo, (b) an
  enrollment flow that stores a *template*, not a raw value, and (c) a
  matching algorithm that returns a confidence score, not a boolean —
  meaning a threshold/false-accept-rate decision becomes a real product
  question (security posture, not just code). This is a new peripheral
  integration plus a different verification algorithm, not an extra `type`
  value on the existing struct. Flagging as out of scope for this doc;
  worth its own design pass once/if fingerprint becomes an actual product
  requirement with real sensor hardware picked.

## What this doc is asking for

Nothing built yet. This is the shape check the operator asked for — PIN
and RFID confirm the "simple keyed DB" pattern already works end to end on
real (bench) hardware, under the same crypto/authorization path everything
else uses, and generalizes cleanly to a second exact-match credential type
already. Fingerprint doesn't generalize the same way and shouldn't be
assumed to — flagging that distinction now, before anyone plans a timeline
assuming it's the same size of work as PIN/RFID were.
