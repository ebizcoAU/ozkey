/*
 * ============================================================================
 *  OZLOCKSERV — OZLOCK Rendezvous Directory (lab deployment)
 *  ---------------------------------------------------------------------------
 *  Role     : Market-A personal cloud, per ozkey-05: MQTT rendezvous + mini
 *             directory that holds doorlock <-> owner-account pairing.
 *  Port     : 4200  (REST base /ozlockserv/api)
 *  Broker   : TalkPOS Mosquitto @ mqtt://10.1.1.21:1883 (lab stand-in for EMQX)
 *  Database : MySQL (localhost / ozlock)
 *
 *  Responsibilities (ozkey-04 §6, ozkey-05 §6)
 *    1. Mint single-use enrollment tokens (BANOI "Add Doorlock" begins here)
 *    2. Enroll locks: verify token, bind device -> site/owner, issue broker
 *       credentials, ack on the command topic
 *    3. Issue/revoke user keys as Tuya 55 AA DPID frames, queue them, flush
 *       on the lock's wake (ozkey/<site>/locks/<id>/heartbeat)
 *    4. Ingest door access transactions (ozkey/<site>/locks/<id>/log)
 *
 *  Lab simplifications (flagged, ozkey-05 §10 migration steps 3-5 pending):
 *    - single seeded owner + site ('lab'); REST is unauthenticated
 *    - broker credentials are minted + stored + acked for contract shape, but
 *      the lab Mosquitto does not enforce them
 *    - device_id is derived from the MAC (real hardware: keypair, ozkey-04 §3)
 * ============================================================================
 */

'use strict';

const express = require('express');
const cors = require('cors');
const mysql = require('mysql2/promise');
const mqtt = require('mqtt');
const os = require('os');
const crypto = require('crypto');

/** First non-internal IPv4 of this host. Override with OZLOCK_SERVER_IP. */
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
  HTTP_PORT: 4200,
  SERVER_IP: process.env.OZLOCK_SERVER_IP || detectLanIp(),
  SITE_ID: 'lab', // single-tenant lab deployment (ozkey-05 §1.3)
  DB: {
    host: 'localhost',
    user: 'root',
    password: 'Cableman',
    database: 'ozlock',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL: 'mqtt://10.1.1.21:1883',
  BROKER: {
    host: '10.1.1.21',
    tcp_port: 1883,
    ws_port: 9001,
    ws_path: '/mqtt',
  },
  // ozkey-04 §9 topic scheme (site-prefixed, device-scoped, room-free).
  // Site-pinned (NOT wildcard) so multiple servers can share one broker —
  // OZKEYSERV (site 'hotel', ozkey-07) publishes device-scoped on the same
  // ozkey/<site>/... root; each server must only consume its own site.
  SUB_ENROLL: 'ozkey/lab/locks/+/enroll',
  SUB_HEARTBEAT: 'ozkey/lab/locks/+/heartbeat',
  SUB_LOG: 'ozkey/lab/locks/+/log',
  topicCommand: (site, deviceId) => `ozkey/${site}/locks/${deviceId}/command`,
  ENROLL_TOKEN_TTL_MS: 10 * 60 * 1000, // ozkey-05 §7.5
  DEFAULT_HEARTBEAT_S: 60,
};

/* ---------------------------------------------------------------------------
 * In-memory state — rolling event log served to the dashboard terminal
 * ------------------------------------------------------------------------- */
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
 * Tuya 55 AA frame codec — identical to ozkeyserv (byte-verified vs LockSim)
 * ------------------------------------------------------------------------- */
const TUYA_CMD = {
  HEARTBEAT: 0x00,
  DP_REPORT: 0x06,
};

const DPID = {
  UNLOCK_CHANNEL: 1, // remote unlock request (BOOL 1) — the away-path "Mở cửa"
  ADD_TEMP_PIN: 21,
  DELETE_PIN: 22,
  ADD_TEMP_RFID: 23,
  DELETE_RFID: 24,
};

const DP_TYPE = { RAW: 0x00, BOOL: 0x01, VALUE: 0x02, STRING: 0x03, ENUM: 0x04 };

const SUPPORTED_CRED_TYPES = ['pin', 'rfid'];

function buildTuyaFrame(command, payloadBuf) {
  const head = Buffer.alloc(6);
  head[0] = 0x55;
  head[1] = 0xaa;
  head[2] = 0x00;
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

function credentialValueBytes(type, rawValue) {
  const value = String(rawValue).trim();
  if (type === 'pin') {
    if (!/^\d+$/.test(value)) {
      throw new Error(`PIN must be digits only (got "${value}")`);
    }
    return Buffer.from(value, 'ascii');
  }
  const hex = value.replace(/[^0-9a-fA-F]/g, '');
  if (hex.length === 0 || hex.length % 2 !== 0) {
    throw new Error(`RFID UID must be an even-length hex string (got "${value}")`);
  }
  return Buffer.from(hex, 'hex');
}

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

function buildDeleteFrame({ type, slotNumber }) {
  if (!SUPPORTED_CRED_TYPES.includes(type)) {
    throw new Error(`unsupported credential type "${type}" for the DP codec`);
  }
  const value = Buffer.alloc(2);
  value.writeUInt16BE(slotNumber & 0xffff, 0);
  const dpId = type === 'pin' ? DPID.DELETE_PIN : DPID.DELETE_RFID;
  return buildTuyaFrame(TUYA_CMD.DP_REPORT, buildDpPayload(dpId, DP_TYPE.RAW, value));
}

/** Remote unlock request: DP_REPORT / DPID 1 (UNLOCK_CHANNEL) BOOL value 1.
 *  Byte-matches LockSim's SAMPLE_REMOTE_UNLOCK_FRAME; the lock's handleFrame
 *  runs unlockCycle() on receipt. */
function buildUnlockFrame() {
  return buildTuyaFrame(
    TUYA_CMD.DP_REPORT,
    buildDpPayload(DPID.UNLOCK_CHANNEL, DP_TYPE.BOOL, Buffer.from([0x01]))
  );
}

/* ---------------------------------------------------------------------------
 * Identity helpers
 * ------------------------------------------------------------------------- */
function normalizeMac(raw) {
  const hex = String(raw || '')
    .replace(/[^0-9a-fA-F]/g, '')
    .toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(':');
}

/** Lab interim device id (ozkey-04 §3: real hardware derives from a keypair). */
function deviceIdFromMac(mac) {
  return `ozk-${mac.replace(/:/g, '').toLowerCase()}`;
}

function makeSecret(bytes = 16, prefix = '') {
  return prefix + crypto.randomBytes(bytes).toString('hex');
}

/* ---------------------------------------------------------------------------
 * MySQL bootstrap — owner/site/lock schema (rooms-free, ozkey-05 §3)
 * ------------------------------------------------------------------------- */
let pool = null;

async function initDatabase() {
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

  pool = mysql.createPool(CONFIG.DB);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS owners (
      id INT AUTO_INCREMENT PRIMARY KEY,
      display_name VARCHAR(255),
      banoi_sub VARCHAR(255) NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS sites (
      id VARCHAR(50) PRIMARY KEY,
      owner_id INT,
      label VARCHAR(255),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (owner_id) REFERENCES owners(id)
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS locks (
      id VARCHAR(64) PRIMARY KEY,
      site_id VARCHAR(50),
      app_id VARCHAR(80) NULL,
      mac VARCHAR(17),
      label VARCHAR(255) DEFAULT 'New Doorlock',
      fw VARCHAR(50) NULL,
      status VARCHAR(20) DEFAULT 'enrolled',
      power_profile VARCHAR(20) DEFAULT 'eco',
      heartbeat_s INT DEFAULT 60,
      broker_username VARCHAR(64),
      broker_secret VARCHAR(64),
      last_seen_at DATETIME NULL,
      enrolled_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (site_id) REFERENCES sites(id)
    ) ENGINE=InnoDB`);

  // app_id = the paired app's self-generated identity (trust-model v2, ozkey-05
  // amendment / XF-42 §13). Additive migration for pre-v2 lab rows.
  const [[{ hasAppId }]] = await pool.query(
    `SELECT COUNT(*) AS hasAppId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'app_id'`,
    [CONFIG.DB.database]
  );
  if (!hasAppId) await pool.query('ALTER TABLE locks ADD COLUMN app_id VARCHAR(80) NULL AFTER site_id');

  const [[{ hasTokenAppId }]] = await pool.query(
    `SELECT COUNT(*) AS hasTokenAppId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'enroll_tokens' AND column_name = 'app_id'`,
    [CONFIG.DB.database]
  );
  if (!hasTokenAppId)
    await pool.query('ALTER TABLE enroll_tokens ADD COLUMN app_id VARCHAR(80) NULL AFTER owner_id');

  await pool.query(`
    CREATE TABLE IF NOT EXISTS enroll_tokens (
      token VARCHAR(64) PRIMARY KEY,
      site_id VARCHAR(50),
      owner_id INT,
      app_id VARCHAR(80) NULL,
      label VARCHAR(255) NULL,
      expires_at DATETIME,
      used_at DATETIME NULL,
      device_id VARCHAR(64) NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS grants (
      id INT AUTO_INCREMENT PRIMARY KEY,
      device_id VARCHAR(64),
      site_id VARCHAR(50),
      user_name VARCHAR(255),
      type ENUM('pin','rfid','fingerprint'),
      slot_number INT,
      raw_value VARCHAR(255),
      date_from VARCHAR(50),
      date_to VARCHAR(50),
      sync_status VARCHAR(50) DEFAULT 'pending',
      issued_by VARCHAR(50) DEFAULT 'owner',
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS pending_queue (
      id INT AUTO_INCREMENT PRIMARY KEY,
      device_id VARCHAR(64),
      site_id VARCHAR(50),
      grant_id INT NULL,
      action_type VARCHAR(50),
      payload_hex TEXT,
      status VARCHAR(50) DEFAULT 'queued',
      expires_at DATETIME NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS lock_logs (
      id INT AUTO_INCREMENT PRIMARY KEY,
      device_id VARCHAR(64),
      site_id VARCHAR(50),
      mac VARCHAR(17),
      result VARCHAR(20),
      detail VARCHAR(255),
      lock_ts VARCHAR(50),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // App-attributed control-plane audit trail: every action an app performs
  // through OZLOCK (register pairing, grant/revoke a key, remote unlock,
  // settings). Distinct from lock_logs (physical door events at the lock).
  await pool.query(`
    CREATE TABLE IF NOT EXISTS audit_log (
      id INT AUTO_INCREMENT PRIMARY KEY,
      app_id VARCHAR(80) NULL,
      device_id VARCHAR(64) NULL,
      site_id VARCHAR(50),
      action VARCHAR(30),
      detail VARCHAR(255),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // Seed the single lab owner + site (ozkey-05 lab simplification).
  const [[{ ownerCnt }]] = await pool.query('SELECT COUNT(*) AS ownerCnt FROM owners');
  if (ownerCnt === 0) {
    const [r] = await pool.query(
      "INSERT INTO owners (display_name, banoi_sub) VALUES ('BANOI Lab Owner', 'banoi:usr_lab')"
    );
    await pool.query('INSERT INTO sites (id, owner_id, label) VALUES (?, ?, ?)', [
      CONFIG.SITE_ID,
      r.insertId,
      'Lab Home',
    ]);
    logEvent('info', `Seeded owner "BANOI Lab Owner" + site '${CONFIG.SITE_ID}'`);
  }

  logEvent('info', `MySQL online — ${CONFIG.DB.host}/${CONFIG.DB.database}`);
}

/* ---------------------------------------------------------------------------
 * MQTT — rendezvous engine (device-scoped topics, ozkey-04 §9)
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

/** Drain queued actions for a device; expired unlock-style rows are skipped. */
async function flushQueueForDevice(siteId, deviceId) {
  const [queued] = await pool.query(
    "SELECT * FROM pending_queue WHERE device_id = ? AND status = 'queued' ORDER BY id ASC",
    [deviceId]
  );
  if (queued.length === 0) return 0;

  let sent = 0;
  for (const job of queued) {
    // ozkey-05 §6.3: commands must never fire stale.
    if (job.expires_at && new Date(job.expires_at).getTime() < Date.now()) {
      await pool.query("UPDATE pending_queue SET status = 'expired' WHERE id = ?", [job.id]);
      logEvent('warn', `Queue #${job.id} (${job.action_type}) expired before ${deviceId} woke`);
      continue;
    }

    const commandTopic = CONFIG.topicCommand(siteId, deviceId);
    const envelope = {
      msg_id: `ozl-${job.id}-${Date.now()}`,
      device_id: deviceId,
      action: job.action_type,
      grant_id: job.grant_id,
      payload_hex: job.payload_hex,
      issued_at: new Date().toISOString(),
      source: 'ozlockserv',
    };

    const ok = mqttPublish(commandTopic, envelope);
    if (!ok) break;

    await pool.query("UPDATE pending_queue SET status = 'sent' WHERE id = ?", [job.id]);
    if (job.grant_id) {
      const newStatus = job.action_type === 'revoke-key' ? 'revoked' : 'synced';
      await pool.query('UPDATE grants SET sync_status = ? WHERE id = ?', [
        newStatus,
        job.grant_id,
      ]);
    }
    sent++;
    logEvent('sync', `${deviceId} wake -> burst ${job.action_type} #${job.id} down ${commandTopic}`);
  }
  return sent;
}

function initMqtt() {
  mqttClient = mqtt.connect(CONFIG.MQTT_URL, {
    clientId: `ozlockserv-${Math.random().toString(16).slice(2, 8)}`,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });

  mqttClient.on('connect', () => {
    logEvent('info', `MQTT online — broker ${CONFIG.MQTT_URL}`);
    mqttClient.subscribe(
      [CONFIG.SUB_ENROLL, CONFIG.SUB_HEARTBEAT, CONFIG.SUB_LOG],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.SUB_ENROLL} + ${CONFIG.SUB_HEARTBEAT} + ${CONFIG.SUB_LOG}`
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
      const m = topic.match(/^ozkey\/([^/]+)\/locks\/([^/]+)\/(enroll|heartbeat|log)$/);
      if (!m) return;
      const [, siteId, topicDeviceId, kind] = m;

      let obj = {};
      try {
        obj = JSON.parse(payload);
      } catch (_) {
        logEvent('warn', `Non-JSON payload on ${topic}: "${payload.slice(0, 60)}"`);
        return;
      }

      /* -- Enrollment: verify token, bind device -> site/owner ------------- */
      if (kind === 'enroll') {
        await handleEnroll(siteId, topicDeviceId, obj);
        return;
      }

      const deviceId = String(obj.device_id || topicDeviceId);

      /* -- Wake heartbeat: update presence, flush the queue ----------------- */
      if (kind === 'heartbeat') {
        await pool.query('UPDATE locks SET last_seen_at = NOW() WHERE id = ?', [deviceId]);
        const sent = await flushQueueForDevice(siteId, deviceId);
        if (sent > 0) return; // flush already logged
        return; // quiet heartbeat
      }

      /* -- Door access transaction ------------------------------------------ */
      if (kind === 'log') {
        const mac = normalizeMac(obj.mac) || null;
        const result = String(obj.result || 'unknown').slice(0, 20);
        const detail = String(obj.detail || '').slice(0, 255);
        const lockTs = obj.ts ? new Date(obj.ts).toISOString() : new Date().toISOString();
        await pool.query(
          'INSERT INTO lock_logs (device_id, site_id, mac, result, detail, lock_ts) VALUES (?, ?, ?, ?, ?, ?)',
          [deviceId, siteId, mac, result, detail, lockTs]
        );
        const [[lk]] = await pool.query('SELECT label FROM locks WHERE id = ?', [deviceId]);
        logEvent(
          'lock',
          `Door ${result.toUpperCase()} — ${detail || 'no detail'} @ "${lk ? lk.label : deviceId}"`
        );
      }
    } catch (err) {
      logEvent('error', `MQTT message handler fault on ${topic}: ${err.message}`);
    }
  });
}

/**
 * Lock's first broker contact (XF-42 §13.2 step 3). Token-free: the device_id
 * announced here must match a pairing the app already registered (§13.2 step 2).
 * Knowing the random device_id IS the bearer proof — no server credential. The
 * server binds the lock's MAC + broker creds to the pre-registered pairing.
 */
async function handleEnroll(siteId, topicDeviceId, obj) {
  const mac = normalizeMac(obj.mac);
  const deviceId = String(obj.device_id || topicDeviceId);

  if (!mac) {
    logEvent('warn', `Enroll from ${deviceId} missing mac — ignored`);
    return;
  }

  const [[row]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
  const fail = async (reason) => {
    logEvent('error', `ENROLL REJECTED for ${deviceId} (${mac}) — ${reason}`);
    mqttPublish(CONFIG.topicCommand(siteId, deviceId), {
      op: 'enrollment_nack',
      device_id: deviceId,
      error: reason,
    });
  };

  // No pre-registered pairing → the app must POST /pairings first (§13.2).
  if (!row) return fail('no pairing registered for this device_id — app must register it first');

  const brokerUsername = deviceId;
  const brokerSecret = makeSecret(16, 'ozl_');
  const label = row.label && row.label !== 'New Doorlock' ? row.label : `Doorlock ${mac.slice(-5)}`;
  const appId = row.app_id || (obj.app_id ? String(obj.app_id).slice(0, 80) : null);

  await pool.query(
    `UPDATE locks SET app_id = ?, mac = ?, label = ?, fw = ?, status = 'enrolled',
       heartbeat_s = COALESCE(heartbeat_s, ?), broker_username = ?, broker_secret = ?, last_seen_at = NOW()
     WHERE id = ?`,
    [appId, mac, label, obj.fw || null, CONFIG.DEFAULT_HEARTBEAT_S, brokerUsername, brokerSecret, deviceId]
  );

  // v1 plaintext ack — bench only; production wraps this in the ozkey-04 §8
  // ECDH session and the broker enforces the credential.
  mqttPublish(CONFIG.topicCommand(siteId, deviceId), {
    op: 'enrollment_ack',
    device_id: deviceId,
    site_id: siteId,
    app_id: appId,
    label,
    broker_username: brokerUsername,
    broker_secret: brokerSecret,
    heartbeat_s: CONFIG.DEFAULT_HEARTBEAT_S,
    issued_by: 'OZLOCK/',
  });

  logEvent('pair', `ENROLLED ${deviceId} (${mac}) -> site '${siteId}' as "${label}", paired to app ${appId || '(anon)'}`);
}

/* ---------------------------------------------------------------------------
 * REST API
 * ------------------------------------------------------------------------- */
const app = express();
app.use(cors());
app.use(express.json());

const api = express.Router();
// Mount under both the process name and the service brand so either base path
// works (LockSim's health probe / the keyring app may use either).
app.use('/ozlockserv/api', api);
app.use('/ozlock/api', api);

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
    service: 'ozlockserv',
    site: CONFIG.SITE_ID,
    db: !!pool,
    mqtt: !!(mqttClient && mqttClient.connected),
    uptime_s: Math.floor(process.uptime()),
  });
});

/** ozkey-04 §5 provision payload — the app writes it over BLE; lab pastes it
 *  into LockSim (transport equivalence, ozkey-04 §2.2). No enrollment_token:
 *  under trust-model v2 the device_id IS the bearer rendezvous handle. */
function buildProvisionPayload(appId, deviceId) {
  return {
    v: 1,
    mode: 'ozkey-cloud',
    ssid: 'OZKEY-LAB',
    password: 'labwifi-secret',
    broker_host: CONFIG.BROKER.host,
    broker_tcp_port: CONFIG.BROKER.tcp_port,
    broker_ws_port: CONFIG.BROKER.ws_port,
    broker_ws_path: CONFIG.BROKER.ws_path,
    server_ip: CONFIG.SERVER_IP,
    server_port: CONFIG.HTTP_PORT,
    site_id: CONFIG.SITE_ID,
    app_id: appId,
    device_id: deviceId,
    heartbeat_s: CONFIG.DEFAULT_HEARTBEAT_S,
  };
}

/**
 * Register an app⇄device pairing (XF-42 §13.2). OZLOCK authenticates neither
 * party: the app self-generated app_id and granted the lock its device_id at
 * the BLE ceremony, and now records the bond here. First-writer-wins on the
 * random device_id — re-registering by the SAME app is idempotent; a different
 * app claiming a live device_id is refused (squatting guard). The device_id's
 * unguessability + the (future) e2e envelope are the security, not a server
 * credential. Returns {status:number}|null via `err` for the caller to map.
 */
async function registerPairing(appId, deviceId, label) {
  const [[existing]] = await pool.query('SELECT app_id, status FROM locks WHERE id = ?', [
    deviceId,
  ]);
  if (existing && existing.app_id && appId && existing.app_id !== appId) {
    const e = new Error(`device_id already paired to a different app`);
    e.httpStatus = 409;
    throw e;
  }
  // Fresh registration lands as 'registered' (awaiting the lock's first
  // contact); an already-enrolled lock keeps its status on re-register.
  await pool.query(
    `INSERT INTO locks (id, site_id, app_id, label, status, heartbeat_s)
       VALUES (?, ?, ?, ?, 'registered', ?)
     ON DUPLICATE KEY UPDATE app_id = VALUES(app_id),
       label = COALESCE(VALUES(label), label)`,
    [deviceId, CONFIG.SITE_ID, appId, label || 'New Doorlock', CONFIG.DEFAULT_HEARTBEAT_S]
  );
  logEvent(
    'pair',
    `Pairing registered: app ${appId || '(anon)'} ⇄ device ${deviceId} — awaiting doorlock contact`
  );
  await recordAudit(appId, deviceId, 'pair', `registered pairing (label "${label || 'New Doorlock'}")`);
}

/* -- Pairing registration (trust-model v2, XF-42 §13.2) ---------------------- */
api.post('/pairings', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { app_id, device_id, label } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    const deviceId = device_id ? String(device_id).slice(0, 64) : null;
    if (!appId || !deviceId) {
      return res
        .status(400)
        .json({ ok: false, error: 'app_id and device_id are required (the app grants both)' });
    }
    await registerPairing(appId, deviceId, label);
    res.json({
      ok: true,
      device_id: deviceId,
      app_id: appId,
      provision_payload: buildProvisionPayload(appId, deviceId),
    });
  } catch (err) {
    res.status(err.httpStatus || 500).json({ ok: false, error: err.message });
  }
});

api.get('/pairings/status', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = String(req.query.device_id || '');
    const [[row]] = await pool.query('SELECT id, app_id, status, mac FROM locks WHERE id = ?', [
      deviceId,
    ]);
    if (!row) return res.status(404).json({ ok: false, error: 'no such pairing' });
    res.json({
      ok: true,
      device_id: row.id,
      app_id: row.app_id,
      // 'registered' = awaiting lock; 'enrolled' = lock made first contact.
      status: row.status,
      mac: row.mac,
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- DEPRECATED: /enroll/begin — token-variant shim over registerPairing ----- */
api.post('/enroll/begin', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { label, app_id, device_id } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    const deviceId = device_id ? String(device_id).slice(0, 64) : makeSecret(16, 'ozl-');
    await registerPairing(appId, deviceId, label);
    logEvent('warn', `/enroll/begin is deprecated — use POST /pairings (device ${deviceId})`);
    res.json({
      ok: true,
      // `token` retained only as a correlation handle for legacy callers.
      token: deviceId,
      device_id: deviceId,
      app_id: appId,
      provision_payload: buildProvisionPayload(appId, deviceId),
    });
  } catch (err) {
    res.status(err.httpStatus || 500).json({ ok: false, error: err.message });
  }
});

/* -- Locks (the owner's fleet) ----------------------------------------------- */
api.get('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT id, site_id, app_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at
         FROM locks ORDER BY enrolled_at DESC`
    );
    res.json({ ok: true, locks: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* ===========================================================================
 * Registry / observability lookups (OZLOCK console — no actions, ozkey-05)
 * ========================================================================= */

/** Enumerate the apps (users) OZLOCK knows, with how many locks each holds. */
api.get('/apps', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT app_id,
              COUNT(*) AS lock_count,
              SUM(status = 'enrolled') AS enrolled_count,
              MAX(last_seen_at) AS last_seen_at
         FROM locks
        WHERE app_id IS NOT NULL
        GROUP BY app_id
        ORDER BY lock_count DESC`
    );
    res.json({ ok: true, apps: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given an app (user id) → all its doorlocks. */
api.get('/apps/:appId/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT id, app_id, mac, label, status, power_profile, heartbeat_s, last_seen_at, enrolled_at
         FROM locks WHERE app_id = ? ORDER BY enrolled_at DESC`,
      [req.params.appId]
    );
    res.json({ ok: true, app_id: req.params.appId, locks: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given an app (user id) → its control-plane activity (grant/revoke/unlock…). */
api.get('/apps/:appId/activity', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { where, params } = rangeWhere('app_id = ?', [req.params.appId], req.query);
    const { limit, offset } = pageParams(req.query);
    const [[{ total }]] = await pool.query(
      `SELECT COUNT(*) AS total FROM audit_log WHERE ${where}`,
      params
    );
    const [rows] = await pool.query(
      `SELECT * FROM audit_log WHERE ${where} ORDER BY id DESC LIMIT ? OFFSET ?`,
      [...params, limit, offset]
    );
    res.json({ ok: true, app_id: req.params.appId, activity: rows, total, limit, offset });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given a doorlock (device id) → the app it's bound to + its record. */
api.get('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [[lock]] = await pool.query(
      `SELECT id, app_id, site_id, mac, label, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at
         FROM locks WHERE id = ?`,
      [req.params.id]
    );
    if (!lock) return res.status(404).json({ ok: false, error: `Lock ${req.params.id} not found` });
    res.json({ ok: true, lock });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.patch('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { label, power_profile, heartbeat_s } = req.body || {};
    const sets = [];
    const params = [];
    if (label !== undefined) {
      sets.push('label = ?');
      params.push(String(label).slice(0, 255));
    }
    if (power_profile !== undefined) {
      if (!['eco', 'responsive', 'scheduled'].includes(power_profile))
        return res.status(400).json({ ok: false, error: 'invalid power_profile' });
      sets.push('power_profile = ?');
      params.push(power_profile);
    }
    if (heartbeat_s !== undefined) {
      sets.push('heartbeat_s = ?');
      params.push(Math.max(5, Number(heartbeat_s) || CONFIG.DEFAULT_HEARTBEAT_S));
    }
    if (!sets.length) return res.status(400).json({ ok: false, error: 'nothing to update' });
    params.push(req.params.id);
    const [r] = await pool.query(`UPDATE locks SET ${sets.join(', ')} WHERE id = ?`, params);
    if (r.affectedRows === 0)
      return res.status(404).json({ ok: false, error: `Lock ${req.params.id} not found` });
    logEvent('info', `Lock ${req.params.id} settings updated (${sets.join(', ')})`);
    res.json({ ok: true, id: req.params.id });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Clear the fleet (start fresh) ------------------------------------------- *
 * No FK cascade from locks → grants/pending_queue/lock_logs (they only carry a
 * device_id column), so wipe the dependents explicitly. DELETE /locks clears
 * every lock for the site; DELETE /locks/:id removes one. Lab/dev convenience.
 */
async function purgeLockRows(conn, where, args) {
  await conn.query(`DELETE FROM grants WHERE ${where}`, args);
  await conn.query(`DELETE FROM pending_queue WHERE ${where}`, args);
  await conn.query(`DELETE FROM lock_logs WHERE ${where}`, args);
  await conn.query(`DELETE FROM audit_log WHERE ${where}`, args);
}

/** Parse pagination query (?limit=&offset=), clamped; default 12 rows/page. */
function pageParams(query) {
  const limit = Math.min(200, Math.max(1, Number(query.limit) || 12));
  const offset = Math.max(0, Number(query.offset) || 0);
  return { limit, offset };
}

/**
 * Append an optional inclusive date range (?from=YYYY-MM-DD&to=YYYY-MM-DD) on
 * created_at to a base WHERE. `to` is inclusive (matches through end-of-day).
 * Returns { where, params } for interpolation (base clause is caller-literal).
 */
function rangeWhere(baseClause, baseParams, query) {
  const clauses = [baseClause];
  const params = [...baseParams];
  // Compare whole calendar dates (both ends inclusive) so a from=to=today
  // range matches regardless of time-of-day / UTC-vs-local boundary skew.
  if (query.from) {
    clauses.push('DATE(created_at) >= ?');
    params.push(String(query.from));
  }
  if (query.to) {
    clauses.push('DATE(created_at) <= ?');
    params.push(String(query.to));
  }
  return { where: clauses.join(' AND '), params };
}

/**
 * Record a control-plane action in the app-attributed audit trail. If appId is
 * omitted it's resolved from the device's current pairing. Best-effort — never
 * throws into the caller (an audit failure must not fail the real action).
 */
async function recordAudit(appId, deviceId, action, detail) {
  try {
    let aid = appId;
    if (!aid && deviceId) {
      const [[l]] = await pool.query('SELECT app_id FROM locks WHERE id = ?', [deviceId]);
      aid = l ? l.app_id : null;
    }
    await pool.query(
      'INSERT INTO audit_log (app_id, device_id, site_id, action, detail) VALUES (?, ?, ?, ?, ?)',
      [aid || null, deviceId || null, CONFIG.SITE_ID, action, String(detail || '').slice(0, 255)]
    );
  } catch (err) {
    logEvent('warn', `audit_log write failed (${action}): ${err.message}`);
  }
}

api.delete('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    await conn.beginTransaction();
    await purgeLockRows(conn, 'site_id = ?', [CONFIG.SITE_ID]);
    const [d] = await conn.query('DELETE FROM locks WHERE site_id = ?', [CONFIG.SITE_ID]);
    await conn.commit();
    logEvent('warn', `Fleet cleared — ${d.affectedRows} doorlock(s) removed (start fresh)`);
    res.json({ ok: true, removed: d.affectedRows });
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

api.delete('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const id = req.params.id;
    await conn.beginTransaction();
    await purgeLockRows(conn, 'device_id = ?', [id]);
    const [d] = await conn.query('DELETE FROM locks WHERE id = ?', [id]);
    await conn.commit();
    if (d.affectedRows === 0)
      return res.status(404).json({ ok: false, error: `Lock ${id} not found` });
    logEvent('info', `Doorlock ${id} removed`);
    res.json({ ok: true, id });
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

/* -- Grants: issue / list / revoke user keys --------------------------------- */
api.post('/locks/:id/grants', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const deviceId = req.params.id;
    const {
      user_name,
      type = 'pin',
      raw_value,
      slot_number = 1,
      date_from,
      date_to,
    } = req.body || {};

    if (!user_name || !raw_value) {
      return res.status(400).json({ ok: false, error: 'user_name and raw_value are required' });
    }
    if (type === 'fingerprint') {
      return res.status(422).json({
        ok: false,
        error: 'fingerprint credentials are on hold — DP codec supports pin/rfid only',
      });
    }
    if (!SUPPORTED_CRED_TYPES.includes(type)) {
      return res
        .status(400)
        .json({ ok: false, error: `type must be one of: ${SUPPORTED_CRED_TYPES.join(', ')}` });
    }

    const [[lock]] = await conn.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock) return res.status(404).json({ ok: false, error: `Lock ${deviceId} not found` });

    const from = date_from || new Date().toISOString();
    const to = date_to || new Date(Date.now() + 24 * 3600 * 1000).toISOString();

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
    const [grantResult] = await conn.query(
      `INSERT INTO grants (device_id, site_id, user_name, type, slot_number, raw_value, date_from, date_to, sync_status)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending')`,
      [deviceId, lock.site_id, user_name, type, slot_number, raw_value, from, to]
    );
    const grantId = grantResult.insertId;
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, payload_hex, status)
       VALUES (?, ?, ?, 'grant-key', ?, 'queued')`,
      [deviceId, lock.site_id, grantId, payloadHex]
    );
    await conn.commit();

    logEvent(
      'key',
      `Granted ${type.toUpperCase()} to "${user_name}" -> "${lock.label}" slot ${slot_number} ` +
        `(grant #${grantId}, queue #${queueResult.insertId}) — awaiting wake`
    );
    await recordAudit(
      lock.app_id,
      deviceId,
      'grant',
      `grant ${type.toUpperCase()} slot ${slot_number} to "${user_name}" (grant #${grantId})`
    );

    flushQueueForDevice(lock.site_id, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
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

api.get('/locks/:id/grants', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      'SELECT * FROM grants WHERE device_id = ? ORDER BY id DESC LIMIT 100',
      [req.params.id]
    );
    res.json({ ok: true, grants: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Remote unlock (the away-path "Mở cửa", ozkey-05 §6.3) -------------------- */
api.post('/locks/:id/unlock', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock) return res.status(404).json({ ok: false, error: `Lock ${deviceId} not found` });
    if (lock.status !== 'enrolled') {
      return res
        .status(409)
        .json({ ok: false, error: `Lock ${deviceId} is not enrolled yet (status: ${lock.status})` });
    }

    // §6.3: a remote unlock MUST NOT fire stale. Queue with a 60 s expiry; the
    // flush drops it if the lock doesn't wake in time.
    const payloadHex = toSpacedHex(buildUnlockFrame());
    const expiresAt = new Date(Date.now() + 60_000);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, payload_hex, status, expires_at)
       VALUES (?, ?, NULL, 'unlock', ?, 'queued', ?)`,
      [deviceId, lock.site_id, payloadHex, expiresAt]
    );

    logEvent('key', `Remote UNLOCK queued for "${lock.label}" (queue #${queueResult.insertId}, expires 60s)`);
    await recordAudit(lock.app_id, deviceId, 'unlock', `remote unlock "${lock.label}"`);
    const sent = await flushQueueForDevice(lock.site_id, deviceId);

    res.json({
      ok: true,
      device_id: deviceId,
      queue_id: queueResult.insertId,
      payload_hex: payloadHex,
      // In the lab LockSim keeps its MQTT link open, so delivery is immediate;
      // a real eco lock would report 'queued' until its next wake.
      delivery: sent > 0 ? 'delivered' : 'queued',
      expires_at: expiresAt.toISOString(),
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.delete('/locks/:id/grants/:gid', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const deviceId = req.params.id;
    const grantId = Number(req.params.gid);

    const [[grant]] = await conn.query('SELECT * FROM grants WHERE id = ? AND device_id = ?', [
      grantId,
      deviceId,
    ]);
    if (!grant)
      return res.status(404).json({ ok: false, error: `Grant #${grantId} not found on ${deviceId}` });
    if (grant.sync_status === 'revoked')
      return res.status(409).json({ ok: false, error: `Grant #${grantId} is already revoked` });
    const [[dupe]] = await conn.query(
      `SELECT id FROM pending_queue
        WHERE grant_id = ? AND action_type = 'revoke-key' AND status = 'queued'`,
      [grantId]
    );
    if (dupe)
      return res.status(409).json({
        ok: false,
        error: `Grant #${grantId} already has revoke queue #${dupe.id} pending`,
      });

    let frame;
    try {
      frame = buildDeleteFrame({ type: grant.type, slotNumber: grant.slot_number });
    } catch (err) {
      return res.status(422).json({ ok: false, error: err.message });
    }
    const payloadHex = toSpacedHex(frame);

    await conn.beginTransaction();
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, payload_hex, status)
       VALUES (?, ?, ?, 'revoke-key', ?, 'queued')`,
      [deviceId, grant.site_id, grantId, payloadHex]
    );
    await conn.query("UPDATE grants SET sync_status = 'revoking' WHERE id = ?", [grantId]);
    await conn.commit();

    logEvent(
      'key',
      `Revoking ${grant.type.toUpperCase()} for "${grant.user_name}" on ${deviceId} slot ${grant.slot_number} ` +
        `(grant #${grantId}, queue #${queueResult.insertId}) — awaiting wake`
    );
    await recordAudit(
      null,
      deviceId,
      'revoke',
      `revoke ${grant.type.toUpperCase()} slot ${grant.slot_number} for "${grant.user_name}" (grant #${grantId})`
    );

    flushQueueForDevice(grant.site_id, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
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

/* -- Door transaction log (paginated + optional date range) ------------------- */
api.get('/locks/:id/log', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { where, params } = rangeWhere('device_id = ?', [req.params.id], req.query);
    const { limit, offset } = pageParams(req.query);
    const [[{ total }]] = await pool.query(
      `SELECT COUNT(*) AS total FROM lock_logs WHERE ${where}`,
      params
    );
    const [rows] = await pool.query(
      `SELECT * FROM lock_logs WHERE ${where} ORDER BY id DESC LIMIT ? OFFSET ?`,
      [...params, limit, offset]
    );
    res.json({ ok: true, log: rows, total, limit, offset });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Introspection + terminal feed --------------------------------------------- */
api.get('/queue', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query('SELECT * FROM pending_queue ORDER BY id DESC LIMIT 100');
    res.json({ ok: true, queue: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.get('/events', (req, res) => {
  const after = Number(req.query.after) || 0;
  res.json({ ok: true, events: eventRing.filter((e) => e.id > after) });
});

/* -- Lab simulator hook: fake a wake without the broker ------------------------ */
api.post('/sim/heartbeat', async (req, res) => {
  if (!guardDb(res)) return;
  const { device_id } = req.body || {};
  if (!device_id) return res.status(400).json({ ok: false, error: 'device_id required' });
  try {
    const [[lock]] = await pool.query('SELECT site_id FROM locks WHERE id = ?', [device_id]);
    if (!lock) return res.status(404).json({ ok: false, error: `Lock ${device_id} not found` });
    await pool.query('UPDATE locks SET last_seen_at = NOW() WHERE id = ?', [device_id]);
    const sent = await flushQueueForDevice(lock.site_id, String(device_id));
    res.json({ ok: true, device_id, flushed: sent });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* ---------------------------------------------------------------------------
 * Boot sequence
 * ------------------------------------------------------------------------- */
async function boot() {
  logEvent('info', 'OZLOCKSERV booting — personal-cloud rendezvous directory (lab)');

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
    logEvent('info', `HTTP directory listening on http://localhost:${CONFIG.HTTP_PORT}/ozlockserv/api`);
  });
}

process.on('unhandledRejection', (err) => {
  logEvent('error', `Unhandled rejection: ${err && err.message ? err.message : err}`);
});

if (require.main === module) {
  boot();
} else {
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
    deviceIdFromMac,
  };
}
