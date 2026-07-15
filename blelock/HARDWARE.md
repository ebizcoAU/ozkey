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

## Touch — I2C @ 0x3B (AXS5106L-class), needs wake sequence

| Signal | GPIO |
|---|---|
| SDA | 4 |
| SCL | 5 |
| INT | 3 (INPUT_PULLUP) |

Wake sequence (required before the chip ACKs — from Waveshare factory init):

```cpp
Wire.beginTransmission(0x3B); Wire.write(0xAA); Wire.write(0xAA); Wire.endTransmission(); delay(20);
Wire.beginTransmission(0x3B); Wire.write(0x80); Wire.write(0x01); Wire.endTransmission(); delay(10);
```

Read: write reg `0x00`, request **12 bytes**:
- `buf[0]` = touch count (valid 1..4)
- X raw = `((buf[1] & 0x0F) << 8) | buf[2]`
- Y raw = `((buf[3] & 0x0F) << 8) | buf[4]`
- Landscape transform used in the diagnostic: `touchX = 320 - rawY; touchY = rawX;`

## Peripheral bring-up checklist (pre-handover)

- [x] Display init + text render
- [x] Touch wake + coordinate read (verify transform matches chosen rotation)
- [ ] Wi-Fi STA join (2.4GHz) — connect to the lab AP
- [ ] **BLE advertise WHILE Wi-Fi associates** — C6 has ONE radio (time-sliced
      coex). This is the §7.5 closed-loop requirement (status notify over BLE
      during WIFI_JOINING) and the #1 real-silicon risk — test early.
- [ ] MQTT TCP connect to lab Mosquitto `10.1.1.21:1883` (hardware path is TCP,
      not the browser's :9001 websocket)
- [ ] NVS write/read (persists room/site/device_id across reboot)

## Firmware target (XFtposDecisions-43 §7.5 / ozkey-07)

Mirrors LockSim's MQTT wire exactly (announce → provision_assign → heartbeat →
DPID command frames → log) so ozkeyserv :3200 needs zero changes. Milestones
F1–F6 in the FTPOS decision log; BLE GATT contract in CONTRACT.md.
