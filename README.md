# OZKEY — Sovereign Smart Lock Laboratory Workspace

Standalone lab bench for the Sovereign Smart Lock enterprise ecosystem: physical
onboarding (pairing) plus steady-state credential sync, bypassing third-party PMS
billing systems.

```
ozkey/
├── ozkeyserv/          Node.js API Gateway + Rule Engine + MySQL manager  (port 3200)
│   └── server.js
└── ozkey/           Next.js dark-mode cockpit (Web Serial + pairing)   (port 3300)
    └── pages/index.js
```

## Prerequisites

- Node.js 18+ (tested on 22.x)
- MySQL on `localhost` — user `root` / password `Cableman` (database `ozkey` is
  auto-created, and 100 rooms are auto-seeded: Block A, floors 1–5, rooms
  101–120 per floor)
- TalkPOS Mosquitto broker reachable at `mqtt://10.1.1.21:1883` (the gateway
  keeps retrying every 5 s if the broker is down)
- Chrome or Edge for the Web Serial desk-module link

## Run

```bash
# Terminal 1 — gateway
cd ozkeyserv && npm install && npm start        # http://localhost:3200

# Terminal 2 — cockpit
cd ozkey     && npm install && npm run dev      # http://localhost:3300
```

## MQTT contract

| Topic | Direction | Purpose |
|---|---|---|
| `hotel/locks/unpaired/heartbeat` | lock → server | Factory-fresh lock broadcasts its raw MAC (bare string or `{"mac":"..."}`) |
| `hotel/locks/<mac>/pair/confirm` | server → lock | Pairing confirmation with assigned room + command/heartbeat topics |
| `hotel/rooms/<room_no>/lock/heartbeat` | lock → server | 30 s heartbeat from a provisioned lock; triggers queue flush |
| `hotel/rooms/<room_no>/lock/command` | server → lock | JSON envelope carrying the Tuya `55 AA` hex frame |

## REST API (base `http://localhost:3200/ozkeyserv/api`)

| Endpoint | Method | Purpose |
|---|---|---|
| `/health` | GET | Gateway / DB / MQTT status |
| `/rooms` | GET | Full 100-room matrix |
| `/locks/unpaired` | GET | MACs broadcasting on the broker not yet bound to a room |
| `/locks/pair` | POST | `{ room_no, mac_address }` — bind lock, confirm over MQTT |
| `/locks/unpair` | POST | `{ room_no }` — release a lock back to discovery |
| `/pms/issue-key` | POST | `{ room_no, guest_name, type, raw_value, slot_number, date_from, date_to }` — creates user + credential, builds `55 AA` frame, queues for next heartbeat |
| `/queue` | GET | Pending queue (last 100) |
| `/credentials` | GET | Issued credentials joined with rooms/users |
| `/events?after=<id>` | GET | Terminal event feed (dashboard polls this) |
| `/sim/unpaired-heartbeat` | POST | Lab hook: inject a discovered MAC without the broker |
| `/sim/room-heartbeat` | POST | Lab hook: fake a provisioned heartbeat to flush the queue |

## Bench workflow

1. Power a factory lock (or use `/sim/unpaired-heartbeat`, or capture its MAC
   over the cockpit's Web Serial link) — it appears under **Discovered
   Unpaired Hardware**.
2. Pick the MAC + target room → **PAIR LOCK TO ROOM**. Room tile turns green.
3. Issue a PIN/RFID/fingerprint credential via the injector. Room turns red
   (`PendingUpdate`) and the Tuya frame sits in `pending_queue`.
4. On the lock's next heartbeat the gateway bursts the frame down the command
   topic, marks the credential `synced`, and the room settles blue (`Occupied`).

Room tile legend: **gray** unpaired · **green** paired + Available ·
**blue** Occupied · **red** PendingUpdate.
