Act as a Principal Embedded Solutions Architect and Senior Full-Stack Engineer. Create a standalone laboratory prototyping workspace for our Sovereign Smart Lock enterprise ecosystem. This setup handles physical onboarding (Pairing) and steady-state credential updates, bypassing third-party PMS billing systems.

Generate a single-repository setup containing two core components:
1. `server.js`: A Node.js API Gateway, Rule Engine, and MySQL manager (Port 4000).
2. `pages/index.js` (or app/page.js): A Next.js/React dark-mode dashboard (Port 3100) using Chrome's Web Serial API.

---

### 1. BACKEND ENGINE ARCHITECTURE (OZKEYSERV/ - server.js)

Using `express`, `mysql2`, `mqtt`, and `cors`, build a script with these specifications:

#### A. MySQL Connection & Onboarding Schema
Connect to MySQL using Host: `localhost`, Database: `ozkey`, User: `root`, Password: `Cableman`.
Establish these relational tables on startup:
- `rooms`: id (INT AUTO_INCREMENT PK), building (VARCHAR(255)), floor (INT), room_no (VARCHAR(50) UNIQUE), mac_address (VARCHAR(17) UNIQUE NULL), status (VARCHAR(50) DEFAULT 'Available')
  * AUTOMATION: On empty initialization, auto-populate 100 rooms (Block A, Floors 1-5, Rooms 101-120 per floor). Leave mac_address as NULL initially.
- `users`: id (INT AUTO_INCREMENT PK), name (VARCHAR(255)), role (VARCHAR(50) DEFAULT 'Staff'), status (VARCHAR(50) DEFAULT 'active')
- `credentials`: id (INT AUTO_INCREMENT PK), room_id (INT), user_id (INT), type ENUM('pin', 'rfid', 'fingerprint'), slot_number (INT), raw_value (VARCHAR(255)), date_from (VARCHAR(50)), date_to (VARCHAR(50)), sync_status (VARCHAR(50) DEFAULT 'pending'), FOREIGN KEY(room_id) REFERENCES rooms(id), FOREIGN KEY(user_id) REFERENCES users(id)
- `pending_queue`: id (INT AUTO_INCREMENT PK), room_no (VARCHAR(50)), credential_id (INT), action_type (VARCHAR(50)), payload_hex (TEXT), status (VARCHAR(50) DEFAULT 'queued')

#### B. REST API Onboarding & Provisioning Endpoints
- `GET /ozkeyserv/api/locks/unpaired`: Returns an array of newly discovered MAC addresses currently hitting the MQTT broker that are not yet assigned to any row in the `rooms` table.
- `POST /ozkeyserv/api/locks/pair`: Accepts `{ room_no, mac_address }`. Binds the `mac_address` to that target room row, changes room status to 'Available', and sends an MQTT confirmation payload back down to the device.
- `POST /ozkeyserv/api/pms/issue-key`: Standard key injector payload creating users, queue entries, and generating Tuya hex packages (`55 AA` frames).

#### C. Dual MQTT Handshake Sync Engine
Connect to TalkPOS Mosquitto Broker at `mqtt://10.1.1.21:1883`.
1. Listen on `hotel/locks/unpaired/heartbeat` for unprovisioned devices broadcasting their raw MAC address. Cache these for the dashboard `unpaired` endpoint.
2. Listen on `hotel/rooms/+/lock/heartbeat` for provisioned heartbeats. If a 30-second heartbeat arrives, extract the `room_no`, find any 'queued' actions for it, wrap the hex payload in JSON, and burst it down to `hotel/rooms/[room_no]/lock/command`.

---

### 2. CORE COCKPIT UI (OZKEY Front-End - Next.js)

Design a scannable dark-mode UI (Background: `#0F172A`, Panels: `#1E293B`) showcasing:

#### A. 100-Room Matrix & Pairing Cockpit
- Left Side: 10x10 grid visualizing all 100 rooms. Gray if unpaired (`mac_address` is null), Green if paired and `Available`, Blue if `Occupied`, Red if `PendingUpdate`.
- Top Banner / Sidebar: "Discovered Unpaired Hardware". Displays MAC addresses detected over the serial link or unpaired MQTT channel. Includes a dropdown to select a Room Number and a "PAIR LOCK TO ROOM" execution button.

#### B. Credential Injector & Lab Logging Terminal
- Input forms to issue PIN/RFID tokens to paired locks via the API gateway.
- An interactive Web Serial connection button to read raw strings directly from your desk test module, alongside a scrolling green logging terminal outputting real-time pairing and sync transitions.

Do not truncate code, leave empty functions, or use placeholders. Provide a completely operational production script for both modules.
