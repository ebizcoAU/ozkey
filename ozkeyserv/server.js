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
 *    1. Bootstrap relational schema + auto-seed 100-room matrix (Block A)
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

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
const CONFIG = {
  HTTP_PORT: 3200,
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
  topicRoomCommand: (roomNo) => `hotel/rooms/${roomNo}/lock/command`,
  topicPairConfirm: (mac) => `hotel/locks/${mac.replace(/:/g, '').toLowerCase()}/pair/confirm`,
  UNPAIRED_TTL_MS: 120_000, // forget an unpaired MAC if silent for 2 minutes
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
 * Tuya 55 AA frame builder
 *
 *   [0x55][0xAA][version][command][len_hi][len_lo][ ...data... ][checksum]
 *   checksum = (sum of every preceding byte) & 0xFF
 * ------------------------------------------------------------------------- */
const TUYA_CMD = {
  CREDENTIAL_WRITE: 0x65, // lab command id: inject credential into slot
  CREDENTIAL_REVOKE: 0x66, // lab command id: clear slot
  PAIR_ACK: 0x02, // lab command id: pairing confirmation
};

const CRED_TYPE_CODE = { pin: 0x01, rfid: 0x02, fingerprint: 0x03 };

function buildTuyaFrame(command, dataBuf) {
  const head = Buffer.alloc(6);
  head[0] = 0x55;
  head[1] = 0xaa;
  head[2] = 0x00; // protocol version
  head[3] = command & 0xff;
  head.writeUInt16BE(dataBuf.length, 4);
  const body = Buffer.concat([head, dataBuf]);
  let sum = 0;
  for (const b of body) sum = (sum + b) & 0xff;
  return Buffer.concat([body, Buffer.from([sum])]);
}

function toSpacedHex(buf) {
  return buf
    .toString('hex')
    .toUpperCase()
    .match(/.{2}/g)
    .join(' ');
}

/**
 * Credential DP payload layout (lab convention):
 *   [type:1][slot:1][valueLen:1][value:N][validFrom:4 BE unix][validTo:4 BE unix]
 */
function buildCredentialFrame({ type, slotNumber, rawValue, dateFrom, dateTo }) {
  const typeCode = CRED_TYPE_CODE[type];
  const valueBuf = Buffer.from(String(rawValue), 'utf8');
  const fromTs = Math.floor(new Date(dateFrom).getTime() / 1000) || 0;
  const toTs = Math.floor(new Date(dateTo).getTime() / 1000) || 0;

  const data = Buffer.alloc(3 + valueBuf.length + 8);
  let o = 0;
  data[o++] = typeCode;
  data[o++] = slotNumber & 0xff;
  data[o++] = valueBuf.length & 0xff;
  valueBuf.copy(data, o);
  o += valueBuf.length;
  data.writeUInt32BE(fromTs >>> 0, o);
  o += 4;
  data.writeUInt32BE(toTs >>> 0, o);

  return buildTuyaFrame(TUYA_CMD.CREDENTIAL_WRITE, data);
}

function buildPairAckFrame(roomNo, mac) {
  const data = Buffer.from(`${roomNo}|${mac}`, 'utf8');
  return buildTuyaFrame(TUYA_CMD.PAIR_ACK, data);
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

  // 4. Auto-seed: Block A, floors 1-5, 20 rooms/floor => 101..120 ... 501..520.
  const [[{ cnt }]] = await pool.query('SELECT COUNT(*) AS cnt FROM rooms');
  if (cnt === 0) {
    const rows = [];
    for (let floor = 1; floor <= 5; floor++) {
      for (let door = 1; door <= 20; door++) {
        const roomNo = String(floor * 100 + door);
        rows.push(['Block A', floor, roomNo, null, 'Available']);
      }
    }
    await pool.query(
      'INSERT INTO rooms (building, floor, room_no, mac_address, status) VALUES ?',
      [rows]
    );
    logEvent('info', `Schema empty — auto-provisioned ${rows.length} rooms (Block A, floors 1-5)`);
  } else {
    logEvent('info', `Room matrix already provisioned (${cnt} rooms)`);
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
      await pool.query("UPDATE credentials SET sync_status = 'synced' WHERE id = ?", [
        job.credential_id,
      ]);
    }
    logEvent(
      'sync',
      `Room ${roomNo} heartbeat -> burst ${job.action_type} #${job.id} down ${commandTopic}`
    );
  }

  // Everything drained -> room settles into Occupied (live credentials on lock).
  const [[{ remaining }]] = await pool.query(
    "SELECT COUNT(*) AS remaining FROM pending_queue WHERE room_no = ? AND status = 'queued'",
    [roomNo]
  );
  if (remaining === 0) {
    await pool.query(
      "UPDATE rooms SET status = 'Occupied' WHERE room_no = ? AND status = 'PendingUpdate'",
      [roomNo]
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
      [CONFIG.TOPIC_UNPAIRED_HEARTBEAT, CONFIG.TOPIC_ROOM_HEARTBEAT],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.TOPIC_UNPAIRED_HEARTBEAT} + ${CONFIG.TOPIC_ROOM_HEARTBEAT}`
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
setInterval(() => {
  const cutoff = Date.now() - CONFIG.UNPAIRED_TTL_MS;
  for (const [mac, entry] of unpairedCache) {
    if (entry.lastSeen < cutoff) {
      unpairedCache.delete(mac);
      logEvent('warn', `Unpaired lock ${mac} went silent — dropped from discovery cache`);
    }
  }
}, 30_000);

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
      'SELECT id, building, floor, room_no, mac_address, status FROM rooms ORDER BY floor, room_no'
    );
    res.json({ ok: true, rooms: rows });
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

    await pool.query(
      "UPDATE rooms SET mac_address = ?, status = 'Available' WHERE room_no = ?",
      [mac, room_no]
    );
    unpairedCache.delete(mac);

    // MQTT confirmation back down to the physical device.
    const ackFrame = buildPairAckFrame(room_no, mac);
    const confirmTopic = CONFIG.topicPairConfirm(mac);
    mqttPublish(confirmTopic, {
      action: 'pair_confirm',
      room_no,
      mac_address: mac,
      command_topic: CONFIG.topicRoomCommand(room_no),
      heartbeat_topic: `hotel/rooms/${room_no}/lock/heartbeat`,
      payload_hex: toSpacedHex(ackFrame),
      issued_at: new Date().toISOString(),
    });

    logEvent('pair', `PAIRED ${mac} -> room ${room_no} (confirm sent on ${confirmTopic})`);
    res.json({ ok: true, room_no, mac_address: mac, status: 'Available' });
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
    if (!CRED_TYPE_CODE[type]) {
      return res
        .status(400)
        .json({ ok: false, error: `type must be one of: ${Object.keys(CRED_TYPE_CODE).join(', ')}` });
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

    const frame = buildCredentialFrame({
      type,
      slotNumber: slot_number,
      rawValue: raw_value,
      dateFrom: from,
      dateTo: to,
    });
    const payloadHex = toSpacedHex(frame);

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

boot();
