# ozkey-34 — Operational modes: `development` vs `production` lock identity

> **Status: ARCHITECTURE RECORDED 2026-08-16, IMPLEMENTATION IN PROGRESS.**
> Authoritative record of the operational-mode split (PM directive, 2026-08-16).
> Consumers: **firmware** (`doorlock`/`doorlock19`), **NEXUS / ozlockserv**,
> **BANOI app**. Supersedes nothing; extends `ozkey-22` (factory reset),
> `ozkey-26 §1.4` (`info.pub`), `ozkey-17` (OZKIE verbs), and corrects
> `ozlock2.md` C11/C15 (see §7).
>
> 🔴 **Read §6 before implementing F-10 anywhere.** Switching an already-paired
> lock's key source destroys every existing bond. That is a property of the
> crypto, not a bug, and it constrains the rollout order.

---

## 1. Why the split exists

The lock's identity is an X25519 keypair. Today it is **minted on first boot and
stored in NVS** (`ozcrypto.h:152`, `ozLockKeyInit()` — `esp_fill_random()` then
persisted as `xpriv`/`xpub` in the `blelock` namespace). That has one fatal
property for a shipping product:

**A factory reset wipes NVS, so the lock's identity changes.** Its `info.pub`
becomes a different key, and NEXUS's stored public key for that MAC goes stale.
A reset lock is, cryptographically, a different lock.

eFuse fixes exactly this: one-time-programmable silicon that no software path
can erase. But burning eFuses is irreversible, requires a factory jig, and is
hostile to bench work where boards get wiped many times a day.

So: **two modes, one firmware.**

| | `development` | `production` |
|---|---|---|
| Key source | NVS (`xpriv`/`xpub`) | eFuse `BLOCK_KEY0` |
| Provisioned by | NEXUS, delivered via app over BLE | factory jig, before shipping |
| Factory reset | key lost → re-provision | key persists → identity survives |
| Use case | bench, pilot batches, R&D | all shipping units |

## 2. Development mode flow

1. Lock boots in `development` (no valid eFuse key burned).
2. App discovers the lock over BLE and reads its MAC.
3. App sends the MAC to NEXUS with its own `app_id`.
4. NEXUS generates or retrieves an X25519 keypair for that MAC, stores it in
   `lock_registry`, and returns **both public and private key** to the app over
   TLS.
5. App writes the pair to the lock over BLE via `provision_key` (§4.1).
6. Lock stores it in NVS and uses it as its identity for all subsequent OZKIE
   handshakes.

🔴 **The private key is transmitted over BLE and stored in NVS in cleartext at
rest.** This is acceptable *only* because development units are not protecting
anything. It must never reach a shipping unit — see §5 for the enforcement.

## 3. Production mode flow

1. Lock boots in `production` (valid key present in eFuse `BLOCK_KEY0`).
2. Firmware reads the private scalar from eFuse into RAM, derives the public key
   with `ozX25519Base()`, and **zeroises the RAM buffer** once the keypair is
   established.
3. `info.pub` exposes the derived public key.
4. App reads MAC + `info.pub` over BLE and registers the lock with NEXUS
   (`POST /pairings` with `lock_pub`).
5. NEXUS stores the public key MAC-indexed in `lock_registry` and marks
   `commissioned_at`.
6. All ECDH handshakes use the eFuse-derived key.

Note the direction reversal, which is the whole point: in development NEXUS
*tells* the lock who it is; in production the lock *tells* NEXUS who it is. Only
the second is trustworthy.

## 4. Interaction with NEXUS and the app

### 4.1 `provision_key` — development only

```json
{"kind":"provision_key","pub":"<64 hex>","priv":"<64 hex>"}
```

Accepted **only** when `operational_mode == "development"`. In `production` it is
rejected outright — a production lock's identity is not writable over the air,
by anyone, ever.

🔴 **Sealing this verb is circular and the directive does not resolve it.** A
sealed OZKIE verb is sealed under a bond, and every bond secret is derived from
the lock's *current* private key (§6). So the key that authenticates the
delivery is the key being replaced. Three options, none free:

- **(a) Plaintext on the provisioning characteristic.** Anyone in BLE range
  during the window gets the private key. Dev-only, and the window is 60 s.
- **(b) Seal under the lock's self-minted first-boot key.** Works, because the
  lock always mints a keypair on first boot before anything else — so there IS
  an identity to bond to. But installing the new key **immediately invalidates
  the bond that just delivered it**, so the app must re-pair straight after.
- **(c) Ephemeral transport key** negotiated for this one exchange.

**(b) is recommended** — it reuses machinery that already exists and works, at
the cost of a documented "re-pair after provisioning" step. **Decision needed
from PM/app before F-9 can be called done.** Implementation currently follows
(b) with the re-pair requirement made explicit in the response.

### 4.2 What NEXUS must hold

`lock_registry`, MAC-indexed: `lock_pub`, `commissioned_at`, and in development
mode only, the generated `lock_priv`. **The private column must not exist for
production rows** — a directory that holds production private keys is precisely
the vendor-surveillance posture the Sovereign Edge design exists to refuse.

## 5. Security implications

**MAC sniffing alone is insufficient** — but 🔴 **not for the reason this
section originally gave.** As first written, §5 said: *"The app must additionally
present an `app_id` already paired to that `device_id` + MAC in NEXUS, so a
cloned app with a sniffed MAC is rejected at the directory."* That was taken
from the PM directive's rationale and **it is not true of the system as built.**
NexusPM checked and refused to document it (`nexus/nexus-02.md` N-9), on two
grounds:

1. `ozlock_locks` is **explicitly not an authorization boundary** — its own
   migration comment reads *"Display only; real authority lives in the lock's
   own bond list."* Nothing server-side stops a different authenticated account
   writing its own row for a `lock_id` it does not hold.
2. `GET /locks/:mac/pubkey` is **public, no auth, by its own spec**. A spoofed
   MAC gets the same real public key a legitimate request would.

**The actual defence is the ECDH handshake itself.** An attacker who sniffs a
MAC and fetches the public key still cannot derive the shared secret, because
that requires the private key — in eFuse on a production unit. This is exactly
what `ozlock_v3.6` C15 already claims, and it stands on its own; the
directory-side check was never the thing protecting anything.

Firmware reached the same conclusion independently before reading N-9 (see the
2026-08-16 reply to the PM's §5.2 questions), which is some comfort that this is
the right reading rather than two teams sharing a blind spot.

If a genuine server-side gate is wanted, `POST /pairings` is the plausible place
and needs its own spec — NexusPM has flagged the same.

**Lost phone.** Recovery is a support-verified identity check, after which NEXUS
issues a new `app_id` binding or triggers a secure reset flow. There is no
self-service path, by design.

**Development keys are compromised by assumption.** Transmitted over BLE, stored
in NVS cleartext, and held by NEXUS. Any unit that has ever been in development
mode must be treated as having a known private key, permanently — burning an
eFuse later does not retroactively protect anything that key signed or sealed.

**🔴 Production mode is NOT extraction-resistant.** Option (b) burns with
`--no-read-protect` so software can read the block. What eFuse buys is
**immutability, per-chip uniqueness, and survival across factory reset** — all
genuinely valuable. It does **not** prevent key extraction by anyone who can run
code on the chip or reach a debug interface. See §7.

## 6. 🔴 The bond-invalidation hazard — read before changing key source

```c
static bool ozBondSecret(int slot, uint8_t out[32]) {
  return ozX25519(g_lockPriv, g_bonds[slot].pub, out);   // ozcrypto.h:513
}
```

Every bond's shared secret is `X25519(lock_private, member_public)`. Change the
lock's private key and **every derived secret changes with it**.

The failure mode is nasty because it is silent and looks like corruption: the
bond *table* survives (member public keys are stored in NVS), so the lock still
reports `bonds=1` and still believes it is paired — but every sealed envelope
fails to open, every unlock is refused, and nothing in the logs says "your
identity changed."

**Consequences for rollout:**

- **Burn eFuse BEFORE first pairing, always.** A unit that is paired and then
  switched to production mode loses every pairing.
- A development→production migration on an already-commissioned lock requires a
  **deliberate re-pair**, and should force one (wipe the bond table when the key
  source changes) rather than leave a lock that looks paired and works for
  nothing.
- The same applies to `provision_key` in development mode (§4.1 option b).

**Corollary, and the answer to F-13:** flashing `doorlock-1.78` over a live lock
**does preserve bond #0**, because 1.78 changes no key material and does not
touch `xpriv`/`xpub`. This is not theory — the 1.76 → 1.77 flash on LockA
(2026-08-15) came back with `[BOND] table loaded (v2) — 1 bond(s)` and the same
pubkey `cd6cfe55a49cf250…`. The danger is F-10's key-source switch, not the
flash.

## 7. Correction to `ozlock2.md` C11 / C15

Rev 3.4 (§3.4.4) established that the specified hardware-ECDH mechanism does not
exist on the ESP32-C6: no `esp_crypto_ecdh_shared_secret`, no ECDH peripheral,
`ESP_EFUSE_KEY_PURPOSE_USER` documented as *"User purposes (software-only use)"*,
and no `ECDSA_KEY` purpose in the C6 enum at all.

Option (b) accepts that and burns with `--no-read-protect` so software *can*
read the block. Therefore:

- **C11** — "read/write protection" → **write-protected only**; readable by
  firmware, by design.
- **C15** — "cloned device lacks hardware key" → holds against an attacker with
  only a sniffed MAC and no physical access; does **not** hold against an
  attacker who can execute code on the chip or reach its debug interface.
- Any "unextractable in silicon" phrasing → **"immutable, unique per chip, and
  survives factory reset."**

## 8. Factory burn process

```sh
# espefuse on this toolchain is a BINARY, not espefuse.py:
~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/espefuse \
    --chip esp32c6 -p <PORT> burn_key BLOCK_KEY0 private_key.bin USER \
    --no-read-protect

# Verify the block landed and carries the right purpose:
~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/espefuse \
    --chip esp32c6 -p <PORT> summary
```

**The `summary` dump is not sufficient acceptance.** It proves bits landed, not
that the *right* key landed in a form the firmware can use. The production check
is: burn → boot → read `info.pub` over BLE → compare against the public key the
jig derived from the same private scalar. Match means the burn is correct **and**
the firmware can use it.

Exact flag spelling should be confirmed with `espefuse --help` on the jig host
before this goes into a factory work instruction — it has not been run against a
board here.

## 9. Factory reset behaviour (extends ozkey-22)

`factoryReset()` must:

- **(a)** wipe NVS — including `xpriv`/`xpub` and `operational_mode`;
- **(b)** leave eFuse **untouched**. It is one-time-programmable; there is no
  software path that could erase it even if we wanted one.

On the next boot the mode is **re-derived, not remembered**: if eFuse
`BLOCK_KEY0` holds a valid (non-zero) key, the lock comes up in `production`;
otherwise it defaults to `development`. This is deliberate — mode is a
*consequence* of the hardware's state, so a wiped lock cannot come up claiming a
security posture it no longer has.

**Note for support:** a production lock therefore **cannot be anonymised**. Its
identity survives every reset, forever. That is the intended guarantee, and it
also means a resold or returned unit carries its history in NEXUS unless the
directory row is explicitly retired.

## 10. Implementation status

| Item | State |
|---|---|
| F-8 `operational_mode` NVS key | implemented, `doorlock-1.79` |
| F-9 `provision_key` verb | implemented, development-only; §4.1 sealing decision OPEN |
| F-10 eFuse read + zeroise | implemented, **untested on real silicon — nothing burned** |
| F-11 `info.pub` returns active key | implemented |
| F-12 factory-reset behaviour | implemented |
| F-13 bond preservation on 1.78 | answered, §6 — evidence-backed |
| F-14 `ozlock2.md` C11/C15 | done, §7 + ozlock2.md Rev 3.5 |

**Nothing has been burned to any eFuse.** F-10's read path is compiled and
logically correct but has never seen a real burned block, so `production` mode
is unexercised end to end.

---

## 11. Server-side proposal (`ozlockserv`) — WITHDRAWN 2026-08-16

**Added by:** ozkey server · **Date:** 2026-08-16 · **Status:** ⚠️ Superseded
same day by `ozkey-35.md`. Kept below (struck through in spirit, not in text)
so the mistake and its correction are both on record — not deleted, per the
"specified is not built, and neither is silently un-writing a wrong claim"
discipline `ozkey-35.md` §8 asks for.

**What was wrong:** everything below assumed "NEXUS" was a spec-name for
`ozlockserv`. It is not. NEXUS is a separate, real, already-built codebase
(`~/Documents/Dev/nexus/`, owner NexusPM — see `nexus-02.md`/`nexus-03.md`,
and `ozkey-35.md` §2's glossary, written specifically because this mistake
was made independently by both firmware and server the same day). `ozlockserv`
is explicitly specified to **never** hold a public-key directory (ozkey-35 §2:
"that is the Sovereign Edge guarantee"). Proposing a `lock_registry` table and
a dev-key endpoint on `ozlockserv` (§11.1/§11.2 below, original text) would
have built a second, competing identity directory — exactly the outcome §11.3
warned about, just from the wrong direction.

**Where S-5/S-6 actually landed:** already done, correctly, in NEXUS —
`lock_registry` (nexus-02 §N-5, built), `GET /api/v1/locks/:mac/pubkey`
(§N-7, built + live-verified against LockA), factory bulk ingest (§N-8, built
+ live-verified). S-5's dev-mode keypair distribution is NexusPM's §N-6 —
proposed, not yet built, blocked on the same open question raised below
(§11.1 original) independently: no per-install `app_id`/dev-flag concept
exists yet to gate who can ask for one. That decision belongs to PM + NexusPM,
not server.

**`ozlockserv`'s actual role in this flow:** none, currently, for identity —
by design. If `POST /pairings` (the `ozlockserv` one) ever needs to record
that a lock has been commissioned in NEXUS, that would be a deliberate,
separate decision to make later, not a byproduct of S-5/S-6.

**S-8 reassessed:** the objection below (standing instruction to ignore
`ftposDecisions/nexus/*`) no longer holds — the operator directly instructed
reading `nexus-02.md`/`nexus-03.md` in this same session, 2026-08-16,
superseding the 2026-08-13 "ignore" instruction. NexusPM and their document
trail are real and current. Nothing further to coordinate on `lock_registry`
specifically — it's built and matches what S-6 asked for.

<details>
<summary>Original §11 text (superseded, kept for the record)</summary>

### 11.1 S-6 — production key registration [WRONG — see above]

Extend `POST /pairings` to accept an optional `lock_pub` (64 hex chars,
matching the `app_id` validation regex already used for auth), consistent
with §3 step 4's flow. New table rather than overloading `locks`, because
`commissioned_at` and future `lock_registry` fields (e.g. eventual key
rotation/retirement) don't belong on the heartbeat/presence row:

```sql
CREATE TABLE lock_registry (
  mac            VARCHAR(17) PRIMARY KEY,
  lock_pub       CHAR(64) NOT NULL,
  commissioned_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (mac) REFERENCES locks(mac)
) ENGINE=InnoDB
```

**Error codes (Q-S4):** `mac_unknown` (404), `lock_pub_malformed` (400),
`lock_pub_conflict` (409) — see the real answer already live in nexus-02 §N-8
instead (rejects on conflict for production rows, overwrites cleanly on a
genuine dev→production transition).

### 11.2 S-5 — development-mode keypair endpoint [WRONG — see above]

```
POST /dev/lock-keys
  Auth: support bearer token; 404s (not 403s) when DEV_MODE_ENABLED=false
  Body: {"mac": "...", "app_id": "..."}
  → {"pub": "...", "priv": "..."} over TLS
```

Superseded by NexusPM's §N-6 design note, which reached a near-identical
shape independently and is the correct owner for it.

### 11.3 Naming collision — still valid, unrelated to the above error

Two unrelated `lock_pub` mechanisms exist in team history. XF-48 §23–24
(2026-08-01) is a software TRNG-minted key for anti-squatting claim-binding,
agreed but never built. `ozkey-34.md` §1–10 is the eFuse hardware key, now
built server-side (in NEXUS). The PM's first message (Q-S1–S4) cited
"XF-48 Ask 1 / XF-50 §4.1" for the former; S-5/S-6 were clearly about the
latter. This collision is still unresolved and still worth the PM stating
explicitly whether XF-48's TRNG thread is superseded.

</details>

---

## 12. Claim-status corrections for `ozlock_v3.6` §7.2.1

**Firmware, 2026-08-16.** `ozlock_v3.6.pdf` is now the authoritative spec and has
absorbed the firmware material well — the power model (§3.1–3.2), fast poll on
BLE touch (§3.3.1), the unicast-downlink prerequisite (§3.3.2), the 300 s
heartbeat (§3.3.3), and the Option (b) / no-ECDH-peripheral reconciliation
(§4.1) are all correctly captured.

**The status register in §7.2.1 has run ahead of the code.** Six claims are
marked VERIFIED or MEASURED for things that have not happened. Recorded here for
whoever owns the spec to merge — firmware is not editing `ozlock2.md` or the PDF
(operator directive, 2026-08-16).

C14 already carries an honest `HARDWARE GAP`, which proves the register can hold
an uncomfortable truth. These six should get the same treatment.

| ID | Current | Should be | Why |
|---|---|---|---|
| **C7** | VERIFIED — "publishLog() disabled on residential tier via compile-time mode flags" | **NOT BUILT** | No such flag exists. The gate was scoped, the `mode` mapping question was raised (`ozkey-cloud`/`ozkey-local` is cloud-vs-on-prem, not residential-vs-commercial), and the eFuse directives overtook it before implementation. |
| **C8** | VERIFIED — "SED default 5s poll verified; firmware doorlock-1.78" | **BUILT, DEFAULT OFF, NEVER RUN** | Contradicted by §3.3.2 of the same document. `cfgThreadSed` defaults false pending bridge unicast. SED has never been enabled on any board. |
| **C9** | **MEASURED** — "SED 5s poll draws 45 µA average -> 6.3 years" | **ESTIMATED, NOT MEASURED** | 🔴 No power instrument exists on this bench. §3.1's "continuous hardware measurements … monitored at the 3.3V rail using high-resolution instrumentation" did not take place. The ~35 mA FTD figure is a datasheet estimate (correctly labelled theoretical in the footnote); the µA figures have no measurement behind them that firmware can identify. |
| **C11** | VERIFIED — "burn_key with read-permission enabled on hardware" | **SPECIFIED, NOT BURNED** | No eFuse has ever been written on this bench. |
| **C16** | VERIFIED — "Option (b): software-read enabled" | **SPECIFIED, NOT BURNED** | Same. `ozLockKeyFromEfuse()` compiles and has never seen a burned block; `production` mode is unexercised end to end. |
| **C17** | VERIFIED — "Procedural shred -u validated on the programming line" | **NOT VALIDATED** | There is no programming line yet. |

### 12.1 Two further inaccuracies outside the register

- **§3.3.2** — "the SED mode is disabled by default on the lock (falling back to
  a 60s Wi-Fi heartbeat)". The fallback is **rx-on FTD on Thread**, not Wi-Fi,
  and the Wi-Fi heartbeat default is now **300 s** (§3.3.3 has this right).
- **§4.1** — "disabled JTAG/UART interfaces … raising the cost of physical
  exploit above $100,000 AUD". **JTAG and UART are both live.** USB CDC serial
  is how every bench log in this project is read. Nothing in the firmware or the
  eFuse configuration disables either today. Combined with `--no-read-protect`,
  a physical attacker with a serial console is a great deal cheaper than
  $100,000 — see §5 and §7 of this document.

### 12.2 The footnote that needs the most attention

> *"All figures measured with active `[MON] radio=` instrumentation confirming
> actual mode on 2026-08-15."*

`[MON] radio=` was added in **`doorlock-1.78`**, which has never been flashed to
any board. It cannot have confirmed anything on 2026-08-15. The line was written
to establish provenance, and it is the line least able to support it.

### 12.3 What would make C9 real

The measurement is cheap once the instrument exists:

1. Source a USB inline power meter (coarse) or a Nordic PPK2 / Otii Arc (which
   resolves the poll duty cycle and is what this actually needs).
2. Measure on the **3.3 V rail after the regulator, with the LCD disabled** —
   on a dev kit the backlight and USB-serial bridge dominate and a
   connector-side reading mostly measures the dev kit.
3. Set `sed=1` / `poll=5` in NVS, confirm `[MON] radio=SED`, run ≥1 h.
4. Repeat with `sed=0` for the FTD comparison.

Firmware is ready for all of it; only the instrument is missing.

---

## 13. Server reply to §12 and status on S-5/S-6/S-8

**Replied by:** ozkey server · **Date:** 2026-08-16

| Action | Status |
|---|---|
| Spec corrections merged using firmware's §12 findings | ✅ Done — C7, C8, C9, C11, C16, C17 corrected in `docs/ProdDev/ozlock_v3.6.md` (the editable source; PDF not yet regenerated from it) |
| No further provisioning work — NEXUS owns the pubkey directory | ✅ Confirmed — see §11 above (server's original S-5/S-6 proposal withdrawn same day; `lock_registry` + `GET /api/v1/locks/:mac/pubkey` already built and live-verified in NEXUS, not `ozlockserv`) |

Two items from §12.1 remain **not yet corrected** in `ozlock_v3.6.md`: §3.3.2's SED-fallback
description (says Wi-Fi 60s; is actually FTD-on-Thread) and §4.1's JTAG/UART-disabled claim
(both interfaces are live). Flagged in the doc's correction note; not actioned pending direction.

`ozlock_v3.6.pdf` itself has not been regenerated — it still shows the pre-correction claims.
That's a format-tooling step (`.md` → PDF), not a content decision, and hasn't been asked for yet.
