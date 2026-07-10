/*
 * ============================================================================
 *  OZKEYSERV — Sovereign Smart Lock Laboratory Gateway
 *  ---------------------------------------------------------------------------
 *  Role     : API Gateway + Rule Engine + MySQL Manager
 *  Port     : 4000
 *  Broker   : TalkPOS Mosquitto @ mqtt://10.1.1.21:1883
 *  Database : MySQL (localhost / ozkey)
 *
 *  Responsibilities
 *    1. Bootstrap relational schema + auto-seed 30-room matrix (Block A)
 *    2. Cache unprovisioned lock heartbeats  (hotel/locks/unpaired/heartbeat)
 *    3. Bind MAC -> room  (physical onboarding / pairing)
 *    4. Issue credentials as Tuya 55 AA serial frames, queue them, and burst
 *       them down on the next provisioned heartbeat
 *       (hotel/rooms/+/lock/heartbeat  ->  hotel/rooms/<room>/lock/command)
 * ============================================================================
 */

'use strict';

const express = require('express');
const cors = require('cors');
const mysql = require('mysql2/promise');
const mqtt = require('mqtt');
const os = require('os');

/** First non-internal IPv4 of this host — the lock's consistency value for
 *  the gateway address (ozkey-02 §3.2). Override with OZKEY_SERVER_IP. */
function detectLanIp() {
  for (const ifaces of Object.values(os.networkInterfaces())) {
    for (const iface of ifaces || []) {
      if (iface.family === 'IPv4' && !iface.internal) return iface.address;
    }
  }
  return '127.0.0.1';
}

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
const CONFIG = {
  HTTP_PORT: 3200,
  SERVER_IP: process.env.OZKEY_SERVER_IP || detectLanIp(),
  DB: {
    host: 'localhost',
    user: 'root',
    password: 'Cableman',
    database: 'ozkey',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL: 'mqtt://10.1.1.21:1883',
  TOPIC_UNPAIRED_HEARTBEAT: 'hotel/locks/unpaired/heartbeat',
  TOPIC_ROOM_HEARTBEAT: 'hotel/rooms/+/lock/heartbeat',
  TOPIC_LOCK_LOG: 'hotel/locks/+/log',
  topicRoomCommand: (roomNo) => `hotel/rooms/${roomNo}/lock/command`,
  topicPairConfirm: (mac) => `hotel/locks/${mac.replace(/:/g, '').toLowerCase()}/pair/confirm`,
  UNPAIRED_TTL_MS: 120_000, // forget an unpaired MAC if silent for 2 minutes
  // Shared secret for PMS write endpoints (ozkey-07 §4.4). Unset = open (lab);
  // set OZKEY_PMS_SECRET to enforce `X-OZKEY-Secret` on /pms/* writes.
  PMS_SECRET: process.env.OZKEY_PMS_SECRET || '',
};

/* ---------------------------------------------------------------------------
 * In-memory state
 * ------------------------------------------------------------------------- */
/** @type {Map<string, {mac: string, firstSeen: number, lastSeen: number, rssi: number|null, fw: string|null}>} */
const unpairedCache = new Map();

/** Rolling event log served to the dashboard terminal (ring buffer). */
const EVENT_RING_MAX = 500;
let eventSeq = 0;
const eventRing = [];

function logEvent(level, message) {
  const evt = { id: ++eventSeq, ts: new Date().toISOString(), level, message };
  eventRing.push(evt);
  if (eventRing.length > EVENT_RING_MAX) eventRing.shift();
  const tag = level.toUpperCase().padEnd(5);
  console.log(`[${evt.ts}] ${tag} ${message}`);
  return evt;
}

/* ---------------------------------------------------------------------------
 * MAC helpers
 * ------------------------------------------------------------------------- */
function normalizeMac(raw) {
  if (typeof raw !== 'string') return null;
  const hex = raw.replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(':');
}

/* ---------------------------------------------------------------------------
 * Tuya 55 AA frame codec — conformant with LockSim's decoder (lib/tuya.ts),
 * which is the hardware truth (ozkey-02 §4).
 *
 *   Frame:      [0x55][0xAA][ver 00][cmd][len 2B BE][payload][checksum]
 *               checksum = (sum of every preceding byte) & 0xFF
 *   DP payload: [dpid 1B][type 1B][len 2B BE][value]
 *   Temp cred:  [slot 2B BE][credential var][start u32 BE][end u32 BE]
 *               PIN credential = ASCII digit bytes; RFID = raw UID bytes
 * ------------------------------------------------------------------------- */
const TUYA_CMD = {
  HEARTBEAT: 0x00,
  DP_REPORT: 0x06,
};

const DPID = {
  ADD_TEMP_PIN: 21,
  DELETE_PIN: 22,
  ADD_TEMP_RFID: 23,
  DELETE_RFID: 24,
};

const DP_TYPE = { RAW: 0x00, BOOL: 0x01, VALUE: 0x02, STRING: 0x03, ENUM: 0x04 };

/** Credential types the DP codec can express. `fingerprint` is held (gap #6). */
const SUPPORTED_CRED_TYPES = ['pin', 'rfid'];

function buildTuyaFrame(command, payloadBuf) {
  const head = Buffer.alloc(6);
  head[0] = 0x55;
  head[1] = 0xaa;
  head[2] = 0x00; // protocol version
  head[3] = command & 0xff;
  head.writeUInt16BE(payloadBuf.length, 4);
  const body = Buffer.concat([head, payloadBuf]);
  let sum = 0;
  for (const b of body) sum = (sum + b) & 0xff;
  return Buffer.concat([body, Buffer.from([sum])]);
}

function buildDpPayload(dpId, dpType, valueBuf) {
  const head = Buffer.alloc(4);
  head[0] = dpId & 0xff;
  head[1] = dpType & 0xff;
  head.writeUInt16BE(valueBuf.length, 2);
  return Buffer.concat([head, valueBuf]);
}

function toSpacedHex(buf) {
  return buf
    .toString('hex')
    .toUpperCase()
    .match(/.{2}/g)
    .join(' ');
}

/** PIN -> ASCII digit bytes; RFID -> raw UID bytes from a hex string. */
function credentialValueBytes(type, rawValue) {
  const value = String(rawValue).trim();
  if (type === 'pin') {
    if (!/^\d+$/.test(value)) {
      throw new Error(`PIN must be digits only (got "${value}")`);
    }
    return Buffer.from(value, 'ascii');
  }
  // rfid: accept "04 A3 7F 1C", "04:A3:7F:1C" or "04A37F1C"
  const hex = value.replace(/[^0-9a-fA-F]/g, '');
  if (hex.length === 0 || hex.length % 2 !== 0) {
    throw new Error(`RFID UID must be an even-length hex string (got "${value}")`);
  }
  return Buffer.from(hex, 'hex');
}

/**
 * DP_REPORT frame carrying DPID 21 (Add Temp PIN) / 23 (Add Temp RFID), RAW:
 *   [slot 2B BE][credential][start u32 BE][end u32 BE]
 * Byte-compatible with LockSim's buildTempCredential()/parseTempCredential().
 */
function buildCredentialFrame({ type, slotNumber, rawValue, dateFrom, dateTo }) {
  if (!SUPPORTED_CRED_TYPES.includes(type)) {
    throw new Error(`unsupported credential type "${type}" for the DP codec`);
  }
  const credBytes = credentialValueBytes(type, rawValue);
  const fromTs = Math.floor(new Date(dateFrom).getTime() / 1000) || 0;
  const toTs = Math.floor(new Date(dateTo).getTime() / 1000) || 0;

  const value = Buffer.alloc(2 + credBytes.length + 8);
  value.writeUInt16BE(slotNumber & 0xffff, 0);
  credBytes.copy(value, 2);
  value.writeUInt32BE(fromTs >>> 0, 2 + credBytes.length);
  value.writeUInt32BE(toTs >>> 0, 2 + credBytes.length + 4);

  const dpId = type === 'pin' ? DPID.ADD_TEMP_PIN : DPID.ADD_TEMP_RFID;
  return buildTuyaFrame(TUYA_CMD.DP_REPORT, buildDpPayload(dpId, DP_TYPE.RAW, value));
}

/** DP_REPORT frame carrying DPID 22/24 (Delete PIN/RFID), RAW: [slot 2B BE]. */
function buildDeleteFrame({ type, slotNumber }) {
  if (!SUPPORTED_CRED_TYPES.includes(type)) {
    throw new Error(`unsupported credential type "${type}" for the DP codec`);
  }
  const value = Buffer.alloc(2);
  value.writeUInt16BE(slotNumber & 0xffff, 0);
  const dpId = type === 'pin' ? DPID.DELETE_PIN : DPID.DELETE_RFID;
  return buildTuyaFrame(TUYA_CMD.DP_REPORT, buildDpPayload(dpId, DP_TYPE.RAW, value));
}

/** Broker-side network token issued at pairing (ozkey-02 §3.2). */
function makeMacToken() {
  const seg = () =>
    Math.floor(Math.random() * 0x10000)
      .toString(16)
      .toUpperCase()
      .padStart(4, '0');
  return `OZK-${seg()}-${seg()}-${seg()}`;
}

/* ---------------------------------------------------------------------------
 * MySQL bootstrap — schema + 100-room auto-seed
 * ------------------------------------------------------------------------- */
let pool = null;

async function initDatabase() {
  // 1. Ensure the database itself exists (connect server-level first).
  const admin = await mysql.createConnection({
    host: CONFIG.DB.host,
    user: CONFIG.DB.user,
    password: CONFIG.DB.password,
    multipleStatements: false,
  });
  await admin.query(
    `CREATE DATABASE IF NOT EXISTS \`${CONFIG.DB.database}\`
     CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`
  );
  await admin.end();

  // 2. Pooled connection into the ozkey schema.
  pool = mysql.createPool(CONFIG.DB);

  // 3. Relational tables.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS rooms (
      id INT AUTO_INCREMENT PRIMARY KEY,
      building VARCHAR(255),
      floor INT,
      room_no VARCHAR(50) UNIQUE,
      mac_address VARCHAR(17) UNIQUE NULL,
      status VARCHAR(50) DEFAULT 'Available'
    ) ENGINE=InnoDB`);

  // Broker-side network token issued at pairing (ozkey-02 §3.2); additive
  // migration for tables created before the column existed.
  const [[{ hasTokenCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasTokenCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'rooms' AND column_name = 'mac_token'`,
    [CONFIG.DB.database]
  );
  if (!hasTokenCol) {
    await pool.query('ALTER TABLE rooms ADD COLUMN mac_token VARCHAR(50) NULL');
  }

  // PMS roster-sync columns (ozkey-07 §4): MAOI is the source of truth for room
  // definitions; these mirror what the PMS pushes. Additive migrations.
  //   maoi_id       — PMS stable row id, the upsert/join key (ozkey-07 §4.3)
  //   name/type     — display label + room-type label (rich model stays in PMS)
  //   capacity      — occupancy
  //   lock_device_id— the room↔lock binding, carried in-band (ozkey-07 §6)
  //   active        — 0 = deactivated (removed in a reconcile, kept non-destructively)
  //   last_synced_at— per-row sync stamp for GET /pms/rooms/status
  const pmsCols = [
    ['maoi_id', 'VARCHAR(64) NULL'],
    ['name', 'VARCHAR(255) NULL'],
    ['room_type', 'VARCHAR(100) NULL'],
    ['capacity', 'INT DEFAULT 1'],
    ['lock_device_id', 'VARCHAR(64) NULL'],
    ['active', 'TINYINT DEFAULT 1'],
    ['last_synced_at', 'DATETIME NULL'],
  ];
  for (const [col, def] of pmsCols) {
    const [[{ has }]] = await pool.query(
      `SELECT COUNT(*) AS has FROM information_schema.columns
        WHERE table_schema = ? AND table_name = 'rooms' AND column_name = ?`,
      [CONFIG.DB.database, col]
    );
    if (!has) await pool.query(`ALTER TABLE rooms ADD COLUMN ${col} ${def}`);
  }
  // Unique index on maoi_id so upserts join cleanly (nullable → legacy rows OK).
  const [[{ hasIdx }]] = await pool.query(
    `SELECT COUNT(*) AS hasIdx FROM information_schema.statistics
      WHERE table_schema = ? AND table_name = 'rooms' AND index_name = 'uniq_maoi_id'`,
    [CONFIG.DB.database]
  );
  if (!hasIdx) {
    await pool.query('ALTER TABLE rooms ADD UNIQUE INDEX uniq_maoi_id (maoi_id)');
  }

  await pool.query(`
    CREATE TABLE IF NOT EXISTS users (
      id INT AUTO_INCREMENT PRIMARY KEY,
      name VARCHAR(255),
      role VARCHAR(50) DEFAULT 'Staff',
      status VARCHAR(50) DEFAULT 'active'
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS credentials (
      id INT AUTO_INCREMENT PRIMARY KEY,
      room_id INT,
      user_id INT,
      type ENUM('pin','rfid','fingerprint'),
      slot_number INT,
      raw_value VARCHAR(255),
      date_from VARCHAR(50),
      date_to VARCHAR(50),
      sync_status VARCHAR(50) DEFAULT 'pending',
      FOREIGN KEY (room_id) REFERENCES rooms(id),
      FOREIGN KEY (user_id) REFERENCES users(id)
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS pending_queue (
      id INT AUTO_INCREMENT PRIMARY KEY,
      room_no VARCHAR(50),
      credential_id INT,
      action_type VARCHAR(50),
      payload_hex TEXT,
      status VARCHAR(50) DEFAULT 'queued'
    ) ENGINE=InnoDB`);

  // Door access transactions pushed by locks on hotel/locks/<mac>/log.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS lock_logs (
      id INT AUTO_INCREMENT PRIMARY KEY,
      mac VARCHAR(17),
      room_no VARCHAR(50) NULL,
      result VARCHAR(20),
      detail VARCHAR(255),
      lock_ts VARCHAR(50),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // 4. Room roster. RETIRED the auto-seed (ozkey-07 §4): the PMS (MAOI) is the
  //    source of truth for rooms and pushes them via POST /pms/rooms. The old
  //    Block-A 101..310 seed only runs if OZKEY_SEED_ROOMS=1 (pure-lab bench
  //    without a PMS).
  const [[{ cnt }]] = await pool.query('SELECT COUNT(*) AS cnt FROM rooms');
  if (cnt === 0 && process.env.OZKEY_SEED_ROOMS === '1') {
    const rows = [];
    for (let floor = 1; floor <= 3; floor++) {
      for (let door = 1; door <= 10; door++) {
        const roomNo = String(floor * 100 + door);
        rows.push(['Block A', floor, roomNo, null, 'Available']);
      }
    }
    await pool.query(
      'INSERT INTO rooms (building, floor, room_no, mac_address, status) VALUES ?',
      [rows]
    );
    logEvent('info', `OZKEY_SEED_ROOMS=1 — seeded ${rows.length} lab rooms (Block A, floors 1-3)`);
  } else {
    logEvent('info', `Room roster: ${cnt} room(s) (source of truth = PMS via POST /pms/rooms)`);
  }

  logEvent('info', `MySQL online — ${CONFIG.DB.host}/${CONFIG.DB.database}`);
}

/* ---------------------------------------------------------------------------
 * MQTT — dual handshake sync engine
 * ------------------------------------------------------------------------- */
let mqttClient = null;

function mqttPublish(topic, payload) {
  if (!mqttClient || !mqttClient.connected) {
    logEvent('warn', `MQTT offline — dropped publish to ${topic}`);
    return false;
  }
  mqttClient.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), {
    qos: 1,
  });
  return true;
}

/** Drain every queued action for a room down its command topic. */
async function flushQueueForRoom(roomNo) {
  const [queued] = await pool.query(
    "SELECT * FROM pending_queue WHERE room_no = ? AND status = 'queued' ORDER BY id ASC",
    [roomNo]
  );
  if (queued.length === 0) return 0;

  for (const job of queued) {
    const commandTopic = CONFIG.topicRoomCommand(roomNo);
    const envelope = {
      msg_id: `oz-${job.id}-${Date.now()}`,
      room_no: roomNo,
      action: job.action_type,
      credential_id: job.credential_id,
      payload_hex: job.payload_hex,
      issued_at: new Date().toISOString(),
      source: 'ozkeyserv',
    };

    const ok = mqttPublish(commandTopic, envelope);
    if (!ok) break; // broker dropped mid-flush; keep remaining jobs queued

    await pool.query("UPDATE pending_queue SET status = 'sent' WHERE id = ?", [job.id]);
    if (job.credential_id) {
      const newStatus = job.action_type === 'revoke-key' ? 'revoked' : 'synced';
      await pool.query('UPDATE credentials SET sync_status = ? WHERE id = ?', [
        newStatus,
        job.credential_id,
      ]);
    }
    logEvent(
      'sync',
      `Room ${roomNo} heartbeat -> burst ${job.action_type} #${job.id} down ${commandTopic}`
    );
  }

  // Everything drained -> room settles into Occupied (live credentials on
  // lock) or back to Available if revokes wiped the last one.
  const [[{ remaining }]] = await pool.query(
    "SELECT COUNT(*) AS remaining FROM pending_queue WHERE room_no = ? AND status = 'queued'",
    [roomNo]
  );
  if (remaining === 0) {
    const [[{ live }]] = await pool.query(
      `SELECT COUNT(*) AS live FROM credentials c
         JOIN rooms r ON r.id = c.room_id
        WHERE r.room_no = ? AND c.sync_status IN ('pending', 'synced')`,
      [roomNo]
    );
    await pool.query(
      "UPDATE rooms SET status = ? WHERE room_no = ? AND status = 'PendingUpdate'",
      [live > 0 ? 'Occupied' : 'Available', roomNo]
    );
  }
  return queued.length;
}

function initMqtt() {
  mqttClient = mqtt.connect(CONFIG.MQTT_URL, {
    clientId: `ozkeyserv-${Math.random().toString(16).slice(2, 8)}`,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });

  mqttClient.on('connect', () => {
    logEvent('info', `MQTT online — TalkPOS broker ${CONFIG.MQTT_URL}`);
    mqttClient.subscribe(
      [CONFIG.TOPIC_UNPAIRED_HEARTBEAT, CONFIG.TOPIC_ROOM_HEARTBEAT, CONFIG.TOPIC_LOCK_LOG],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.TOPIC_UNPAIRED_HEARTBEAT} + ${CONFIG.TOPIC_ROOM_HEARTBEAT} + ${CONFIG.TOPIC_LOCK_LOG}`
          );
      }
    );
  });

  mqttClient.on('reconnect', () => logEvent('warn', 'MQTT reconnecting...'));
  mqttClient.on('error', (err) => logEvent('error', `MQTT error: ${err.message}`));
  mqttClient.on('offline', () => logEvent('warn', 'MQTT broker offline'));

  mqttClient.on('message', async (topic, payloadBuf) => {
    const payload = payloadBuf.toString('utf8').trim();
    try {
      /* -- Channel 1: factory-fresh locks announcing raw MACs ------------- */
      if (topic === CONFIG.TOPIC_UNPAIRED_HEARTBEAT) {
        let mac = null;
        let rssi = null;
        let fw = null;
        // Accept either a bare MAC string or a JSON envelope {mac, rssi, fw}.
        if (payload.startsWith('{')) {
          const obj = JSON.parse(payload);
          mac = normalizeMac(obj.mac || obj.mac_address);
          rssi = obj.rssi ?? null;
          fw = obj.fw ?? obj.firmware ?? null;
        } else {
          mac = normalizeMac(payload);
        }
        if (!mac) {
          logEvent('warn', `Unpaired heartbeat with malformed MAC: "${payload.slice(0, 60)}"`);
          return;
        }
        const now = Date.now();
        const existing = unpairedCache.get(mac);
        unpairedCache.set(mac, {
          mac,
          firstSeen: existing ? existing.firstSeen : now,
          lastSeen: now,
          rssi,
          fw,
        });
        if (!existing) logEvent('pair', `Discovered unprovisioned lock ${mac} on MQTT`);
        return;
      }

      /* -- Channel 3: door access transactions pushed by the lock --------- */
      const logMatch = topic.match(/^hotel\/locks\/([^/]+)\/log$/);
      if (logMatch) {
        let obj = {};
        try {
          obj = JSON.parse(payload);
        } catch (_) {
          /* tolerate non-JSON; fields default below */
        }
        const mac = normalizeMac(obj.mac || logMatch[1]);
        if (!mac) {
          logEvent('warn', `Lock log with unusable MAC on ${topic}: "${payload.slice(0, 60)}"`);
          return;
        }
        const result = String(obj.result || 'unknown').slice(0, 20);
        const detail = String(obj.detail || '').slice(0, 255);
        const lockTs = obj.ts ? new Date(obj.ts).toISOString() : new Date().toISOString();
        // The rooms table is authoritative for MAC -> room, not the payload.
        const [[room]] = await pool.query('SELECT room_no FROM rooms WHERE mac_address = ?', [
          mac,
        ]);
        const roomNo = room ? room.room_no : null;
        await pool.query(
          'INSERT INTO lock_logs (mac, room_no, result, detail, lock_ts) VALUES (?, ?, ?, ?, ?)',
          [mac, roomNo, result, detail, lockTs]
        );
        logEvent(
          'lock',
          `Door ${result.toUpperCase()} — ${detail || 'no detail'} @ ${
            roomNo ? `room ${roomNo}` : 'unpaired lock'
          } (${mac})`
        );
        return;
      }

      /* -- Channel 2: provisioned 30s heartbeats -> flush queued actions -- */
      const match = topic.match(/^hotel\/rooms\/([^/]+)\/lock\/heartbeat$/);
      if (match) {
        const roomNo = match[1];
        const sent = await flushQueueForRoom(roomNo);
        if (sent === 0) {
          // Quiet heartbeat — nothing pending; keep the terminal readable.
          return;
        }
      }
    } catch (err) {
      logEvent('error', `MQTT message handler fault on ${topic}: ${err.message}`);
    }
  });
}

/** Periodically expire unpaired MACs that stopped broadcasting. */
function startDiscoveryPruneInterval() {
  setInterval(() => {
    const cutoff = Date.now() - CONFIG.UNPAIRED_TTL_MS;
    for (const [mac, entry] of unpairedCache) {
      if (entry.lastSeen < cutoff) {
        unpairedCache.delete(mac);
        logEvent('warn', `Unpaired lock ${mac} went silent — dropped from discovery cache`);
      }
    }
  }, 30_000);
}

/* ---------------------------------------------------------------------------
 * REST API
 * ------------------------------------------------------------------------- */
const app = express();
app.use(cors());
app.use(express.json());

const api = express.Router();
app.use('/ozkeyserv/api', api);

function guardDb(res) {
  if (!pool) {
    res.status(503).json({ ok: false, error: 'Database not ready' });
    return false;
  }
  return true;
}

/* -- Health ---------------------------------------------------------------- */
api.get('/health', (req, res) => {
  res.json({
    ok: true,
    service: 'ozkeyserv',
    db: !!pool,
    mqtt: !!(mqttClient && mqttClient.connected),
    unpaired_cached: unpairedCache.size,
    uptime_s: Math.floor(process.uptime()),
  });
});

/* -- Room matrix (dashboard grid) ------------------------------------------ */
api.get('/rooms', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT id, maoi_id, building, floor, room_no, name, room_type, capacity,
              mac_address, lock_device_id, active, status, last_synced_at
         FROM rooms ORDER BY floor, room_no`
    );
    res.json({ ok: true, rooms: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* ===========================================================================
 * PMS roster sync (ozkey-07 §4) — MAOI is the source of truth for rooms;
 * OZKEYSERV is a read-only mirror for pairing + command routing.
 * ========================================================================= */

/** ozkey-07 §4.4: enforce X-OZKEY-Secret on PMS writes when a secret is set. */
function guardPmsSecret(req, res) {
  if (!CONFIG.PMS_SECRET) return true; // lab: open when no secret configured
  if (req.get('X-OZKEY-Secret') === CONFIG.PMS_SECRET) return true;
  res.status(401).json({ ok: false, error: 'missing or invalid X-OZKEY-Secret' });
  return false;
}

/** True if a room currently has a lock bound (device or legacy MAC). */
function roomIsBound(room) {
  return !!(room.lock_device_id || room.mac_address);
}

api.post('/pms/rooms', async (req, res) => {
  if (!guardDb(res)) return;
  if (!guardPmsSecret(req, res)) return;
  try {
    const { mode = 'upsert', rooms } = req.body || {};
    if (!Array.isArray(rooms)) {
      return res.status(400).json({ ok: false, error: 'rooms[] is required' });
    }
    if (!['upsert', 'reconcile'].includes(mode)) {
      return res.status(400).json({ ok: false, error: "mode must be 'upsert' or 'reconcile'" });
    }

    const conflicts = [];
    let upserted = 0;
    const seenMaoiIds = [];

    for (const r of rooms) {
      const maoiId = r.id ? String(r.id).slice(0, 64) : null;
      const roomNo = r.room_no != null ? String(r.room_no).slice(0, 50) : null;
      if (!maoiId || !roomNo) {
        conflicts.push({ room_no: roomNo, issue: 'row skipped — missing id or room_no' });
        continue;
      }
      seenMaoiIds.push(maoiId);
      const fields = {
        room_no: roomNo,
        name: r.name != null ? String(r.name).slice(0, 255) : null,
        room_type: r.type != null ? String(r.type).slice(0, 100) : null,
        floor: Number.isFinite(r.floor) ? r.floor : null,
        capacity: Number.isFinite(r.capacity) ? r.capacity : 1,
        lock_device_id: r.lock_device_id ? String(r.lock_device_id).slice(0, 64) : null,
      };

      const [[existing]] = await pool.query('SELECT * FROM rooms WHERE maoi_id = ?', [maoiId]);
      // Guard: a different room already uses this room_no (unique) → report, skip.
      const [[clash]] = await pool.query(
        'SELECT maoi_id FROM rooms WHERE room_no = ? AND (maoi_id IS NULL OR maoi_id <> ?)',
        [roomNo, maoiId]
      );
      if (clash) {
        conflicts.push({ room_no: roomNo, issue: `room_no already used by another room — not applied` });
        continue;
      }

      if (existing) {
        await pool.query(
          `UPDATE rooms SET room_no = ?, name = ?, room_type = ?, floor = ?, capacity = ?,
             lock_device_id = COALESCE(?, lock_device_id), active = 1, last_synced_at = NOW()
           WHERE maoi_id = ?`,
          [fields.room_no, fields.name, fields.room_type, fields.floor, fields.capacity, fields.lock_device_id, maoiId]
        );
      } else {
        await pool.query(
          `INSERT INTO rooms (maoi_id, room_no, name, room_type, floor, capacity, lock_device_id,
             status, active, last_synced_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, 'Available', 1, NOW())`,
          [maoiId, fields.room_no, fields.name, fields.room_type, fields.floor, fields.capacity, fields.lock_device_id]
        );
      }
      upserted++;
    }

    // Reconcile: rooms in the mirror (that came from the PMS) but absent from the
    // full payload → deactivate NON-destructively; surface bound/live-PIN ones.
    let deactivated = 0;
    if (mode === 'reconcile') {
      const [mirror] = await pool.query('SELECT * FROM rooms WHERE maoi_id IS NOT NULL');
      for (const room of mirror) {
        if (seenMaoiIds.includes(room.maoi_id)) continue;
        // live (unexpired, non-revoked) credentials on this room?
        const [[{ live }]] = await pool.query(
          `SELECT COUNT(*) AS live FROM credentials
            WHERE room_id = ? AND sync_status IN ('pending','synced')`,
          [room.id]
        );
        if (roomIsBound(room) || live > 0) {
          conflicts.push({
            room_no: room.room_no,
            issue:
              `removed room still has ` +
              [roomIsBound(room) ? 'a bound lock' : null, live > 0 ? `${live} live credential(s)` : null]
                .filter(Boolean)
                .join(' + '),
            lock_device_id: room.lock_device_id || null,
            action: 'kept inactive — resolve at the room',
          });
        }
        await pool.query("UPDATE rooms SET active = 0, status = 'Inactive' WHERE id = ?", [room.id]);
        deactivated++;
      }
    }

    logEvent(
      'info',
      `PMS roster ${mode}: ${upserted} upserted` +
        (mode === 'reconcile' ? `, ${deactivated} deactivated` : '') +
        (conflicts.length ? `, ${conflicts.length} conflict(s)` : '')
    );
    res.json({ ok: true, upserted, deactivated, conflicts });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.get('/pms/rooms/status', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [[s]] = await pool.query(
      `SELECT MAX(last_synced_at) AS last_synced_at,
              SUM(active = 1) AS active_count,
              SUM(lock_device_id IS NOT NULL) AS bound_count,
              COUNT(*) AS room_count
         FROM rooms WHERE maoi_id IS NOT NULL`
    );
    res.json({
      ok: true,
      last_synced_at: s.last_synced_at,
      room_count: Number(s.room_count) || 0,
      active_count: Number(s.active_count) || 0,
      bound_count: Number(s.bound_count) || 0,
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Discovery: MACs on the broker not yet bound to a room ----------------- */
api.get('/locks/unpaired', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [bound] = await pool.query(
      'SELECT mac_address FROM rooms WHERE mac_address IS NOT NULL'
    );
    const boundSet = new Set(bound.map((r) => r.mac_address));
    const list = [...unpairedCache.values()]
      .filter((e) => !boundSet.has(e.mac))
      .sort((a, b) => b.lastSeen - a.lastSeen)
      .map((e) => ({
        mac_address: e.mac,
        first_seen: new Date(e.firstSeen).toISOString(),
        last_seen: new Date(e.lastSeen).toISOString(),
        rssi: e.rssi,
        fw: e.fw,
      }));
    res.json({ ok: true, unpaired: list });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Physical onboarding: bind MAC -> room --------------------------------- */
api.post('/locks/pair', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { room_no, mac_address } = req.body || {};
    const mac = normalizeMac(mac_address);
    if (!room_no || !mac) {
      return res
        .status(400)
        .json({ ok: false, error: 'room_no and a valid mac_address are required' });
    }

    const [[room]] = await pool.query('SELECT * FROM rooms WHERE room_no = ?', [room_no]);
    if (!room) return res.status(404).json({ ok: false, error: `Room ${room_no} not found` });
    if (room.mac_address && room.mac_address !== mac) {
      return res.status(409).json({
        ok: false,
        error: `Room ${room_no} is already bound to ${room.mac_address} — unpair first`,
      });
    }

    const [[collision]] = await pool.query(
      'SELECT room_no FROM rooms WHERE mac_address = ? AND room_no <> ?',
      [mac, room_no]
    );
    if (collision) {
      return res.status(409).json({
        ok: false,
        error: `MAC ${mac} is already bound to room ${collision.room_no}`,
      });
    }

    // Issue (or reuse) the broker-side network token for this room binding.
    const macToken = room.mac_token || makeMacToken();
    await pool.query(
      "UPDATE rooms SET mac_address = ?, mac_token = ?, status = 'Available' WHERE room_no = ?",
      [mac, macToken, room_no]
    );
    unpairedCache.delete(mac);

    // Gap #2 (ozkey-02 §3.2/§8.4): provision_assign handshake on the room
    // command topic — key is `mac`, NEVER `payload_hex` (that key would route
    // the JSON to the lock's Tuya parser instead of its provisioning parser).
    const commandTopic = CONFIG.topicRoomCommand(room_no);
    const handshake = {
      topic: commandTopic, // optional for LockSim since §8.4, kept for manual paste
      op: 'provision_assign',
      mac,
      room_no: String(room_no),
      server_ip: CONFIG.SERVER_IP,
      server_port: CONFIG.HTTP_PORT,
      mac_token: macToken,
      issued_by: 'OZKEYSERV/',
      issued_at: new Date().toISOString(),
    };
    mqttPublish(commandTopic, handshake);

    // Legacy MAC-scoped confirm — debug side channel only (ozkey-02 §3.2).
    mqttPublish(CONFIG.topicPairConfirm(mac), handshake);

    logEvent('pair', `PAIRED ${mac} -> room ${room_no} (provision_assign sent on ${commandTopic})`);
    res.json({ ok: true, room_no, mac_address: mac, mac_token: macToken, status: 'Available' });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Unpair (lab convenience: release a lock back to discovery) ------------ */
api.post('/locks/unpair', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { room_no } = req.body || {};
    if (!room_no) return res.status(400).json({ ok: false, error: 'room_no is required' });
    const [result] = await pool.query(
      "UPDATE rooms SET mac_address = NULL, status = 'Available' WHERE room_no = ?",
      [room_no]
    );
    if (result.affectedRows === 0)
      return res.status(404).json({ ok: false, error: `Room ${room_no} not found` });
    logEvent('pair', `UNPAIRED room ${room_no} — lock released back to discovery pool`);
    res.json({ ok: true, room_no });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- PMS bypass: direct credential injection -------------------------------- */
api.post('/pms/issue-key', async (req, res) => {
  if (!guardDb(res)) return;
  if (!guardPmsSecret(req, res)) return;
  const conn = await pool.getConnection();
  try {
    const {
      room_no,
      guest_name,
      role = 'Guest',
      type = 'pin',
      raw_value,
      slot_number = 1,
      date_from,
      date_to,
    } = req.body || {};

    if (!room_no || !guest_name || !raw_value) {
      return res
        .status(400)
        .json({ ok: false, error: 'room_no, guest_name and raw_value are required' });
    }
    if (type === 'fingerprint') {
      // Gap #6 hold (ozkey-02 §8.5): LockSim has no temp-fingerprint DPID.
      return res.status(422).json({
        ok: false,
        error: 'fingerprint credentials are on hold — the lock-side DP codec only supports pin/rfid (ozkey-02 §4)',
      });
    }
    if (!SUPPORTED_CRED_TYPES.includes(type)) {
      return res
        .status(400)
        .json({ ok: false, error: `type must be one of: ${SUPPORTED_CRED_TYPES.join(', ')}` });
    }

    const [[room]] = await conn.query('SELECT * FROM rooms WHERE room_no = ?', [room_no]);
    if (!room) return res.status(404).json({ ok: false, error: `Room ${room_no} not found` });
    if (!room.mac_address) {
      return res.status(409).json({
        ok: false,
        error: `Room ${room_no} has no paired lock — pair hardware before issuing keys`,
      });
    }

    const from = date_from || new Date().toISOString();
    const to = date_to || new Date(Date.now() + 24 * 3600 * 1000).toISOString();

    // Build the frame before opening the transaction so malformed input
    // (non-digit PIN, odd-length RFID hex) returns 400, not a rollback.
    let frame;
    try {
      frame = buildCredentialFrame({
        type,
        slotNumber: slot_number,
        rawValue: raw_value,
        dateFrom: from,
        dateTo: to,
      });
    } catch (err) {
      return res.status(400).json({ ok: false, error: err.message });
    }
    const payloadHex = toSpacedHex(frame);

    await conn.beginTransaction();

    const [userResult] = await conn.query(
      'INSERT INTO users (name, role, status) VALUES (?, ?, ?)',
      [guest_name, role, 'active']
    );
    const userId = userResult.insertId;

    const [credResult] = await conn.query(
      `INSERT INTO credentials
         (room_id, user_id, type, slot_number, raw_value, date_from, date_to, sync_status)
       VALUES (?, ?, ?, ?, ?, ?, ?, 'pending')`,
      [room.id, userId, type, slot_number, raw_value, from, to]
    );
    const credentialId = credResult.insertId;

    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (room_no, credential_id, action_type, payload_hex, status)
       VALUES (?, ?, 'issue-key', ?, 'queued')`,
      [room_no, credentialId, payloadHex]
    );

    await conn.query("UPDATE rooms SET status = 'PendingUpdate' WHERE id = ?", [room.id]);
    await conn.commit();

    logEvent(
      'key',
      `Issued ${type.toUpperCase()} for "${guest_name}" -> room ${room_no} slot ${slot_number} ` +
        `(cred #${credentialId}, queue #${queueResult.insertId}) — awaiting heartbeat`
    );

    // Opportunistic push: if the lock is chatty right now, don't wait 30s.
    flushQueueForRoom(room_no).catch(() => {});

    res.json({
      ok: true,
      credential_id: credentialId,
      queue_id: queueResult.insertId,
      user_id: userId,
      room_no,
      payload_hex: payloadHex,
      sync_status: 'pending',
    });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});

/* -- PMS bypass: credential revocation (gap #8, ozkey-02 §8.5) -------------- */
api.post('/pms/revoke-key', async (req, res) => {
  if (!guardDb(res)) return;
  if (!guardPmsSecret(req, res)) return;
  const conn = await pool.getConnection();
  try {
    const { credential_id } = req.body || {};
    if (!credential_id) {
      return res.status(400).json({ ok: false, error: 'credential_id is required' });
    }

    const [[cred]] = await conn.query(
      `SELECT c.*, r.room_no, r.mac_address, u.name AS user_name
         FROM credentials c
         JOIN rooms r ON r.id = c.room_id
         LEFT JOIN users u ON u.id = c.user_id
        WHERE c.id = ?`,
      [credential_id]
    );
    if (!cred) {
      return res.status(404).json({ ok: false, error: `Credential #${credential_id} not found` });
    }
    if (cred.sync_status === 'revoked') {
      return res
        .status(409)
        .json({ ok: false, error: `Credential #${credential_id} is already revoked` });
    }
    const [[dupe]] = await conn.query(
      `SELECT id FROM pending_queue
        WHERE credential_id = ? AND action_type = 'revoke-key' AND status = 'queued'`,
      [credential_id]
    );
    if (dupe) {
      return res.status(409).json({
        ok: false,
        error: `Credential #${credential_id} already has revoke queue #${dupe.id} pending`,
      });
    }
    if (!cred.mac_address) {
      return res.status(409).json({
        ok: false,
        error: `Room ${cred.room_no} has no paired lock — nothing to revoke against`,
      });
    }

    let frame;
    try {
      frame = buildDeleteFrame({ type: cred.type, slotNumber: cred.slot_number });
    } catch (err) {
      // fingerprint rows (pre-hold legacy) have no delete DPID — same 422 as issue.
      return res.status(422).json({ ok: false, error: err.message });
    }
    const payloadHex = toSpacedHex(frame);

    await conn.beginTransaction();

    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (room_no, credential_id, action_type, payload_hex, status)
       VALUES (?, ?, 'revoke-key', ?, 'queued')`,
      [cred.room_no, credential_id, payloadHex]
    );
    await conn.query("UPDATE credentials SET sync_status = 'revoking' WHERE id = ?", [
      credential_id,
    ]);
    await conn.query("UPDATE rooms SET status = 'PendingUpdate' WHERE room_no = ?", [
      cred.room_no,
    ]);
    await conn.commit();

    logEvent(
      'key',
      `Revoked ${cred.type.toUpperCase()} for "${cred.user_name || 'unknown'}" -> room ${cred.room_no} ` +
        `slot ${cred.slot_number} (cred #${credential_id}, queue #${queueResult.insertId}) — awaiting heartbeat`
    );

    // Opportunistic push: if the lock is chatty right now, don't wait 30s.
    flushQueueForRoom(cred.room_no).catch(() => {});

    res.json({
      ok: true,
      credential_id: Number(credential_id),
      queue_id: queueResult.insertId,
      room_no: cred.room_no,
      slot_number: cred.slot_number,
      type: cred.type,
      payload_hex: payloadHex,
      sync_status: 'revoking',
    });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});

/* -- Queue + credentials introspection -------------------------------------- */
api.get('/queue', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query('SELECT * FROM pending_queue ORDER BY id DESC LIMIT 100');
    res.json({ ok: true, queue: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.get('/locks/log', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { room_no, mac } = req.query;
    const where = [];
    const params = [];
    if (room_no) {
      where.push('room_no = ?');
      params.push(room_no);
    }
    if (mac) {
      where.push('mac = ?');
      params.push(mac);
    }
    const [rows] = await pool.query(
      `SELECT * FROM lock_logs ${where.length ? 'WHERE ' + where.join(' AND ') : ''}
        ORDER BY id DESC LIMIT 100`,
      params
    );
    res.json({ ok: true, log: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.get('/credentials', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT c.*, r.room_no, u.name AS user_name
         FROM credentials c
         LEFT JOIN rooms r ON r.id = c.room_id
         LEFT JOIN users u ON u.id = c.user_id
        ORDER BY c.id DESC LIMIT 100`
    );
    res.json({ ok: true, credentials: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Terminal event feed (dashboard polls ?after=<id>) ----------------------- */
api.get('/events', (req, res) => {
  const after = Number(req.query.after) || 0;
  res.json({ ok: true, events: eventRing.filter((e) => e.id > after) });
});

/* -- Lab simulator hooks (drive the pipeline without physical hardware) ------ */
api.post('/sim/unpaired-heartbeat', (req, res) => {
  const mac = normalizeMac((req.body || {}).mac_address);
  if (!mac) return res.status(400).json({ ok: false, error: 'valid mac_address required' });
  const now = Date.now();
  const existing = unpairedCache.get(mac);
  unpairedCache.set(mac, {
    mac,
    firstSeen: existing ? existing.firstSeen : now,
    lastSeen: now,
    rssi: -42,
    fw: 'sim',
  });
  if (!existing) logEvent('pair', `Discovered unprovisioned lock ${mac} (simulated/serial)`);
  res.json({ ok: true, mac_address: mac });
});

api.post('/sim/room-heartbeat', async (req, res) => {
  const { room_no } = req.body || {};
  if (!room_no) return res.status(400).json({ ok: false, error: 'room_no required' });
  try {
    const sent = await flushQueueForRoom(String(room_no));
    res.json({ ok: true, room_no, flushed: sent });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* ---------------------------------------------------------------------------
 * Boot sequence
 * ------------------------------------------------------------------------- */
async function boot() {
  logEvent('info', 'OZKEYSERV booting — Sovereign Smart Lock laboratory gateway');

  // MySQL with retry so the gateway survives a cold lab bench.
  let attempts = 0;
  for (;;) {
    try {
      await initDatabase();
      break;
    } catch (err) {
      attempts++;
      logEvent('error', `MySQL init failed (attempt ${attempts}): ${err.message} — retry in 5s`);
      await new Promise((r) => setTimeout(r, 5000));
    }
  }

  initMqtt();

  app.listen(CONFIG.HTTP_PORT, () => {
    logEvent('info', `HTTP gateway listening on http://localhost:${CONFIG.HTTP_PORT}/ozkeyserv/api`);
  });
}

process.on('unhandledRejection', (err) => {
  logEvent('error', `Unhandled rejection: ${err && err.message ? err.message : err}`);
});

if (require.main === module) {
  startDiscoveryPruneInterval();
  boot();
} else {
  // Required as a library (conformance tests): expose the codec, don't boot.
  module.exports = {
    CONFIG,
    TUYA_CMD,
    DPID,
    DP_TYPE,
    buildTuyaFrame,
    buildDpPayload,
    buildCredentialFrame,
    buildDeleteFrame,
    toSpacedHex,
    normalizeMac,
    makeMacToken,
  };
}
