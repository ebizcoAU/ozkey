# LockSim

A hardware-accurate **smart door lock simulator** and laboratory bench testbed.
It emulates a physical lock motherboard MCU talking to its Wi-Fi module over an
unencrypted 4-wire UART bus (3.3V TTL) using the **Tuya MCU Hex Serial Protocol**
(`0x55 0xAA` framing), presented inside an iPhone-styled simulator with a live
dual-terminal serial diagnostic console.

Built with Next.js 15 (App Router), React 19, TypeScript (strict) and Tailwind CSS v4.

## Requirements

- Node.js 18.18+ (developed on Node 22)
- A Chromium browser (Chrome/Edge) is required only for the optional physical
  serial mode; all other features work in any modern browser.

## Getting started

```bash
npm install
npm run dev      # starts the dev server on http://localhost:3100
```

Then open http://localhost:3100.

### Scripts

| Command         | What it does                                        |
| --------------- | --------------------------------------------------- |
| `npm run dev`   | Start the dev server on port 3100                   |
| `npm run build` | Production build (also runs the TypeScript checks)  |
| `npm run start` | Serve the production build on port 3100             |

## Using the simulator

### Credentials for testing
- **Master PIN:** `123456` — type it on the keypad and press `#` to unlock.
- **Master RFID card UID:** `7B 3F 91 D2` — via the "Tap RFID Card" button.
- **Fingerprint:** the sensor alternates pass/fail on each scan for deterministic
  bench runs.

### Core interactions
- **Keypad** — enter 6 digits then `#` to submit (`*` clears). A valid PIN drives
  the clutch motor, unlocks for 5 seconds, then auto-relocks.
- **Peripherals** — Tap RFID, Scan Fingerprint, and a Low Battery event trigger.
- **Mechanical key slider** — physically forces the clutch open, overriding auth.
- **Power management** — the lock sits in deep sleep (7µA, radio off); every
  physical input is a GPIO wake interrupt (45mA). A 10-minute heartbeat timer
  bursts awake to send an MQTT ping and falls back to sleep.

### Diagnostic console
- **Terminal A (RX)** and **Terminal B (TX)** stream raw hex frames with
  human-readable annotations.
- **Inject Incoming Hex Command** — paste a raw Tuya frame (e.g.
  `55 AA 00 06 00 05 01 01 00 01 01 0E`) and Execute it into the parser. A JSON
  payload starting with `{` is routed to the provisioning handshake parser instead.
- **Server / Cloud Admin Push** — one-click Remote Unlock / Add Temp PIN / Add
  Temp RFID commands.
- **Virtual Master Clock** — warp the simulator's date/time forward or backward
  to test time-restricted credentials.

### Time-restricted credentials
Inject the "Add Temp PIN 482915 (Slot 14)" preset, then enter `482915#` to unlock.
Warp the Virtual Master Clock past the credential's expiry window to see the
`Expired` rejection path and the outbound access-failure frame. Stored credentials
appear in the **Sovereign Device Registry DB**, colour-coded by live temporal
status, each with a "Revoke / Wipe Slot" button that fires a DPID 22/24 delete
frame.

### BLE provisioning (Matter-style onboarding)
1. Toggle **BLE Provisioning Mode** on inside the lock face — the device goes
   `UNPROVISIONED`, the BLE LED flashes blue.
2. Click **Broadcast Hardware MAC ID** to advertise `AA:BB:CC:11:22:33` to the
   `OZKEYSERV/` broker.
3. In the **OZKEYSERV/ Onboarding Handshake** panel, click **Publish to Lock**
   with the valid Room-412 preset. A matching handshake on the
   `hotel/rooms/+/lock/command` topic pairs the lock: the display shows
   `PAIRED - ROOM 412`, the LED flashes green three times, and the network
   variables (`assigned_room_no`, `server_ip`, `mac_token`) are saved to
   LocalStorage.

### Hardware pipeline (software vs physical wire)
The **Hardware Pipeline** toggle at the top switches the backend:
- **Mode A — Pure Software Emulation:** the app simulates both the lock
  motherboard and the Wi-Fi chip; frames loop into the internal parser.
- **Mode B — Physical Wire Integration:** binds a real 3.3V USB-UART COM port via
  the browser Web Serial API and streams frames out to a desk-side ESP32-C6. This
  requires Chrome/Edge over `localhost` or HTTPS and physical hardware attached.

## Persistence

Credentials and the provisioning record are stored in the browser's LocalStorage
(`locksim.credentials.v1`, `locksim.provisioning.v1`). Clear site data to reset.

## Project layout

```
app/          Next.js App Router entry (page.tsx wires the hooks together)
components/    UI — phone shell, keypad, display, console, registry, toggles
hooks/         useTuyaProtocol, useLockState, useVirtualClock, useSerialLink,
              useProvisioning, useElementWidth
lib/          Pure logic — tuya.ts (protocol engine), credentials.ts,
              provisioning.ts, audio.ts
docs/         Development log (docs/locksim-01.md)
```

See [docs/locksim-01.md](docs/locksim-01.md) for the full architecture and
protocol reference.
