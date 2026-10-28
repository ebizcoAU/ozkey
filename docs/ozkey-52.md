# ozkey-52 — Device identity and certification: how Nexus admits only genuine OZKIE devices

> Status: **DRAFT, 2026-10-18, firmware (team lead).** Doc order agreed by the operator: 52 → 51 → 50 → 53.
> Business decision (open spec + certification gate) is with Finance and the PM in **XF-388**. This
> doc is the engineering design under either outcome: the gate is needed even if the spec stays closed.
> Builds on ozkey-48 (OZKIE v2) and ozkey-49 (AK1037 port).
> **PM reply (nexus-28, XF-388, 2026-10-18): accept, with a real server-side certificate gate.**
> Publish only after all of these:
> - AK1037 hardware validation;
> - §6 key-storage answers from Anyka;
> - the OZKIE trademark filing;
> - the AU pre-trial.
> Publish from a **separate public repo** built from a clean export plus a secret scan, never a mirror.
> 🟢 **Operator RULED XF-388 (1a 2a, ftpos d5bac155): publish OZKIE openly (MIT) with the
> certificate gate.** Publish ONLY after all of these:
> - AK1037 hardware validation;
> - Anyka's key-storage answer **showing the gate holds** (§6);
> - trademark filings: AU now via Finance, **CN before the spec goes public**;
> - the AU pre-trial.
> Separate public repo, clean export + secret scan. The PM confirms all gates before anything is published.

## 0. Goals

1. **Nexus admits only genuine devices.** A lock, sensor or bridge registers with Nexus only if it
   proves it came from an authorised factory line. Operator, 2026-10-18: "Nexus identify them, and
   exclude other AK1037 chipset doorlock from entering Nexus."
2. **A genuine device cannot be cloned.** Copying the public data off one lock must not let a second
   device pass as it.
3. **Self-hosting stays free.** A hotel or landlord running its own server may accept any OZKIE
   device. The gate is at Nexus, not inside the protocol (Sovereign Edge "no hold-up").
4. **No new crypto on the lock.** The lock stays X25519 + AES-128 + SHA-256 only (ozkey-48 §1).

## 1. What the AK1037 gives us (SDK V1.02 docs, checked 2026-10-18)

| Feature | Available? | Use |
|---|---|---|
| TRNG | ✅ `hal_trng` | generate the device key on the chip |
| Chip ID | ✅ `fsa_get_globalid()`, **32-bit** | an index only: too short to be an identity on its own |
| Serial number | ✅ `fsa_get_sn()`, 32 chars, burned by the factory tool; *"the customer is responsible for uniqueness"* | bound into the certificate |
| BLE MAC | ✅ `fsa_get_bt_addr()`, burned by the factory tool | bound into the certificate |
| Secure boot | ✅ RSA-2048/4096 or SM2 signature, with our key hash in eFuse | only our signed firmware runs, so only our code can read the device key |
| Firmware encryption | ✅ SM4, under a "customer root key" derived on chip from our 4-byte custID | **the same key on every one of our chips**, so not a per-device secret |
| **Per-device secret key slot** | ❌ **not documented** | the ESP32 had eFuse BLOCK_KEY0 (ozlodge v2.4 §3.1.2). It doesn't carry over |
| **Debug port (SWD/JTAG) lockout** | ❓ not documented | **critical**: without it, a flash dump can extract the key |

**Consequence:** the device key has to live in flash. How hard it is to steal depends on two things
we must get from Anyka (§6): whether the debug port can be fused off, and whether flash data can be
encrypted with a key unique to each chip.

## 2. Design: one key, one certificate per device

```
            eBizco OZKIE Root CA (offline, never on a network)
                 │ signs
            Factory issuing CA  (one per factory line, hardware token, issuance cap)
                 │ signs
            Device certificate  (one per device, ~230 bytes, stored on the device)
                 │ binds
            Device X25519 public key  ⇐ private key generated on the chip, never leaves it
```

- **The device key is the lock's existing OZKIE identity key**: `info.pub` and the EDHOC static key
  (ozkey-48 §3). No second key.
- **The device certificate** is a CWT (RFC 8392) signed with COSE_Sign1 / Ed25519 by the factory
  issuing CA. Claims:
  - `cnf`: the X25519 public key;
  - `sub`: device_id;
  - model and hardware revision;
  - serial number, chip ID, BLE MAC;
  - issue time, factory/batch ID.
- **The lock never signs anything.** EDHOC method 3 already proves the lock holds the private key
  that matches `cnf` (static Diffie-Hellman). The certificate only says that this key belongs to a
  genuine device. Only the **verifiers** (phone app, Nexus) need Ed25519, and only to verify, which
  is standard on both.

### 2.1 Where the certificate is checked

| Verifier | When | Trusts |
|---|---|---|
| **Nexus** | device registration, and every pairing it brokers | only the eBizco root. **This is the gate** |
| **BANOI/AURA/MAVEN app** | first pairing (with the keypad code, ozkey-48 §3.4) | eBizco root built in, so the app can warn "not a certified device" |
| **Self-hosted ozlock/ozlodge/ozpms** | registration | configurable: eBizco root, the operator's own root, or "any device" |

**The certificate and the keypad code do different jobs.** The certificate proves the device is
genuine. The keypad code proves it is the device at your door. Both stay.

### 2.2 EDHOC carriage

- `info` gains key 6: the device certificate (CWT bytes).
- In EDHOC, ID_CRED_R carries the certificate by value (`kcwt`) on first contact. After that, a
  short `kid` is used once the app has cached it.
- message_2 grows by about 230 bytes the first time, so it takes 2 frames at MTU 247 (ozkey-48 §2.1).

## 3. Factory flow (at the Chinese manufacturer's line)

1. The burn tool writes the firmware, serial number and BLE MAC (Anyka tool; the serial must be unique).
2. **Provisioning firmware** on the device:
   - generates the X25519 key with the TRNG;
   - stores the private key in the protected region (§4);
   - outputs `{pubkey, chip ID, serial, MAC}` over the test UART.
3. The **factory station** sends that record to the factory issuing CA and gets back the signed
   certificate, which it writes to the device.
4. The station locks the device:
   - burns the secure-boot fuse;
   - **disables the debug port** (pending Anyka, §6);
   - loads production firmware;
   - powers up and reads the certificate back from `info`.
5. The station reports `{certificate}` to Nexus, which records it as manufactured and not yet sold.

**The factory CA is not on an open network:**
- It holds its key on a hardware token with an **issuance cap per production order**. A
  1,000-unit order gets 1,000 certificates.
- Nexus compares certificates issued against units ordered. Any excess points to a "ghost shift":
  extra units made on our line and sold outside our books.
- That is the main counterfeit risk with a contract manufacturer. Certificates make it countable.

## 4. Protecting the device key on AK1037

| Layer | Status | Blocks |
|---|---|---|
| Secure boot (our signature only) | ✅ available | firmware swapped to read the key out |
| Debug port fused off | ❓ ask Anyka | the probe-and-dump attack. **Without it, the rest is weak** |
| Key region encrypted with a per-chip key | ❓ ask Anyka | desoldered flash read in a programmer |
| Fallback: encrypt with the SM4 customer root key | ✅ possible | casual dumps only. The key is shared across all our chips, so one leak breaks all |

**Honest position until Anyka answers:** an attacker with the lock open on a bench, a debug probe and
skill may extract that one lock's key. That clones one lock, not the fleet. The certificate can then
be revoked (§5), and the count check (§3) still catches mass counterfeiting. Don't claim
"unextractable in silicon" for the AK1037. That was an ESP32 claim, and even there it was never
burned on a bench board (ozlock v4.0 §4.1).

## 5. Revocation

- **Nexus publishes a revocation list:** single devices (a key stolen or leaked) or whole batches
  (a factory CA compromised).
- Apps fetch it with their normal Nexus sync. Self-hosted servers can subscribe or ignore it.
- The root CA can issue a new factory CA without touching devices already in the field.

## 6. Questions for Anyka (add to the next email)

1. Is there an **eFuse/OTP region for a per-device secret**, readable only by the crypto engine or
   by secure-boot firmware?
2. Can the **SWD/JTAG debug port be permanently disabled** by eFuse? Does secure-boot mode do it
   automatically?
3. Can **data in flash** (not only the firmware image) be encrypted with a **per-chip** key?
4. Is `fsa_get_globalid()` unique across all AK1037 chips ever made, or only within a wafer/lot?
   32 bits is short.
5. **SKE "secure port" keys** (found 2026-10-18 in P3): `hal_ske_init()` takes a key from a "secure
   port" with `sp_key_idx` 0–3 when the key pointer is NULL. The driver marks that path `//TODO`.
   - Where do the 4 secure-port keys come from (eFuse? per chip?), and can firmware or a debug
     port read them?
   - Can we program a **per-device** key there and use it only through the engine?
   If yes, this is the per-device secret slot that §1 says is missing. The device key could then
   be stored in flash encrypted under a key that never leaves the silicon.

## 7. Cost

- **Lock:** about 230 bytes of flash for the certificate. One extra frame on first pairing. No new
  crypto code.
- **App and Nexus:** Ed25519 verify (standard libraries) and a CWT parser.
- **Factory:** a provisioning station plus a hardware token per line.
  - Firmware will write the station software.
  - The manufacturer runs it.
  - eBizco holds the root CA offline.

## 8. Open for the operator

1. **Who holds the root CA key**, and where (a safe, a hardware security module)? Firmware
   recommends two offline copies under two-person control.
2. **Bridges:** certify bridges too? Recommended: yes, the same scheme, so Nexus admits only
   genuine bridges as well.
