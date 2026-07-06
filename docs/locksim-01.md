# LockSim — Development Log 01

Hardware-accurate **Smart Door Lock Simulator** — a bench testbed for a 2-person
engineering team. Simulates a physical lock motherboard MCU talking to its Wi-Fi
module over an unencrypted 4-wire UART (3.3V TTL) bus using the **Tuya MCU Hex
Serial Protocol (0x55 0xAA)**. Presented inside an iPhone-styled simulator shell
with a dual-terminal diagnostic console.

## Stack
- Next.js 15 (App Router) · React 19 · TypeScript (strict) · Tailwind CSS v4
- No runtime deps beyond next/react. WebAudio for click/motor SFX (no assets).
- Credential DB persisted to `localStorage` (key `locksim.credentials.v1`).

## Run
```bash
npm install
npm run dev      # http://localhost:3000
npm run build    # type-checks + prod build (passing)
```

## Architecture — protocol logic isolated from the view
```
lib/
  tuya.ts         Pure protocol engine: frame build/parse, checksum8, DP codec,
                  temp-credential codec (DPID 21-24), annotateFrame(),
                  extractFrames() for Web Serial reassembly. No React.
  credentials.ts  localStorage slot table + checkWindow() temporal check.
  provisioning.ts BLE onboarding: topicMatches(), parseOnboardingPayload(),
                  broadcast/sample builders, NetworkProvisioning storage. No React.
  audio.ts        WebAudio synth: keyClick / accessGranted / accessDenied / motorWhirr.
hooks/
  useTuyaProtocol.ts  Simulated UART bus + mode router: rxLog/txLog, transmit(),
                      receiveBytes(), injectHex(), serverPush(), clearLogs().
                      Routes frames per HardwareMode (software loopback vs wire).
  useLockState.ts     MCU state machine: power (sleep/wake), lock state, PIN buffer,
                      heartbeat countdown, GPIO wake, credential validation,
                      clutch motor, incoming-frame dispatch, revokeCredential().
  useVirtualClock.ts  Virtual Master Clock (real time + warp offset). now() getter.
  useSerialLink.ts    Mode B transport: Web Serial API. connect()/disconnect()/
                      write(Uint8Array); read loop reassembles frames via
                      extractFrames(). status/portLabel/ready/supported.
  useProvisioning.ts  Registration state machine: beginRegistration (announce →
                      awaiting room), OZKEYSERV handshake capture, green-x3
                      confirm pulse, persisted NetworkProvisioning.
  useMqttLink.ts      Mode A network transport: MQTT-over-WebSocket (mqtt.js) to
                      the OZKEYSERV broker. connect(settings)/disconnect/publish;
                      subscribes command + pair-confirm topics; onMessage.
  useElementWidth.ts  ResizeObserver width measurement driving the responsive
                      layout (promotes conversation panel to a 3rd column ≥1280px).
components/
  PhoneShell, StatusLeds (power + server-link + Wi-Fi LEDs), LockDisplay, Keypad,
  PeripheralControls, KeySlider,
  SerialConsole (virtual clock + dual terminals + injector + server-push),
  ConversationPanel (live lock ⇄ server MQTT transcript + Register Doorlock +
  Server Settings — its own 3rd column ≥1280px, else folds under the console),
  SettingsDialog (broker host/ws-port/path/MAC modal + connect controls),
  DeviceRegistry (Sovereign Device Registry DB grid),
  HardwarePipelineToggle (compact single-row Mode A / Mode B segmented control).
app/
  page.tsx        Wires hooks. Ref-bridge breaks the transmit<->handleFrame cycle.
  layout.tsx, globals.css (keyframes: flash, alarm-blink, motor-spin).
```

## Protocol details (lib/tuya.ts)
Frame: `[55 AA][ver 00][cmd 1B][len 2B BE][payload N][checksum 1B]`
Checksum = sum of all preceding bytes % 256.

- `TuyaCommand`: HEARTBEAT 0x00, DP_REPORT 0x06.
- `DpType`: RAW/BOOL/VALUE/STRING/ENUM/BITMAP.
- `DpId`: UNLOCK_CHANNEL 1, RFID_CARD 2, FINGERPRINT 3, BATTERY_ALARM 5,
  ACCESS_RESULT 8, ADD_TEMP_PIN 21, DELETE_PIN 22, ADD_TEMP_RFID 23, DELETE_RFID 24.
- `AccessResult` (ENUM on DP 8): SUCCESS 0, DENIED 1, EXPIRED 2.
- Temp credential payload: `[Slot 2B][Credential var][Start unix 4B][End unix 4B]`.
  Delete payload: `[Slot 2B]`.
- `annotateFrame()` produces "Parsed Incoming Hex -> Action: Add Temporary PIN,
  Slot: 14, Value: 482915, Valid ... -> Expires ..." lines for the RX terminal.
- Sample frames exported for the console presets: SAMPLE_REMOTE_UNLOCK_FRAME,
  SAMPLE_ADD_TEMP_PIN_FRAME (slot 14, PIN 482915), SAMPLE_ADD_TEMP_RFID_FRAME.

## Behaviours implemented
- **iPhone shell**: notch, side buttons, home indicator, neutral aluminium frame.
- **LEDs**: power/status green(awake)/red(alarm|lowbatt)/off(sleep); Wi-Fi icon
  flashes during wake burst.
- **Display**: LOCKED / UNLOCKED / SLEEPING (7µA) / WAKING (45mA), system clock,
  heartbeat T-countdown, animated deadbolt, 6-dot PIN, event ticker.
- **Keypad** 3x4 with press animation + click sound. 6 digits then `#` submits,
  `*` clears.
- **Peripherals**: Tap RFID (Mifare), Scan Fingerprint (alternates pass/fail for
  deterministic bench runs), Low Battery toggle. Temp RFID cards appear as extra
  tap buttons.
- **Mechanical key slider**: forces clutch open, holds bolt against auto-relock.
- **Power management**: default DEEP SLEEP (7µA, radio off). Every physical input
  is a GPIO wake interrupt -> WAKING (45mA) -> back to sleep after 1s idle.
- **10-min heartbeat**: countdown from 600s; at 0 bursts WAKING for 200ms, fires
  a HEARTBEAT (MQTT ping) TX frame, resets.
- **Incoming remote unlock**: DP_REPORT / DP 1 BOOL value 1 -> UNLOCKED 5s +
  clutch motor animation -> auto-relock.
- **Time-restricted entry**: temp PIN/RFID checked against Virtual Master Clock.
  Outside window -> Expired state, red LED flash, TX ACCESS_RESULT=EXPIRED.
  Inside -> unlock + TX ACCESS_RESULT=SUCCESS.
- **Console**: Terminal A (RX) / Terminal B (TX) matrix-green hex, auto-scroll,
  annotations; hex injector with Execute + presets; Clear Logs; virtual clock
  warp (±1h/±1d, datetime picker, sync-real).
- **Sovereign Device Registry DB** (`DeviceRegistry.tsx`): compliance grid over
  the LocalStorage array. Columns: Slot ID | Credential Type | Raw String Value/
  Hash | Valid From | Valid To | System Registration Token | Action. Rows colour
  -code by live status vs Master Clock — ACTIVE = low-saturation green border,
  PENDING/EXPIRED = faded amber backdrop. Each row has "Revoke / Wipe Slot" which
  compiles + fires a DPID 22 (PIN) / 24 (RFID) delete frame on the TX bus and
  wipes the slot instantly. Registration token = `SRT-XXXX-XXXX` issued on
  provisioning (backfilled on load for pre-existing records).

- **Dual-mode Hardware Pipeline** (`HardwarePipelineToggle.tsx`, top of page):
  - **Mode A — Pure Software Emulation (no ESP32):** app simulates both the lock
    motherboard and the Wi-Fi chip. `transmit()` and inbound server/admin commands
    stay internal — `serverPush`/`injectHex` encode the frame and loop it into the
    virtual parser (`receiveBytes`), updating LocalStorage immediately.
  - **Mode B — Physical Wire Integration (ESP32-C6):** `useSerialLink` opens a
    Web Serial port (9600 8N1). Keypad/peripheral frames from `transmit()` are
    flushed out as a binary `Uint8Array` over USB-UART; inbound server commands
    are forwarded over the wire (not parsed locally) so the ESP32 does translation.
    Frames physically arriving from the ESP32 are reassembled (`extractFrames`)
    and fed to the virtual parser to drive the on-screen sim.
  - TX log annotates the route taken ("↳ FLUSHED TO USB-UART", "↳ internal
    software bus", "SERVER CMD FORWARDED TO ESP32", or wire-offline warnings).
  - Web Serial needs Chrome/Edge over localhost or HTTPS; the toggle shows an
    "unsupported" state otherwise. Switching back to Mode A closes the port.

- **Matter-style BLE provisioning** (`useProvisioning`, `BleProvisioning.tsx`):
  - "BLE Provisioning Mode" switch inside the lock face. On → device goes
    UNPROVISIONED (any prior pairing wiped), BLE LED flashes blue, main display
    reads "UNPROVISIONED".
  - "Broadcast Hardware MAC ID" button advertises MAC `AA:BB:CC:11:22:33` up to
    the OZKEYSERV/ broker (`buildBroadcastPayload`) — logged on TX, and in Mode B
    written out the USB-UART bridge as newline-terminated bytes via Web Serial.
  - Handshake capture: the inbound stream parser accepts JSON on the MQTT topic
    filter `hotel/rooms/+/lock/command`. A payload whose `mac` matches this device
    and that carries a `room_no` (+ `server_ip`) is validated by
    `parseOnboardingPayload`. On success it halts the blue flashing, shows
    "PAIRED - ROOM [X]", persists `{assigned_room_no, server_ip, mac_token, mac}`
    to LocalStorage (`locksim.provisioning.v1`), and flashes the LED green ×3.
  - Two entry points: the console's "OZKEYSERV/ Onboarding Handshake" JSON panel
    (Publish to Lock, plus valid/mismatch presets), and the main hex injector —
    any input starting with `{` is auto-routed to the provisioning parser instead
    of the Tuya hex decoder.

## Mode A networking — MQTT-over-WebSocket (2026-07-06)

LockSim (browser) now speaks **MQTT-over-WS directly** to the OZKEYSERV broker —
the simulated ESP32 radio. No HTTP `/sim/*` bridge needed. See `ozkey-02.md`
(handshake contract) in `~/Documents/Dev/ozkey/docs`.

- **Server Settings** dialog (gear in the conversation panel): broker host, WS
  port, WS path, device MAC → `ws://host:port/path`. Persisted to
  `locksim.broker.v1`. Lab default `ws://10.1.1.21:9001/mqtt` (Mosquitto
  `mosquitto.dev.conf` in nexus already has `listener 9001 / protocol websockets`).
- **Register Doorlock** button: publishes `buildBroadcastPayload(mac)` to
  `hotel/locks/unpaired/heartbeat` → lock enters UNPROVISIONED/awaiting-room.
- LockSim subscribes `hotel/rooms/+/lock/command` (+ `hotel/locks/+/pair/confirm`).
  Inbound routing (`handleInboundJson`): JSON with `payload_hex` → unwrap → Tuya
  parser (closes ozkey-02 gap #5); JSON with `mac`+`room_no` → `parseOnboardingPayload`
  (real MQTT topic injected when not embedded) → pair. Heartbeat tick publishes
  `hotel/rooms/<room>/lock/heartbeat` (closes gap #4).
- **OZKEYSERV/ Onboarding Handshake** panel is now a live **conversation
  transcript** (lock→server right, server→lock left) instead of a JSON editor.
- BLE Provisioning Mode toggle + Broadcast button were **removed** (ozkey-03 BLE
  Phase 0 is deferred; "assume network connected"). Provisioning *state* stays.

⚠ Cross-repo dependency: full auto-pairing still needs OZKEYSERV to emit the
ozkey-02 §3.2 handshake shape on `hotel/rooms/<room>/lock/command` (gap #2) and
DPID 21/23 DP_REPORT credential frames (gap #3). Until then, paste a §3.2 sample
into the console inject bar (JSON auto-routes to the provisioning parser).

## Credentials for testing
- Master PIN: `123456#`  ·  Master card UID: `7B 3F 91 D2`
- Inject preset "Add Temp PIN 482915 (Slot 14)" then enter `482915#`. Warp the
  clock past 2026-12-31 to see the Expired rejection path.

## Known follow-ups / not yet done
- No automated tests yet (protocol engine in lib/tuya.ts is pure and unit-testable).
- Fingerprint pass/fail is a deterministic alternation, not a stored enrolment.
- Incoming heartbeat auto-responds; no MQTT transport modelling beyond the log line.

## Git
Remote: https://github.com/ebizcoAU/locksim.git
