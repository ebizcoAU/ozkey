You are a technical documentation expert. Regenerate the SIMLOCK_GEEK_DLMCU_Technical_Specification.docx with the following complete specifications.

## HARDWARE CHANGE: REPLACE GEEK WITH N8 DEV BOARD

The test hardware is now the **ESP32-C6-DevKitC-1-N8** (or any ESP32-C6 board with full GPIO breakout), NOT the Waveshare GEEK.

### Why N8 Dev Board

| Aspect | GEEK | N8 Dev Board | Decision |
|--------|------|--------------|----------|
| **IO pins available** | Limited (GPIO3, 4, 5, 6, 9 only) | Full GPIO breakout (all pins) | ✅ N8 is better |
| **Camera MCU connector** | Not possible (insufficient pins) | Yes (SPI + GPIOs available) | ✅ N8 is better |
| **DL MCU connector** | Possible (5-wire uses GPIO3-6) | Yes (5-wire + spare pins) | ✅ Both work |
| **Form factor** | USB stick | Standard dev board | ✅ N8 for testing |

### Required GPIOs (N8 Dev Board)

| Signal | N8 GPIO | Connector |
|--------|---------|-----------|
| TXD | GPIO5 | DL MCU (5-wire) |
| RXD | GPIO6 | DL MCU (5-wire) |
| SRDY | GPIO3 | DL MCU (5-wire) |
| MRDY | GPIO4 | DL MCU (5-wire) |
| GND | GND | DL MCU (5-wire) |
| SPI SCK | GPIO19 | Camera MCU |
| SPI MOSI | GPIO18 | Camera MCU |
| SPI MISO | GPIO20 | Camera MCU |
| SPI CS | GPIO21 | Camera MCU |
| 3.3V | 3.3V | Camera MCU |
| GND | GND | Camera MCU |

### N8 Dev Board Specifications

| Feature | Specification |
|---------|---------------|
| **Module** | ESP32-C6-WROOM-1-N8 (8MB flash) |
| **USB** | USB-C for power, programming, and serial debug |
| **IO breakout** | All GPIOs available on header pins |
| **Antenna** | On-board PCB antenna |
| **Wi-Fi Provisioning** | SoftAP + Captive Portal (no app required) |
| **Control Interface** | Built-in web server + MQTT reporting |
| **Firmware Updates** | OTA from remote server |

## DOCUMENT STRUCTURE

Maintain the existing structure:
1. Introduction (Purpose, Scope, Definitions)
2. System Architecture (UPDATED with MQTT)
3. Hardware Interface Specification (UPDATED for N8)
4. Communication Protocol
5. Device Provisioning (UPDATED for N8)
6. Functional Specification (10 sections)
7. REST API Reference
8. MQTT Reporting Specification (NEW)
9. Requirements Traceability Matrix
10. Appendices

## SECTION 2: SYSTEM ARCHITECTURE (UPDATED)

### 2.1 Overview (UPDATED)

The SIMLOCK test system now uses the **ESP32-C6-DevKitC-1-N8** as the test controller. The system consists of four logical tiers:

1. **N8 Dev Board (Test Controller)**: Runs the test firmware, hosts the web server, and publishes test results via MQTT.
2. **MQTT Broker (Your Server)**: Receives live test data from all N8 boards.
3. **Backend Server + Web Application (Your Server)**: Subscribes to MQTT topics, stores test history, and displays live dashboard.
4. **DL MCU (Device Under Test)**: The doorlock controller being tested.

### 2.2 System Architecture Diagram
┌─────────────────────────────────────────────────────────────────────────────┐
│ DISTRIBUTED TEST SYSTEM │
├─────────────────────────────────────────────────────────────────────────────┤
│ │
│ ┌──────────────────────────────────────────────────────────────────────┐ │
│ │ YOUR WEB APPLICATION (Dashboard) │ │
│ │ Hosted on your server │ │
│ │ │ │
│ │ ┌────────────────────────────────────────────────────────────────┐ │ │
│ │ │ Live Dashboard: Board #1: 45/50 PASS | Board #2: 50/50 PASS │ │ │
│ │ │ Alerts: None | OTA: geektst_A00 → geektst_A02 │ │ │
│ │ └────────────────────────────────────────────────────────────────┘ │ │
│ └──────────────────────────────────────────────────────────────────────┘ │
│ ▲ │
│ │ WebSocket / REST API │
│ │ │
│ ┌──────────────────────────────────────────────────────────────────────┐ │
│ │ BACKEND SERVER (Your Server) │ │
│ │ - Subscribes to MQTT topics │ │
│ │ - Stores test results in database │ │
│ │ - Serves web dashboard │ │
│ └──────────────────────────────────────────────────────────────────────┘ │
│ ▲ │
│ │ MQTT Subscribe (Live Data) │
│ │ │
│ ┌──────────────────────────────────────────────────────────────────────┐ │
│ │ MQTT BROKER (Your Server) │ │
│ │ Mosquitto / HiveMQ / AWS IoT Core │ │
│ │ │ │
│ │ Topics: │ │
│ │ - test/boards/{board_id}/status │ │
│ │ - test/boards/{board_id}/test/result │ │
│ │ - test/boards/{board_id}/test/complete │ │
│ │ - test/boards/{board_id}/log │ │
│ │ - test/boards/{board_id}/alert │ │
│ │ - test/boards/{board_id}/ota/status │ │
│ │ - test/boards/{board_id}/connection │ │
│ └──────────────────────────────────────────────────────────────────────┘ │
│ ▲ ▲ ▲ │
│ │ │ │ │
│ MQTT Publish MQTT Publish MQTT Publish │
│ │ │ │ │
│ ┌─────────────┴──────┐ ┌─────┴─────┐ ┌─────┴─────┐ │
│ │ N8 Board #1 │ │ N8 Board #2│ │ N8 Board #3│ │
│ │ (Test Agent) │ │ (Test Agent)│ │ (Test Agent)│ │
│ │ │ │ │ │ │ │
│ │ - geektst_A00 │ │ - geektst_A01│ │ - geektst_A02│ │
│ │ - Testing: 45/50 │ │ - Testing: 50/50│ │ - Testing: 25/50│ │
│ │ - DL MCU Online │ │ - DL MCU Online│ │ - DL MCU Error│ │
│ └────────────────────┘ └────────────┘ └────────────┘ │
│ │
└─────────────────────────────────────────────────────────────────────────────┘

text

## SECTION 3: HARDWARE INTERFACE SPECIFICATION (UPDATED)

### 3.1 N8 Dev Board Pinout

The ESP32-C6-DevKitC-1-N8 provides full GPIO breakout. Use the following pins:

| Signal | N8 GPIO | DL MCU Pin | Direction |
|--------|---------|------------|-----------|
| TXD | GPIO5 | RX (MCU UART) | N8 → DL MCU |
| RXD | GPIO6 | TX (MCU UART) | DL MCU → N8 |
| SRDY | GPIO3 | SRDY | DL MCU → N8 (active-low) |
| MRDY | GPIO4 | MRDY | N8 → DL MCU (active-low) |
| GND | GND | GND | Common ground |

| Signal | N8 GPIO | Camera MCU Pin | Direction |
|--------|---------|----------------|-----------|
| SCK | GPIO19 | SCK | N8 → Camera |
| MOSI | GPIO18 | MOSI | N8 → Camera |
| MISO | GPIO20 | MISO | Camera → N8 |
| CS | GPIO21 | CS | N8 → Camera |
| 3.3V | 3.3V | VCC | Power |
| GND | GND | GND | Common ground |

### 3.2 Host Connection

N8 connects to laptop via USB-C for:
- Power (5V)
- Programming (initial firmware flash)
- Serial debug (115200 baud)

After initial programming, N8 operates standalone via Wi-Fi.

## SECTION 5: DEVICE PROVISIONING (UPDATED FOR N8)

### 5.1 First Boot (No Wi-Fi Credentials)

When the N8 board boots with no saved Wi-Fi credentials:

| Step | Action |
|------|--------|
| 1 | N8 starts in SoftAP mode (SSID: `ESP32-Setup-XXXX`) |
| 2 | Laptop/phone connects to this SSID (no password required) |
| 3 | Browser automatically opens to captive portal (`http://192.168.4.1`) |
| 4 | User selects Wi-Fi network and enters password |
| 5 | N8 saves credentials to NVS |
| 6 | N8 switches to STA mode, connects to Wi-Fi |
| 7 | N8 displays its IP address (on serial monitor or LCD) |
| 8 | User navigates to `http://<N8_IP>/` for SIMLOCK web interface |
| 9 | N8 connects to MQTT broker and starts publishing status |

### 5.2 Post-Provisioning Verification

| Step | Action | Expected Result |
|------|--------|-----------------|
| 1 | Note IP address shown on serial monitor | --- |
| 2 | Open browser to `http://<N8_IP>/` | SIMLOCK web page loads |
| 3 | Check MQTT connection | Board status appears in dashboard |
| 4 | Record firmware version | `geektst_A00` displayed |

## SECTION 7: REST API REFERENCE (COMPLETE)

### 7.1 Lock Control

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/lock/unlock | POST | Unlock door |
| /api/lock/lock | POST | Lock door |
| /api/lock/status | GET | Get door and lock status |

### 7.2 PIN Credential Management

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/credential/pin/otp | POST | Set One-Time PIN |
| /api/credential/pin/duration | POST | Set PIN with expiry duration |
| /api/credential/pin/permanent | POST | Set permanent password |
| /api/credential/pin/{id} | DELETE | Delete PIN by Hardware ID |
| /api/credential/pin/all | DELETE | Delete All PINs |
| /api/credential/pin/list | GET | List all PINs |
| /api/credential/pin/verify | POST | Unlock with PIN |

### 7.3 RFID Credential Management

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/credential/rfid/read | POST | Read RFID card |
| /api/credential/rfid/duration | POST | Add RFID with expiry |
| /api/credential/rfid/{id} | DELETE | Delete RFID by Hardware ID |
| /api/credential/rfid/all | DELETE | Delete All RFID Cards |
| /api/credential/rfid/list | GET | List all RFID cards |

### 7.4 Fingerprint Credential Management

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/credential/finger/read | POST | Read fingerprint |
| /api/credential/finger/duration | POST | Add fingerprint with expiry |
| /api/credential/finger/{id} | DELETE | Delete fingerprint by Hardware ID |
| /api/credential/finger/all | DELETE | Delete All Fingerprints |
| /api/credential/finger/list | GET | List all fingerprints |

### 7.5 Alerts & Buzzer

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/buzzer/beep | POST | Single beep |
| /api/buzzer/double | POST | Double beep |
| /api/buzzer/alarm | POST | Alarm pattern |
| /api/alert/failed-attempt | POST | Trigger failed-attempt alert |
| /api/alert/stuck-bolt | POST | Trigger stuck-bolt alert |
| /api/alert/high-temp | POST | Trigger high-temperature alert |
| /api/alert/duress | POST | Trigger duress alarm |

### 7.6 Backlight Control

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/backlight/flash | POST | Flash backlight 2 times |
| /api/backlight/on | POST | Turn backlight ON |
| /api/backlight/off | POST | Turn backlight OFF |

### 7.7 Logs & Status

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/log/transaction | GET | Get transaction log |
| /api/log/alert | GET | Get alert history |
| /api/status/battery | GET | Get battery level |
| /api/status/rssi | GET | Get Wi-Fi RSSI |
| /api/version | GET | Get firmware version |

### 7.8 Configuration

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/config/autolock | POST | Set Auto-Lock timer |
| /api/config/autolock/enable | PUT | Enable/Disable Auto-Lock |
| /api/config/lockout | POST | Set wrong-attempt lockout |
| /api/config/scramble | PUT | Enable/Disable Scramble PIN |
| /api/config/volume | PUT | Set volume level |
| /api/config/language | PUT | Set language |
| /api/credential/list/all | GET | Get Hardware ID List |

### 7.9 System Management

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/system/factory-reset | POST | Factory reset |
| /api/system/reboot | POST | Reboot device |
| /api/system/info | GET | Get device info |
| /api/log/export | GET | Export logs |

### 7.10 OTA & Version Management

| Endpoint | Method | Description |
|----------|--------|-------------|
| /api/version | GET | Get current firmware version |
| /api/ota/check | POST | Check for OTA update |
| /api/ota/status | GET | Get OTA update status |
| /api/ota/update | POST | Download and install firmware update |
| /api/ota/rollback | POST | Rollback to previous firmware version |
| /api/ota/trigger | POST | Trigger OTA via triple-click simulation |
| /api/ota/config | GET/PUT | Get/Set OTA configuration |
| /api/ota/history | GET | Get OTA update history |

### 7.11 WebSocket Real-Time Updates

| Endpoint | Method | Description |
|----------|--------|-------------|
| /ws | WebSocket | Real-time status updates |

## SECTION 8: MQTT REPORTING SPECIFICATION (NEW)

### 8.1 Overview

The N8 test board publishes live test results to a central MQTT broker. Your web application subscribes to these topics to display a live dashboard of all test boards.

### 8.2 MQTT Broker Configuration

| Parameter | Value |
|-----------|-------|
| **Broker** | `mqtt.ebizco.com.au` (or your server) |
| **Port** | 1883 (non-TLS) or 8883 (TLS) |
| **Client ID** | `N8-{MAC}` or `N8-{serial}` |
| **QoS** | 1 (at least once delivery) |
| **Retain** | Status topics retained |

### 8.3 MQTT Topic Structure

| Topic | Payload | Frequency | Description |
|-------|---------|-----------|-------------|
| `test/boards/{board_id}/status` | JSON | Every 10 seconds | Board health: Wi-Fi RSSI, uptime, firmware version |
| `test/boards/{board_id}/test/start` | JSON | On test start | Test suite started, test count |
| `test/boards/{board_id}/test/result` | JSON | On each test completion | Individual test result (PASS/FAIL) |
| `test/boards/{board_id}/test/complete` | JSON | On test suite complete | Summary: total passed, failed, skipped |
| `test/boards/{board_id}/log` | JSON | On each log event | Detailed log messages |
| `test/boards/{board_id}/alert` | JSON | On alert triggered | Alerts (failed attempts, stuck bolt, etc.) |
| `test/boards/{board_id}/ota/status` | JSON | On OTA progress | OTA update progress |
| `test/boards/{board_id}/connection` | JSON | On connect/disconnect | Board online/offline status |

### 8.4 MQTT Message Formats

#### Board Status (Heartbeat)

**Topic:** `test/boards/{board_id}/status`

```json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:00Z",
  "firmware_version": "geektst_A00",
  "uptime_seconds": 3600,
  "wifi": {
    "ssid": "Office-WiFi",
    "rssi": -55,
    "ip": "192.168.1.100"
  },
  "dl_mcu": {
    "connected": true,
    "last_heartbeat": "2026-08-04T10:29:55Z"
  },
  "heap_usage_percent": 42
}
Individual Test Result
Topic: test/boards/{board_id}/test/result

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:05Z",
  "test_id": 1,
  "test_name": "Open Door (Direct Unlock)",
  "section": "Direct Control",
  "status": "PASS",
  "duration_ms": 250,
  "details": "Door unlocked successfully"
}
Test Complete (Summary)
Topic: test/boards/{board_id}/test/complete

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:35:00Z",
  "summary": {
    "total": 50,
    "passed": 48,
    "failed": 2,
    "skipped": 0,
    "pass_rate": 96.0
  },
  "failed_tests": [
    {"test_id": 25, "name": "Failed-Attempt Alert", "error": "Alert not triggered"}
  ],
  "duration_seconds": 300
}
Log Message
Topic: test/boards/{board_id}/log

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:10Z",
  "level": "INFO",
  "source": "UART",
  "message": "PIN 123456 added with expiry 5 minutes"
}
Alert
Topic: test/boards/{board_id}/alert

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:15Z",
  "alert_type": "failed_attempt",
  "severity": "WARNING",
  "details": "Fingerprint failed attempts (5 attempts)"
}
OTA Status
Topic: test/boards/{board_id}/ota/status

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:20Z",
  "status": "downloading",
  "current_version": "geektst_A00",
  "target_version": "geektst_A02",
  "progress_percent": 45,
  "error_message": null
}
Board Connection Status
Topic: test/boards/{board_id}/connection

json
{
  "board_id": "N8-001",
  "timestamp": "2026-08-04T10:30:00Z",
  "status": "online"
}
8.5 Remote Commands (Server → Board)
The server can send commands to the board via MQTT:

Command	Topic	Payload	Description
Start Test	test/boards/{board_id}/command	{"action": "start_test"}	Start full test suite
Restart Board	test/boards/{board_id}/command	{"action": "restart"}	Reboot board
Check OTA	test/boards/{board_id}/command	{"action": "check_ota"}	Check for OTA update
Factory Reset	test/boards/{board_id}/command	{"action": "factory_reset"}	Reset all credentials
SECTION 9: REQUIREMENTS TRACEABILITY MATRIX
Test	Description	Section
1	Open Door	§6.1
2	Lock Door	§6.1
3	Get Door Status	§6.1
4	Get Lock Status	§6.1
5	Set One-Time PIN	§6.2
6	Set PIN with Duration	§6.2
7	Set Permanent Password	§6.2
8	Delete PIN by Hardware ID	§6.2
9	Delete All PINs	§6.2
10	List All PINs	§6.2
11	Unlock with PIN (web)	§6.2
12	Read RFID Card	§6.3
13	Add RFID with Duration	§6.3
14	Delete RFID by Hardware ID	§6.3
15	Delete All RFID Cards	§6.3
16	List All RFID Cards	§6.3
17	Read Fingerprint	§6.4
18	Add Fingerprint with Duration	§6.4
19	Delete Fingerprint by Hardware ID	§6.4
20	Delete All Fingerprints	§6.4
21	List All Fingerprints	§6.4
22	Buzzer: Single Beep	§6.6
23	Buzzer: Double Beep	§6.6
24	Buzzer: Alarm Pattern	§6.6
25	Failed-Attempt Alert	§6.6
26	Stuck Bolt Alert	§6.6
27	High Temperature Alert	§6.6
28	Duress Alarm	§6.6
29	Backlight: Flash 2 Times	§6.7
30	Backlight: ON	§6.7
31	Backlight: OFF	§6.7
32	Get Transaction Log	§6.8
33	Get Alert History	§6.8
34	Get Battery Level	§6.8
35	Get Wi-Fi RSSI	§6.8
36	Get Firmware Version	§6.8
37	Set Auto-Lock Timer	§6.9
38	Enable/Disable Auto-Lock	§6.9
39	Set Wrong-Attempt Lockout	§6.9
40	Enable/Disable Scramble PIN	§6.9
41	Set Volume Level	§6.9
42	Set Language	§6.9
43	Get Hardware ID List	§6.9
44	Factory Reset	§6.10
45	Reboot Device	§6.10
46	Get Device Info	§6.10
47	Export Logs	§6.10
48	Version Cycling	§6.11
49	OTA Update Check	§6.11
50	Triple-Click Boot Detection	§6.11
OTA FIRMWARE VERSIONING SCHEME
Version Format
geektst_ followed by a letter (A–Z) and a two‑digit number (00–99)

Version Progression
A00 → A01 → A02 → ... → A99

Then B00 → B01 → B02 → ... → B99

Then C00 → C01 → ... → Z99

After Z99, wrap to A00

Firmware Binary Naming
geektst_A00.ino

geektst_A01.ino

...

geektst_A99.ino

geektst_B00.ino

OTA Server URL
text
https://ebizco.com.au/doorlock/test/dle32/
Version JSON URL
text
https://ebizco.com.au/doorlock/test/dle32/version.json
version.json Format
json
{
  "latest_version": "geektst_A02",
  "release_date": "2026-08-04",
  "changelog": "Fixed Wi-Fi reconnection bug, improved handshake reliability",
  "size_bytes": 1048576,
  "md5": "a3b7c9d1e5f2a4b6c8d0e1f2a3b4c5d6",
  "url": "https://ebizco.com.au/doorlock/test/dle32/geektst_A02.ino",
  "force_update": false
}
OUTPUT REQUIREMENTS
Generate a complete, polished technical specification document (Microsoft Word .docx format compatible) with:

All sections above included

Professional formatting (tables, figures, headings)

All 50 test cases with procedures and pass criteria

Complete DPID command set (41 commands)

Frame structure specification

Credential data structure

Complete OTA API with version JSON format

Complete MQTT reporting specification (NEW)

WebSocket specification

Error handling specification

Full traceability matrix

Revision history (Version 2.0)

All appendices (test environment, issue log, approvals)

OTA server URL and version naming convention documented in Section 7.10

N8 Dev Board pinout and hardware specification (UPDATED)

WHY THIS SYSTEM PROTECTS YOUR INVESTMENT
Investment	How This System Protects It
Hardware cost	Verify each board works before shipping
Firmware development	OTA updates fix bugs remotely — no recalls
Engineer time	Live test results visible from anywhere
Quality assurance	Every test logged, timestamped, traceable
Supply chain	Compare hardware revisions side-by-side
Customer trust	Know exactly what works before it ships