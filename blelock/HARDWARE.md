# blelock — ESP32-C6 doorlock emulator hardware notes

Board: **Waveshare ESP32-C6 Touch LCD 1.47"** (N8 = 8MB flash), operator-verified
pin map from the Arduino IDE diagnostic sketch (2026-07-14). Toolchain decision:
**Arduino core 3.x (arduino-cli / Arduino IDE)** — same core, shared
`~/Library/Arduino15`.

## Display — ST7789, 172×320, SPI

| Signal | GPIO |
|---|---|
| LCD_DC  | 15 |
| LCD_CS  | 14 |
| LCD_SCK | 1  |
| LCD_DIN (MOSI) | 2 |
| LCD_RST | 22 |
| LCD_BL (backlight, HIGH=on) | 23 |

Arduino_GFX_Library init (verified working):

```cpp
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 172, 320, 34, 0, 34, 0);
// col offset 34 both ends; operator sketch used setRotation(5) for landscape
```

## Touch — CST816-class I2C @ **0x63**, hardware reset pin (verified 2026-07-16)

⚠ The section that used to be here (0x3B + AA/80 wake sequence on SDA 4/SCL 5)
was WRONG for this batch — blelock.ino shipped with it and touch was dead on
real hardware. The values below are the ones Touch.ino / TicTacToe.ino verified
on the actual board.

| Signal | GPIO |
|---|---|
| SDA | 18 |
| SCL | 19 |
| RST | 20 (hardware power reset) |
| INT | 21 (INPUT_PULLUP) |

Init — hardware reset, no I2C wake sequence:

```cpp
pinMode(TOUCH_INT, INPUT_PULLUP);
pinMode(TOUCH_RST, OUTPUT);
digitalWrite(TOUCH_RST, LOW);  delay(100);
digitalWrite(TOUCH_RST, HIGH); delay(200);
Wire.begin(18, 19); delay(50);
```

Read: write reg `0x00`, request **7 bytes** (CST816 register map):
- `buf[2]` = active touch count (valid 1..5)
- X raw = `((buf[3] & 0x0F) << 8) | buf[4]`
- Y raw = `((buf[5] & 0x0F) << 8) | buf[6]`
- Landscape transform (rotation 5): `touchX = 320 - rawY; touchY = rawX;`

## Peripheral bring-up checklist (pre-handover)

- [x] Display init + text render
- [x] Touch wake + coordinate read (verify transform matches chosen rotation)
- [ ] Wi-Fi STA join (2.4GHz) — connect to the lab AP
- [ ] **BLE advertise WHILE Wi-Fi associates** — C6 has ONE radio (time-sliced
      coex). This is the §7.5 closed-loop requirement (status notify over BLE
      during WIFI_JOINING) and the #1 real-silicon risk — test early.
- [ ] MQTT TCP connect to lab Mosquitto `10.1.1.20:1883` (hardware path is TCP,
      not the browser's :9001 websocket)
- [ ] NVS write/read (persists room/site/device_id across reboot)

## Firmware target (XFtposDecisions-43 §7.5 / ozkey-07)

Mirrors LockSim's MQTT wire exactly (announce → provision_assign → heartbeat →
DPID command frames → log) so ozkeyserv :3200 needs zero changes. Milestones
F1–F6 in the FTPOS decision log; BLE GATT contract in CONTRACT.md.

---

## Production hardening — decisions that must be made BEFORE the first batch

Recorded 2026-08-04. None of this is needed for the pilot; all of it is a
**manufacturing-flow** decision, which means it cannot be retrofitted to units
already built. Same agenda as the RCM work.

### Current state (verified 2026-08-04)

| | Status |
|---|---|
| Flash encryption | **off** |
| Secure boot | **off** |
| eFuse use | **none beyond the factory MAC** |
| X25519 private key | **plaintext in NVS** (`xpriv`) |
| Device identity | `device_id = "ozk-" + eFuse MAC` |

### 1. A burned unique ID adds nothing — we already have one

`device_id` is derived from the factory-burned eFuse MAC. **A MAC identifies, it
does not authenticate**: it is readable by anyone and spoofable in firmware. No
burned *identifier* solves a security problem. Only a burned **secret** or a
**signature** does. Do not spend a manufacturing step on an ID.

### 2. Key at rest — the flash-dump attack

The X25519 private key sits in plaintext NVS. Opening the case and clipping onto
the flash yields `xpriv`, which clones the lock's identity, derives the pairing
secret and forges sealed frames. It is **quieter than the crowbar attack the
threat model already accepts** — no damage, no trace.

Scoped honestly: each lock has its own key, so this compromises one door, and the
attacker already has physical access. It matters where a lock is *briefly
handled* and attacked later — installer, cleaner, previous tenant, or
interception between factory and site.

**Fix: flash encryption** (eFuse-held AES key, hardware XTS on flash reads), not
an ID. Optionally the HMAC/DS peripheral, which can use an eFuse key that
hardware reads and software never can.

### 3. Trust anchor — there isn't one today

The app trusts whatever `info.pub` a device advertises. **Nothing distinguishes a
genuine OZLOCK from a counterfeit or a man-in-the-middle.** Already on the record
as CONTRACT.md "Deferred (v2) — factory-pubkey trust anchor".

**Fix: a factory signature over the device's public key.** Our CA attests "this
pubkey belongs to a genuine unit, serial N"; the app verifies before bonding.
This also gives anti-counterfeit, which starts to matter once suppliers build to
our spec.

### 4. ⚠ THE TRAP — the factory must never generate the keypair

This is the default way factory provisioning is done: generate, burn, log to a
database. **It would mean the contract manufacturer holds a copy of every
customer's private key.** For a product whose entire pitch is "we cannot open
your door, even if compelled", a supplier holding the keys is fatal — and it is
the first thing an enterprise security review will look for.

**Required pattern: the key is generated ON DEVICE and never leaves. The factory
station signs only the PUBLIC half** (a CSR-style flow). Trust anchor and
anti-counterfeit, with nobody ever holding the private key.

### 5. Costs, stated plainly

- Flash encryption and secure boot are **irreversible per device**, complicate
  OTA, and make field debugging much harder.
- The signing station needs a CA key, which then becomes the thing that must be
  protected. This **moves** the crown jewels; it does not eliminate them.

### 6. RCM / radio compliance — one firmware rule

The production board uses the **ESP32-C6-MINI-1-N8** with antenna tuning "100%
unchanged" (`docs/DoorLockHW/ESP32C6SEEEDSTUDIO.md:104`), so the DoC can lean on
Espressif's existing radio test reports. **That inheritance holds only while the
radio runs inside the tested envelope.**

**RULE: no `setTxPower` / `esp_wifi_set_max_tx_power` / radio-power call in
shipped firmware without a compliance conversation first.** Verified clean in
`doorlock.ino` and `bridge32.ino` on 2026-08-04 — both run the SDK default. This
is the kind of line that gets added innocently to fix a range complaint, and it
silently invalidates the evidence the Declaration of Conformity rests on.

Note also: RCM is **not** a certificate a module carries. It is a mark the
Australian supplier applies under their own DoC, backed by test evidence and
registered with ACMA. No module "has" RCM — not the Seeed dev board, not the
MINI-1.
