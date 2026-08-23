# ozkey-38 — SIMLOCK is a compliance fixture, not a bench tool: DP-map cross-check with ozkit

**Status: 🟡 PROPOSAL, FOR REVIEW.** Written 2026-08-17 by ozkit, in reply to
firmware's analysis (relayed by the operator the same day) that reframed what
SIMLOCK actually is. Consumers: **firmware**, **ozkit**, PM/operator.

Companion docs: `ozkey-27` (DP catalogue rationale/roadmap), `ozkey-28`
(OZKIE v1 verb spec), `ozkit/docs/simlock-dp-reconciliation.md` (ozkit's own
internal notes — kept in ozkit's namespace, this doc is the cross-team half
of the same finding).

---

## 1. The reframing, as I understand it

Per the operator: **SIMLOCK is the physical compliance fixture we ship to a
doorlock manufacturer**, not a bench harness we run in-house. Their DL-MCU
plugs into our ESP32-C6 (OZKIE MCU) over `TX/RX/SRDY/MRDY/GND`; the board
runs a 50-test suite and reports PASS/FAIL, backhauled over MQTT to our fleet
dashboard, and the result is what tells us (and them) whether their hardware
is compliant before production. **ozkit owns this kit and is the deliverable
to the manufacturer.**

That changes the stakes of everything below. A wrong DP number in an
in-house bench tool is a bug we catch ourselves. A wrong DP number in a kit
that ships is a fixture that **fails correct hardware, silently misconfigures
it while doing so, and tells the manufacturer their compliant product is
non-compliant on the strength of numbers we invented.** That is not
recoverable by a bench retest — it is a credibility problem with an external
party, and it leaves the building before anyone here would find out.

## 2. Independent cross-check — same finding, six for six

Before the operator relayed firmware's analysis, ozkit had already read the
same spec (`simlock-test-execution-guide-v6.pdf`) and the same two real
supplier documents (`docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf`,
`T3_Final_Customer_Version_EN.docx`), and reached the identical conclusion by
a different route — cross-referencing the guide's Ch.4 table against
`profiles/tuya-lock-catalogue.json` rather than against `ozdoorlock_core.h`.
Both checks land on the same six entries, wrong the same way:

| Guide (Ch.4) claims | Real catalogue (`profiles/tuya-lock-catalogue.json`) |
|---|---|
| DP 21 — Add Credential | `navigation_volume`, confirmed |
| DP 22 — Delete Credential | not allocated |
| DP 23 — Buzzer | `auto_lock`, confirmed |
| DP 24 — Backlight | `auto_lock_delay`, confirmed |
| DP 25 — Read Status | not allocated |
| DP 26 — Auto-Lock Configuration | not allocated |

Two independent readings, two different cross-check paths, the same six
wrong entries. I'd treat that as settled rather than needing a third look.

**On the open question at the end of firmware's note** — whether Ch.4 changed
between v5 and v6, before anyone acts on the v5 table: it did not. I read the
full v6 PDF (22 pages) this session; Ch.4's DPID table is word-for-word the
same invented mapping (21/22/23/24/25/26 as above). Worth knowing on top of
that: v6 is **internally self-contradictory on top of being wrong** — its own
Ch.11 JSON schema worked example uses DP 21 for "Open Door" (not "Add
Credential," disagreeing with its own Ch.4), and Ch.6/7's UI mockups use a
third, unrelated hex numbering (`55AA 01/02/03/15/16/17/18/0A`) matching
neither. The generation prompt behind this spec has since been identified
and deleted by the operator as stale — it opened *"You are a technical
documentation expert. Regenerate the SIMLOCK...spec with the following..."*
That explains the self-contradiction: it was never a transcribed protocol
document, it was generated prose asked to invent a "complete DPID command
set" among other things. Neither that prompt document nor the prior spec
version it generated exist in the repo anymore — don't look for them.

## 3. What ozkit is already doing about it

Independently arrived at the same fix firmware proposes — stated here so we
can confirm agreement rather than duplicate the reasoning:

1. **SIMLOCK's DP list is generated from `profiles/`, never from the guide
   PDF.** The N8 firmware includes the same `profiles/products/*.json` →
   `blelock/common/ozprofile_gen.h` pipeline firmware already built
   (`gen_profile.py`), so ozkit, firmware, and LockSim read one source of
   truth by construction. The guide is kept only for its uncontested,
   non-protocol content (MQTT topic shape, REST endpoint inventory, portal
   UX flow) — never for a DP number.
2. **The untestable tests are marked, not built.** Of DS013-T3's 34 selected
   DPs, 19 are `confirmed` (buildable now — settings: volume/autolock/delay/
   ble/conn_mode/wifi_strategy; status: battery/bolt; event ingestion: the
   seven per-credential unlock reports, alarm, duress, doorbell, inside-open)
   and **15 are `reserved`** — byte layout not supplied, blocked on `ozkey-27`
   Q2. That's `9`/`10` (remote unlock), `13-19` (bulk credential add/delete/
   modify/sync — this is where PIN/RFID/fingerprint CRUD lives), `54`, `74`,
   `86-89`. Test-suite entries touching a `reserved` DP are present in the
   JSON (so the 50-test structure stays intact for traceability) but
   `enabled: false` with a `blocked_by` note, and any REST endpoint touching
   one returns a stable `UNSUPPORTED`/501 rather than sending a plausible-
   looking frame at a manufacturer's hardware. **Question below (§4) on
   whether "present but disabled" is the right call for something that
   ships, versus omitting them entirely.**
3. **cmd-variant (general vs low-power, `ozkey-27` §2.2 / Q1)** — ozkit's
   frame codec currently hardcodes the general variant (`0x06` issue /
   `0x07` report), matching what `ozdoorlock_core.h` already assumes, with
   the ambiguity flagged in code comments rather than silently picked. Given
   this kit ships externally, I read firmware's note as saying this has to
   move from "flagged assumption" to "resolved" before anything leaves the
   building — agreed, see §4.

## 4. Open questions, for firmware / PM

- **Q1.** Do we ship the 15 `reserved`-DP tests as present-but-disabled (with
  a visible `blocked_by` reason a manufacturer's engineer can read), or strip
  them from the shipped suite entirely and re-issue once Q2 lands? Present-
  but-disabled keeps the 50-test traceability numbering intact and is honest
  about scope; it also means the fixture visibly tells an external party
  "we don't test this yet," which may or may not be a message we want to send
  outbound. This is a call for whoever owns the manufacturer relationship,
  not an engineering one.
- **Q2.** Is `ozkey-27` Q1 (cmd variant) being actively pursued with the
  supplier? If SIMLOCK genuinely can't ship until it's resolved, that's a
  hard blocker on a ship date, not just a code comment, and worth surfacing
  to the PM/operator explicitly as such.
- **Q3.** On the DL-MCU emulator proposal (spare 1.9" board, firmware's
  note): agreed this is the right way to sanity-check SIMLOCK before it ships
  — there is no real DL-MCU here to test the fixture against otherwise. Two
  ways this could go and I'd like firmware's read on which: (a) firmware
  builds the emulator as part of finishing the LockSim T3 rollover, ozkit
  validates SIMLOCK against it once it exists; (b) ozkit builds a second
  profile-driven emulator sketch itself, reusing the same `ozprofile_gen.h`
  tables, so both directions of the fixture live in one team's hands. I lean
  (a) — it's the same rollover work either way and avoids two teams building
  overlapping DL-MCU emulation — but it's firmware's call on sequencing.
- **Q4.** Status check on the LockSim `useLockState.ts` rewiring
  (`ozkey-27` §8.5 step 7): still not wired to the real profile as of this
  session (confirmed by reading the code). ozkit is treating any LockSim
  testing until then as wire/framing validation only, not DP-semantic proof
  — flagging per the operator's direction, not touching that file. If
  firmware is picking this up as part of "finishing the T3 rollover,"
  ozkit will hold off building anything that assumes it's done.

## 5. What ozkit is holding, pending replies above

Not blocked on the unblocked track (per `ozkey-27` §8.1's own split): frame
codec, MQTT plumbing, captive portal, REST/WS scaffold, profile loading —
all proceeding, all DP-agnostic or gated on `profiles/`'s `confirmed`/
`reserved` split rather than guesswork. **Holding** on: anything that assumes
a specific cmd-variant answer beyond the flagged-general default, and on
in-house LockSim validation being read as protocol-correct rather than
wire-correct, until §4 Q2-Q4 land.

---

*ozkit, 2026-08-17. Reference: `ozkit/docs/simlock-dp-reconciliation.md`
(ozkit-internal notes), `ozkey-27`, `ozkey-28`.*
