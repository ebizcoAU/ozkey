# OZKEY — Development Record 01

> Session log for the initial build of the Sovereign Smart Lock laboratory
> workspace. Written so a fresh engineering session (human or AI) can pick up
> exactly where this one left off. Recorded 2026-07-05/06.

---

## 1. What this project is

A standalone lab bench for the **Sovereign Smart Lock** enterprise ecosystem.
It handles **physical onboarding (pairing)** of lock hardware and
**steady-state credential updates**, deliberately bypassing third-party PMS
billing systems. Two components in one repo:

| Component | Path | Stack | Port |
|---|---|---|---|
| API Gateway / Rule Engine / MySQL manager | `ozkeyserv/server.js` | Node 22, Express, mysql2, mqtt, cors | **3200** |
| Core Cockpit dashboard | `ozkey/pages/index.js` | Next.js 14 (pages router), React 18, Web Serial API | **3300** |

Repo: `https://github.com/ebizcoAU/ozkey.git` (branch `main`).

**Port history (important):** the original spec asked for gateway :4000 and UI
:3100. Something else on this Mac already listens on :4000 (responds
`{"success":false,"message":"... not found"}` — an unrelated service), so the
user directed: **gateway :3200, cockpit :3300**. Don't move back to 4000.

**Folder-name history:** the UI was scaffolded as `ozkey-ui/`; the user renamed
it to `ozkey/` ("the app name"). The repo root folder is *also* named `ozkey`,
so paths look like `ozkey/ozkey/pages/index.js` from the parent dir. Package
name inside is still `ozkey-ui` (harmless).

## 2. Environment facts (verified working)

- macOS, zsh, Node v22.17.1, npm 10.9.2; the user prefers **pnpm**
  (`pnpm -C ozkeyserv run dev`, `pnpm -C ozkey run dev`).
- **MySQL** at `localhost`, user `root`, password `Cableman` — running from
  `/usr/local/mysql/bin` (not brew). Database `ozkey` is auto-created by the
  gateway on boot. ⚠️ The password is hardcoded in `server.js` and is now in
  the GitHub repo — flagged to the user; recommended migration to `.env`
  (already gitignored) + password rotation if repo is/becomes public.
- **MQTT broker** (TalkPOS Mosquitto) at `mqtt://10.1.1.21:1883` — **reachable
  and confirmed connected** from this machine on first try. Real lab network.
- Web Serial requires Chrome/Edge on localhost or HTTPS.

## 3. Database schema (auto-created on gateway boot)

- `rooms(id PK AI, building VARCHAR255, floor INT, room_no VARCHAR50 UNIQUE, mac_address VARCHAR17 UNIQUE NULL, status VARCHAR50 DEFAULT 'Available')`
- `users(id PK AI, name, role DEFAULT 'Staff', status DEFAULT 'active')`
- `credentials(id PK AI, room_id FK→rooms, user_id FK→users, type ENUM('pin','rfid','fingerprint'), slot_number INT, raw_value, date_from, date_to, sync_status DEFAULT 'pending')`
- `pending_queue(id PK AI, room_no, credential_id, action_type, payload_hex TEXT, status DEFAULT 'queued')`

**Seeding:** if `rooms` is empty at boot → auto-provision **30 rooms**:
Block A, floors 1–3, rooms 101–110 / 201–210 / 301–310 (`floor*100 + door`).
*History:* spec said 100 rooms (floors 1–5 × rooms 101–120); user cut to 30 on
2026-07-05 so the Lab Terminal fits on screen. To force a reseed: delete rows
from `pending_queue`, `credentials`, `users`, `rooms` (that FK order), then
restart the gateway.

## 4. Gateway design (`ozkeyserv/server.js`)

Single file, ~600 lines. Key structures:

- `CONFIG` object at top — ports, DB creds, broker URL, topic builders,
  `UNPAIRED_TTL_MS` (120 s discovery-cache expiry).
- `unpairedCache: Map<mac, {mac, firstSeen, lastSeen, rssi, fw}>` — in-memory
  discovery pool; pruned every 30 s.
- `eventRing` (max 500) + `logEvent(level, message)` — feeds both console and
  the dashboard terminal via `GET /events?after=<id>`. Levels used:
  `info|warn|error|pair|key|sync`.
- **Tuya frame codec** (rewritten 2026-07-06 for LockSim conformance — gap #3):
  `55 AA | version 00 | cmd | len(2,BE) | payload | checksum` (checksum = sum of
  preceding bytes & 0xFF). Credentials are **DP_REPORT (cmd 0x06)** frames with
  DP wrapper `[dpid 1B][type 1B][len 2B BE][value]`: DPID 21 add-PIN / 23
  add-RFID (RAW value `[slot 2B BE][cred bytes][start u32 BE][end u32 BE]`,
  PIN = ASCII digits, RFID = raw UID bytes) and DPID 22/24 deletes
  (`[slot 2B BE]`). Byte-matches `locksim/lib/tuya.ts` samples. `fingerprint`
  type is held (422). The old custom `0x65/0x66/0x02` command IDs are gone.
- MySQL boot: create DB if missing → pool → create tables → seed. Retries
  forever every 5 s if MySQL is down (HTTP doesn't listen until DB is up).
- MQTT: reconnects every 5 s; QoS 1 everywhere.

### MQTT contract

| Topic | Dir | Purpose |
|---|---|---|
| `hotel/locks/unpaired/heartbeat` | lock→srv | Factory lock broadcasts MAC (bare string **or** JSON `{mac, rssi, fw}`) → cached for discovery |
| `hotel/rooms/<room_no>/lock/command` (pairing) | srv→lock | `provision_assign` handshake on pair (gap #2): `{topic, op, mac, room_no, server_ip, server_port, mac_token, issued_by}` — no `payload_hex` key. Duplicated on `hotel/locks/<mac-no-colons-lowercase>/pair/confirm` as a debug side channel |
| `hotel/rooms/<room_no>/lock/heartbeat` | lock→srv | 30 s heartbeat; triggers `flushQueueForRoom(room_no)` |
| `hotel/rooms/<room_no>/lock/command` | srv→lock | JSON envelope `{msg_id, room_no, action, credential_id, payload_hex, issued_at, source}` |

### REST API (base `http://localhost:3200/ozkeyserv/api`)

| Endpoint | Method | Notes |
|---|---|---|
| `/health` | GET | `{ok, db, mqtt, unpaired_cached, uptime_s}` |
| `/rooms` | GET | full matrix, ordered by floor+room_no |
| `/locks/unpaired` | GET | discovery cache minus already-bound MACs |
| `/locks/pair` | POST | `{room_no, mac_address}`; 409 on double-bind either direction; publishes pair confirm |
| `/locks/unpair` | POST | `{room_no}` — lab convenience, releases lock |
| `/pms/issue-key` | POST | `{room_no, guest_name, role?, type, raw_value, slot_number?, date_from?, date_to?}` — transactional: INSERT user + credential + queue row, room→`PendingUpdate`, then opportunistic immediate flush |
| `/queue`, `/credentials` | GET | introspection (last 100) |
| `/events?after=<id>` | GET | terminal feed |
| `/sim/unpaired-heartbeat` | POST | `{mac_address}` — fake discovery without broker/hardware |
| `/sim/room-heartbeat` | POST | `{room_no}` — fake heartbeat, flushes queue |

### Room status lifecycle

`mac_address NULL` (gray, unpaired) → pair → `Available` (green) → issue-key →
`PendingUpdate` (red) → heartbeat flush drains queue → `Occupied` (blue).
Flush marks queue rows `sent` and credentials `synced`.

## 5. Cockpit design (`ozkey/pages/index.js`)

Single-page, one component `Cockpit`, inline styles + one `<style jsx global>`.
Palette: bg `#0F172A`, panels `#1E293B`, edges `#334155`; status colors
gray `#475569` / green `#22C55E` / blue `#3B82F6` / red `#EF4444`.

- Polls gateway every **2.5 s**: `/health`, `/rooms`, `/locks/unpaired`,
  `/events?after=lastId` (merged into terminal, ring-capped at 400 lines).
- **Layout (after the 2026-07-05 "compact chrome" pass — user wants minimal
  labels / max real estate):**
  1. Slim header row: `OZKEY // LOCK COCKPIT` + GATEWAY/MQTT/SERIAL status dots.
  2. Single-row pairing strip: `UNPAIRED HW` tag → clickable MAC chips (source
     tag MQTT/SERIAL/SIM) → target-room `<select>` (unpaired rooms only) →
     `PAIR LOCK TO ROOM` button.
  3. Main split: left = 10-column room grid (30 tiles, click gray tile →
     selects as pair target; click paired tile → selects in injector);
     right = Credential Injector (paired-rooms dropdown, name, type
     pin/rfid/fingerprint, slot, value, datetime-local validity range) +
     Web Serial connect/disconnect button.
  4. `Lab Terminal` — black scroll box, auto-scroll, color-coded levels,
     merges server events + local UI actions.
- **Web Serial:** 115200 baud, line-buffered read loop; regex-scans the stream
  for MACs (`XX:XX:...` or 12 bare hex); every new MAC → local chip list +
  `POST /sim/unpaired-heartbeat` so the gateway can pair it. Uses a
  `serialMacsRef` mirror because the read loop is long-lived.
- Design principle from user feedback: **remove labels where not needed,
  shrink everything** — don't reintroduce verbose headings/subtitles.

## 6. Verification already performed (2026-07-05)

Full pipeline exercised against real MySQL + real broker:
sim discovery `AA:BB:CC:11:22:33` → appeared in `/locks/unpaired` → paired to
room 101 → issued PIN `482913` slot 3 → frame
`55 AA 00 65 00 11 01 03 06 34 38 32 39 31 33 ...` queued → flush burst it down
`hotel/rooms/101/lock/command` → queue `sent`, credential `synced`, room
`Occupied`. Test data was then wiped. `next build` passes clean.

## 7. Git history

```
c5849c9  Initial commit: full workspace (server + cockpit + README + docs/app_creation.md)
8788c7f  Add dev script (node --watch) to ozkeyserv
edda843  Shrink lab matrix from 100 to 30 rooms (Block A, floors 1-3)
36d45fd  Compact cockpit chrome to maximize working real estate
```

`.gitignore` covers `node_modules/`, `.next/`, logs, `.env*`, `.DS_Store`,
`.claude/settings.local.json`.

## 8. How to run

```bash
pnpm -C ozkeyserv run dev   # gateway :3200 (node --watch, auto-restarts on edit)
pnpm -C ozkey run dev       # cockpit :3300
# open http://localhost:3300 in Chrome
```

If `EADDRINUSE`: a stray background instance is holding the port —
`pkill -f "node server.js"` / `pkill -f "next dev"`.

## 9. Open items / next steps

- [ ] Move MySQL password (and broker URL) from hardcoded `CONFIG` into `.env`;
      rotate `Cableman` if the repo is public.
- [ ] Terminal height is fixed at 260 px — candidate improvement: flex-fill the
      remaining viewport (user hinted interest).
- [ ] `credentials` REVOKE flow exists as a frame command (`0x66`) but has no
      endpoint/UI yet.
- [ ] Real lock hardware not yet tested end-to-end (only sim + broker); Web
      Serial path untested against a physical desk module.
- [ ] `docs/app_creation.md` is the user's own doc — do not overwrite.
