# ozkey-44 — DL-MCU (CH584M) trust boundary: sovereignty risk assessment

**Status: 🟡 ASSESSMENT — informs vendor questions and firmware hardening,
does not block anything in flight.** Written 2026-08-28 by **firmware**, at
the operator's request, following the CH584M+ESP32-C6 architecture lock
([[ch584m-network-coprocessor-architecture-locked]] in memory) and the
operator's framing: *"As a sovereign architecture, the DL-MCU firmware
cannot be ignored. We need total control — give a risk [assessment] where
malicious code in DL-MCU can break my ESP32 firmware, contact its own
server, and bypass my security to have its own control of the doorlock."*

Companion docs: `blelock/CONTRACT.md` (the BLE contract this assessment
checks against), `docs/ozkey-27.md` (DP catalogue provenance),
`docs/ozkey-39.md` §1.1 (the DP1-fiction precedent for "the bench agreeing
with itself"). Evidence below is cited to `blelock/common/ozdoorlock_core.h`
line numbers as of this commit; re-verify before citing in a later session.

**🔴 UPDATE 2026-08-28, same day, later in the conversation this doc came
from: the architecture has since moved past what §1-3 below assume.**
ESP32-C6 has been dropped from the doorlock entirely — no UART link, no
ESP32 on the lock board at all. The doorlock is now BLE-native (CH584M's
built-in BLE, or BF5885AM64 + an external BLE slave module), talking
directly to a bridge. §1-3's "UART trust boundary" framing is now the
BLE-link trust boundary instead — the mechanism differs but the underlying
question (can the DL-side chip's firmware exceed what we authorize, and can
we audit/control it) is unchanged and sharpened by §2.8 below. Operator's
direction as of this update: **"we may take control of the firmware on
CH584M or BF5885AM64, i.e. develop our own firmware"** — which resolves
§2.1's original open question (does OZKIE's crypto get a home on the
lock-side chip) in the strongest possible direction, IF it happens.

---

## 1. The headline answer

**Yes — by construction, not by bug.** The DL-MCU (CH584M) is not a
peripheral we command and verify; it is the credential-validation AND
motor-drive authority for every LOCAL unlock method (keypad, RFID,
fingerprint). ESP32/OZKIE's X25519 bond + envelope crypto governs the
REMOTE/BLE-from-app path, but it was never in the loop for local methods —
that boundary already existed under the old piggyback model. What's NEW and
genuinely concerning in the vendor's stated architecture is **CH584M
running its own "local BLE"**, a claim we have not yet gotten clarified
(§2.1) — if that is an independent, Tuya-stock pairing stack, it is a
second, unaudited door into the same motor, that never touches ESP32 or
our crypto at all.

The sovereignty question is therefore not "can we stop the DL-MCU from
having unlock authority" — we can't, and for legitimate local methods we
don't want to, that's the entire point of local access working offline.
The real questions are: (a) does it have an unaudited REMOTE-reachable
door into that authority (§2.1), and (b) can we detect, rate-limit, and
audit when it exercises that authority outside what we expect (§2.2-2.4).

## 2. Risk catalogue

### 2.1 🔴 UNCONFIRMED, HIGHEST SEVERITY — CH584M's "local BLE" may be an independent, unaudited control channel

The vendor's message states plainly: *"CH584M: Handles the touch keypad
(12+ keys), internal RFID, motor driver, and **local BLE**."* Today, under
the piggyback model, the ESP32 owns the ONLY BLE identity the lock has —
the GATT service and `control` characteristic `blelock/CONTRACT.md`
defines, gated by bond #0 and the M3/M4 challenge (`ozcrypto.h`). If
CH584M *also* runs a BLE radio/stack, one of two things is true, and we do
not yet know which:

- **(a) Best case**: "local BLE" is Tuya's low-power BLE *advertising/mesh
  assist* used only for factory pairing to the Tuya Smart Life app during
  manufacturing test, and is disabled or unreachable once our firmware
  takes ownership. No new attack surface.
- **(b) Worst case**: "local BLE" is a live, standing Bluetooth stack
  (Tuya's stock consumer lock-pairing protocol, which the Tuya/Smart Life
  app and any BLE-capable phone can discover) that remains active in the
  field, independent of ESP32, with its own path to the motor driver. If
  so, **anyone with the Tuya app — or a clone of Tuya's published BLE
  protocol — can pair directly to the CH584M and command an unlock without
  ever touching OZKIE's bond, X25519 envelope, or M3/M4 challenge.** This
  would be a complete bypass, not a weakness in our crypto: our crypto
  would simply never be in the path.

**Action, not yet taken:** this needs a direct, written answer from the
vendor before anything else in this doc matters much. Ask: *does CH584M's
"local BLE" run independently of the ESP32-C6's BLE identity? Is it Tuya's
stock Smart-Life pairing stack? Can it be disabled, stripped, or
reconfigured so it never accepts an unlock command, leaving pairing
notification/telemetry only if anything?* Get this in writing, not "trust
us" — this is precisely the "total control" gap the operator flagged.

### 2.2 🟢 CONFIRMED BY CODE — the DL-MCU's reports are trusted as fact, not verified

`handleMcuFrame()` (`ozdoorlock_core.h:2425`) checksum-gates inbound
frames, then treats anything that passes as ground truth: *"MCU → server
translation: the module's actual job. ACCESS_RESULT becomes the door log
the servers already understand... anything else goes up raw as dp_report
so nothing is silently dropped."* There is no secondary confirmation that
an access-granted report actually corresponds to an OZKIE-authorized
event — the DL-MCU's word is the audit trail. This is not new-architecture
risk, it is a pre-existing property of the Tuya DP model, but it is
sharpened by the operator's "total control" framing: **a compromised or
malicious DL-MCU can write false history into our own audit log** —
report accesses that didn't happen, or omit ones that did — and firmware
has no way today to tell the difference from a genuine report.

Concrete precedent this already happened, unintentionally: the 2026-08-20
0x08-census bug (`ozdoorlock_core.h:2448`) — a DL-MCU status reply,
byte-for-byte indistinguishable from a real event, latched `has_doorbell`
and **opened the BLE pairing window** as a side effect of a diagnostic
query. Nobody had to be malicious for a DL-MCU frame to trigger a
security-relevant action nobody intended. A deliberately malicious DL-MCU
has strictly more room than that bug did.

### 2.3 🟡 PLAUSIBLE, MODERATE SEVERITY — UART flood as a denial-of-service on the ESP32

`tuyaWirePump()` (`ozdoorlock_core.h:3017`) drains `Serial1.available()`
in a single `while` loop each call. A DL-MCU sustaining a continuous byte
stream (garbage, or a flood of validly-checksummed frames) would keep this
loop busy for as long as bytes remain queued; if that outlasts the ESP32
task watchdog's tolerance, the likely outcome is a watchdog reset —
i.e. a DL-MCU can potentially reboot the ESP32 at will. Consequence:
**remote/BLE unlock and the audit-log relay to `ozlockserv` go down**,
while local keypad/RFID/fingerprint unlock — owned entirely by the
DL-MCU — keeps working. So this DoS hurts OUR visibility and remote
authority, not physical access, which was never dependent on the ESP32
in the first place. Not verified against real hardware; flagging as
plausible from reading the loop structure, not as a reproduced finding.

### 2.4 🟢 CONFIRMED SAFE (worth stating, so this doc doesn't overclaim) — no memory-corruption RCE found in the frame parser

`tuyaWirePump()`'s reassembly buffer is a fixed 128-byte array with
explicit bounds checks on both the running index (`bn < sizeof(buf)`) and
the declared payload length (`total > sizeof(buf)` rejects and resets).
A hostile length field cannot write past the buffer — worst case is a
dropped frame, not corrupted memory. I looked for this specifically
because "malicious code... can break my ESP32 firmware" most naturally
reads as memory-corruption/RCE, and on the current parser I did not find
one. This does not cover the full firmware image (only the DL-MCU-facing
ingest path was reviewed for this doc), and it says nothing about §2.1-2.3
above, which are trust/logic-layer issues a memory-safe parser does not
touch.

### 2.5 🟡 UNCONFIRMED — "contact its own server"

Neither CH584M nor (unverified) BF5885AM64 has a stated WAN radio in the
vendor's architecture — the vendor's own message frames CH584M's role as
BLE-only (peripherals + "local BLE"), with the ESP32-C6 sole owner of any
uplink (Thread SED, Wi-Fi held off). Read literally, CH584M **cannot**
reach an external server directly — it has no path there. Two indirect
channels remain open, both requiring more than the DL-MCU acting alone:

- **Via its own local BLE (§2.1) to a nearby phone** that itself has
  internet — a proximity relay, not a standing backdoor. Only matters if
  §2.1(b) is true.
- **Via our own ESP32 uplink as an unwitting relay**: `dp_unclassified`
  and `dp_report` payloads are already forwarded to `ozlockserv`
  (`ozdoorlock_core.h:3010`, shape-only for unclassified DPs, full value
  for classified ones). A malicious DL-MCU could encode data in fields we
  treat as legitimate telemetry (e.g. abuse the battery-percentage VALUE
  field's numeric range as a covert low-bandwidth channel) and have OUR
  OWN infrastructure carry it outward. Low bandwidth, and it would be
  carried to **our** server, not "its own" — but worth having
  `ozlockserv`'s ingestion treat DP payload values as untrusted input
  (bounds/range-check server-side, not just log them) rather than
  implicitly trusted telemetry, which ties to
  [[xf-105-log-endpoint-gone-query-events-exists]]'s general lesson about
  what the log/event pipeline should and shouldn't trust.

**I do not have datasheet-level confirmation of either chip's full radio
inventory** — flagging per [[state-uncertainty-explicitly]] rather than
asserting BLE-only as fact. Get written confirmation from the vendor (no
Wi-Fi/cellular/LoRa on either part) before treating this as closed.

### 2.6 🟡 Firmware supply chain — no source, no audit, no attestation

We do not have CH584M's firmware source and (as far as this assessment
knows) no reproducible-build or signed-firmware-hash arrangement with the
vendor. Consequence: we cannot verify that what's physically flashed on a
manufactured unit matches what we were shown or agreed to, and if the
part supports BLE-based OTA (common in Tuya reference designs — an app
pushes new DL-MCU firmware over the air), a unit's actual behavior could
change in the field without our knowledge, silently. This is the same
"physical reset = full takeover" class of problem
[[threat-model-bond-and-physical]] already names for OUR side of the
lock, extended to a chip we don't control at all.

### 2.7 Credential exposure, if DP 13-15 ever unblock

Ties to [[pin-writes-land-on-navigation-volume]]/XF-128: PIN/RFID/
fingerprint provisioning (`cred.put`) is currently BLOCKED because the
supplier never gave us the RAW payload layout for DP 13-15. If/when that
unblocks, the DL-MCU becomes a second place plaintext credentials live
(it must store them to validate keypad/RFID/fingerprint locally) — a
second attack surface for credential theft, separate from whatever the
ESP32/OZKIE side does. Not urgent (the DPs are blocked today), but worth
carrying into that unblock's design rather than rediscovering it then.

### 2.8 🔴 EVIDENCED, NOT JUST HYPOTHESIZED — a named backdoor precedent on BF5885AM64

§2.6 above was written as a hypothetical ("if... a unit's behavior could
change... silently"). It is not hypothetical for this specific chip.
Alessandro Tagliafierro's published analysis, *"IoT Security — A Backdoor
in Your Front Door"* (InTheCyber Posts,
`https://posts.inthecyber.com/iot-security-a-backdoor-in-your-front-door-e28d79158fc5`),
is a hardware/firmware teardown of a commercial smart lock built on **the
BYD Semiconductor BF5885AM64** — named explicitly as the device's main MCU,
running the application logic and peripherals (touch, RFID, voice — the
same "four-in-one" role our vendor is proposing it for). Findings, and
critically **none of them require any radio at all**:

- **An undocumented backdoor PIN left in the firmware** — enterable on the
  keypad by anyone who knows it. Local only, no network channel involved.
- **NFC/RFID "authentication" reduced to a bare UID comparison** — no
  cryptographic challenge, trivially cloned/spoofed. Local only.
- **Flash readout protection disabled, no secure boot, and a debug
  interface left enabled** — physical access to the board is enough to
  dump the firmware (how the backdoor PIN above was found), reverse-engineer
  it, and potentially reflash it.
- **Modifications made this way survive a factory reset, invisibly** — no
  trace to the owner. This is a strictly worse variant of
  [[threat-model-bond-and-physical]]'s already-accepted "physical reset =
  full takeover" model for OUR side: there, at least a reset is a known,
  bounded event the server can flag as "reset & reclaimed." Here, a
  compromise can persist THROUGH a reset with no signal at all that
  anything happened.
- The author separately notes the **near-total absence of public
  documentation for the MCU added friction to the analysis** — the same
  opacity this assessment already ran into trying to find BF5885AM64
  current-consumption figures.

**What this does and doesn't tell us.** It's evidence that BF5885AM64 CAN
ship this insecurely, on some vendor's product — not proof that OUR
vendor's board has these exact flaws, and not a silicon defect: readback
protection, secure boot, and debug-port locking are standard MCU security
primitives that (if the silicon has them, which is unconfirmed for this
part) a competent firmware build would enable. If we take firmware control
ourselves (per the operator's 2026-08-28 direction, see the update note at
the top of this doc), we would not ship the vendor's backdoor PIN and
would design our own NFC/RFID challenge rather than a bare UID check. But
we cannot even confirm the silicon *has* the protections we'd need to
enable, because — same as the article's author found — there's no public
datasheet to check against.

## 3. Recommendations, ranked

1. **Get §2.1 answered in writing before anything else here matters much.**
   If CH584M's local BLE is a live, independent, Tuya-stock pairing stack,
   every other mitigation in this doc is secondary to closing or auditing
   that door.
2. **Ask for the full radio inventory of both CH584M and BF5885AM64**
   (§2.5) — explicit confirmation of no Wi-Fi/cellular/LoRa, and whether
   either supports any form of OTA (BLE or otherwise).
3. **Ask for firmware provenance**: reproducible builds, or at minimum a
   signed hash we can check against what a unit reports, so a swapped or
   backdoored unit post-manufacture is detectable.
4. **Treat DL-MCU DP reports as untrusted telemetry server-side, not
   authoritative fact** (§2.2) — add a consistency/anomaly check before an
   access-granted report becomes an unqualified audit-log entry (rate
   limits, expected-state cross-checks), extending the lesson the 0x08
   census bug already taught us for free.
5. **Add a UART rate/flood guard** in `tuyaWirePump()` (§2.3) — a frame or
   byte-count ceiling per second — so a misbehaving or malicious DL-MCU
   cannot watchdog-reboot the ESP32 and take down remote-unlock/audit
   relay at will.
6. **Carry §2.7 into the DP 13-15 unblock design**, whenever the supplier
   answers `ozkey-42`.

None of 1-3 are firmware's to close alone — they are vendor-relationship
and hardware-sourcing decisions, per [[ozlock2-is-not-firmware-doc]]'s
"operator's call" convention. 4-5 are firmware work I can pick up once
prioritized.

## 4. Chip selection: should BF5885AM64 be crossed out?

**Not a flat disqualification, but CH584M should be the lead candidate**,
and BF5885AM64 shouldn't advance past "on notice" without answers §2.6/§2.8
don't currently have. Nothing in §2.8 is a silicon defect — every issue
named (backdoor PIN, weak NFC check, no secure boot, open debug port) is a
firmware/configuration choice, and if we're writing our own firmware
anyway (per the operator's direction) we would not repeat any of them, in
principle. What tips the balance is everything ELSE stacked against
BF5885AM64 at the same time:

- **No public datasheet**, confirmed independently twice now — once by
  this assessment trying to find current-consumption figures, once by the
  article's own author trying to do a security review. We cannot even
  confirm the silicon HAS the protections (flash readout lock, secure
  boot, debug fuse) our own firmware would need to enable, without either
  a vendor NDA/datasheet or the same reverse-engineering effort the
  article's author did.
- **No RF at all** (confirmed earlier this session) — needs an external
  BLE module either way, one more component, one more integration
  boundary, one more thing to secure (the link between BF5885AM64 and
  whatever BLE chip sits next to it).
- **CH584M by contrast has real public SDK material** (WCH's own
  `openwch/ch583` GitHub — register headers, a BLE library, example
  projects) and a community Arduino core (`ElectronicCats/arduino-wch58x`)
  — a genuine on-ramp for exactly the "develop our own firmware" plan,
  plus **built-in BLE**, so no external radio module or that extra
  integration boundary.
- **No equivalent public backdoor precedent turned up for CH584M** in this
  session's research — absence of evidence, not evidence of absence, but
  it's the more favorable side of that asymmetry given everything above.

If the vendor can produce a real CH584M-equivalent datasheet for
BF5885AM64 and confirm (not just claim) it has readback protection, secure
boot, and a lockable debug port, it's back in play — those are the
specific, checkable things that would change this recommendation. Until
then, treat it as the fallback, not the lead.

Related: [[ch584m-network-coprocessor-architecture-locked]],
[[threat-model-bond-and-physical]], [[our-dp-map-is-invented]],
[[pin-writes-land-on-navigation-volume]], [[silent-failures-rule]].
