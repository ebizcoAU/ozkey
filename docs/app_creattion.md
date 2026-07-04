Create a hardware-accurate Smart Door Lock Simulator Web Application using React (TypeScript), Next.js, and Tailwind CSS. The app must feature a high-fidelity visual layout mimicking a sleek smartphone/iPhone simulator interface. 

The primary purpose of this web app is to act as a laboratory testbed for a 2-person engineering team. It must simulate a physical door lock motherboard communicating over an unencrypted 4-wire serial bus (UART 3.3V TTL) using the standard Tuya MCU Hex Serial Communication Protocol (0x55 0xAA headers).

### 1. VISUAL LAYOUT REQUIREMENTS (iPhone Simulator Theme)
- Encase the app in an iPhone-styled chassis centered on the screen (rounded corners, speaker notch, sleek borders).
- Inside the simulator, design a modern, clean smart lock interface containing:
  - LED Indicators: One "Power/Status" light (Green/Red/Off), one "Wi-Fi Connected" icon (flashing during burst).
  - Main Display Area: A digital readout displaying lock status ("LOCKED", "UNLOCKED", "SLEEPING (7µA)", "WAKING (45mA)").
  - Numerical Keypad: Grid 3x4 (1-9, *, 0, #) with responsive press animations and tactile click feedback sounds.
  - Peripheral Control Buttons: Dedicated buttons labeled "Tap RFID Card (Mifare)", "Scan Fingerprint", and "Low Battery Event Trigger".
  - Mechanical Key Slider: A toggle to simulate a physical mechanical key override.

### 2. PROTOCOL ENGINE & HARDWARE LOGIC (Tuya 0x55 0xAA)
Implement a raw byte-array compilation and parsing engine that tracks the standard Tuya MCU Serial Framework:
Frame Structure: [Header 0x55 0xAA] [Version 0x00] [Command ID (1 Byte)] [Length (2 Bytes, Big-Endian)] [Payload (Variable)] [8-bit Checksum (1 Byte)]
Calculate the checksum dynamically by adding all bytes prior to the checksum index modulo 256.

Handle these specific translation functions:
- Keypad Entry Output: When a 6-digit pin is entered and '#' is pressed, output a hex frame with Command ID 0x06 (Data Point ID 1, Type Value, Payload containing the status).
- RFID Tap Output: When "Tap RFID" is clicked, output a hex frame with Command ID 0x06 (Payload representing a mock card ID).
- Fingerprint Scan Output: Output a hex frame indicating successful or failed biometric verification.
- Incoming Remote Unlock Command: If the simulator receives an incoming hex block containing Command ID 0x06 with an unlock payload value, transition the lock state to "UNLOCKED" for 5 seconds, fire a simulated clutch motor animation, and then auto-relock.

### 3. ASYNCHRONOUS BATTERY SAVING SIMULATION (10-Min Sleep Loop)
- Implement a visible system clock and a countdown timer representing the "10-Minute Heartbeat Loop".
- By default, the simulator must sit in "DEEP SLEEP" state (Displaying: 7µA draw, Radio: OFF).
- Wake Event 1 (Timer): When the 10-minute timer hits zero, burst the lock into "WAKING" state for 200ms, simulate sending an MQTT heartbeat packet, and fall back to sleep.
- Wake Event 2 (Physical Touch/Interrupt): The absolute millisecond any Keypad button, RFID tap, or Fingerprint scan is clicked, instantly yank an internal simulated GPIO line high, wake the system processor, change state to "WAKING (45mA)", burst transmit the corresponding Tuya Hex Packet, and return to sleep after 1 second of inactivity.

### 4. LIVE SERIAL / DIAGNOSTIC CONSOLE COMPONENT
Beneath or alongside the iPhone simulator housing, implement a dual-terminal split diagnostic logging window:
- Terminal Window A (Incoming Hex Stream Data): Displays a scrolling list of raw hexadecimal string blocks received by the lock (formatted as pairs, e.g., "55 AA 00 06 00 05 01 02 00 01 01 B0").
- Terminal Window B (Outgoing Hex Stream Data): Displays all outbound hexadecimal frames fired by the simulator when buttons are interacted with.
- Provide a manual input text bar labeled "Inject Incoming Hex Command" with an "Execute" button to let the developer manually paste and stream binary strings directly into the simulator parsing engine.
- Include a "Clear Logs" button for clean bench management.

### 5. ARCHITECTURAL CODE REPOSITORY SETUP
- Write clean, modular React hooks (`useLockState`, `useTuyaProtocol`) to isolate the protocol logic from the visual layer.
- Ensure all states use TypeScript interfaces mapping out exact hex array types (`uint8_t` equivalents in JavaScript arrays).
- Utilize Tailwind CSS for all UI, making the console look like an engineered hardware development testbed (dark mode matrix green text on black boxes for terminals, clean neutral grays for the iPhone shell).



### 6. LOCAL STORAGE / JSON DATABASE SIMULATOR (CREDENTIAL MANAGEMENT)
- Implement a lightweight, client-side mockup database (initialized via a local JSON structure and persisted in browser LocalStorage).
- This simulator database must hold a collection of authorized access credentials, structured as follows:
  - User Slots (e.g., Slot 1 to Slot 100).
  - Credentials types: Numeric PINs (stored as hashed strings or strings), RFID Card UIDs (e.g., "73 A2 F1 0C"), and Fingerprint Slot Identifiers (e.g., Fingerprint ID #12).
- When a user types a code on the phone keypad or clicks the "Fingerprint" or "RFID" button, the engine must perform a lookup against this local JSON data collection:
  - If a MATCH is found: Transmit the corresponding Tuya 'Access Granted' Hex string, toggle the LED to Green, and activate the 5-second clutch motor unlock logic.
  - If NO MATCH is found: Transmit a Tuya 'Access Denied' Error Hex packet, flash the LED Red, and trigger an auditory warning buzzer.
- Handle Incoming Configuration Commands: The parsing engine must read incoming Tuya commands for Admin Actions:
  - Command ID 0x09: "Add New User/Pin" (Appends a payload parameter string straight into the JSON database array).
  - Command ID 0x0A: "Delete User/Pin" (Purges the targeted index row from the LocalStorage storage loop instantly).
- Add a clean "Database Debug Viewer" slide-out tray on the side of the UI so the developer can see the live authorized user slots and credentials currently active inside the simulator's memory.




### 7. ADVANCED TUYA DATA POINT (DPID) & TIME-RESTRICTED ENTRY LOGIC
- Expand the `useTuyaProtocol` engine to explicitly support multi-parameter Tuya Data Points (DPIDs) for credential syncing, tracking the following command matrix:
  - DPID 21 (Add Temp PIN) & DPID 23 (Add Temp RFID): Payload contains a raw string formatting [Slot ID (2B)][Credential Value (Var)][Start Unix Timestamp (4B)][End Unix Timestamp (4B)].
  - DPID 22 (Delete PIN) & DPID 24 (Delete RFID): Payload contains [Slot ID (2B)] to wipe records.
- Implement an editable 'Virtual Master Clock' at the top of the diagnostic console that allows the developer to warp the simulator's current date/time forward or backward.
- When an entry is attempted via a temporary PIN or RFID button, execute a strict temporal check against the LocalStorage database:
  - If the Virtual Master Clock falls outside the credential's Start/End parameters, reject the entry, trigger an 'Expired Credential' error state, flash the LED Red, and fire an Outbound Tuya Status Hex Packet indicating an expired access failure.
  - If inside the window, proceed with standard unlock logic and fire a success packet.
- Ensure all incoming commands automatically print beautifully broken-down annotations inside the scrolling logs (e.g., displaying 'Parsed Incoming Hex -> Action: Add Temporary PIN, Slot: 14, Value: 123456, Expires: 2026-12-31').
