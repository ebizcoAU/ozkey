/*
 * ============================================================================
 *  OZLOCKSERV — OZLOCK Rendezvous Directory (lab deployment)
 *  ---------------------------------------------------------------------------
 *  Role     : Market-A personal cloud, per ozkey-05: MQTT rendezvous + mini
 *             directory that holds doorlock <-> owner-account pairing.
 *  Port     : 4200  (REST base /ozlockserv/api)
 *  Broker   : TalkPOS Mosquitto @ mqtt://10.1.1.20:1883 (lab stand-in for EMQX)
 *  Database : MySQL (localhost / ozlock)
 *
 *  Responsibilities (ozkey-04 §6, ozkey-05 §6)
 *    1. Mint single-use enrollment tokens (BANOI "Add Doorlock" begins here)
 *    2. Enroll locks: verify token, bind device -> site/owner, issue broker
 *       credentials, ack on the command topic
 *    3. Issue/revoke user keys as Tuya 55 AA DPID frames, queue them, flush
 *       on the lock's wake (ozkey/<site>/locks/<id>/heartbeat)
 *
 *  NOT a responsibility, deliberately — DOOR EVENTS ARE NEVER INGESTED.
 *    Removed 2026-07-31 (operator decision, XF-48 §9.4). This server is the
 *    HOSTED RELAY: it is run by us, for other people, over their doors. The
 *    Sovereign Edge whitepaper v3 §4.1 data inventory says we hold connection
 *    metadata (7 d) and security events (90 d) and "explicitly not which lock
 *    opened, when, or by whom" — and `lock_logs` was exactly that. It is gone:
 *    no table, no `log` subscription, no ingest, no query endpoint.
 *
 *    Same rule XF-47 Ask 7 set for `grants.raw_value`. Door events live on
 *    OZPMSSERV / OZKEYSERV — the OPERATOR'S OWN servers, over their own doors,
 *    which is not a sovereignty breach. The XF-47 §8(b) log work (seq /
 *    recorded_at / sync_batch / window_from / window_to) targets those, not this.
 *
 *    DO NOT REINSTATE as a flag or a shorter retention. The guarantee is only
 *    credible because the hosted build cannot do it at all.
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
  // Override to spin a verification instance beside the watcher-run :4200 one.
  HTTP_PORT: Number(process.env.OZLOCK_HTTP_PORT) || 4200,
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
  MQTT_URL: 'mqtt://10.1.1.20:1883',
  BROKER: {
    host: '10.1.1.20',
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
  // SUB_LOG removed 2026-07-31 — see the header. We do not subscribe to
  // `ozkey/<site>/locks/+/log` at all, so door events are never delivered to
  // this process. Locks may still publish there; nothing here consumes it.
  topicCommand: (site, deviceId) => `ozkey/${site}/locks/${deviceId}/command`,
  // A Thread lock has no MQTT client of its own — bridge32 is its gateway and
  // subscribes to its OWN topic (blelock/bridge32/bridge32.ino:489), then
  // demuxes onto the mesh by `target`. ozkey-11 §3.
  topicBridgeCommand: (site, bridgeId) => `ozkey/${site}/bridges/${bridgeId}/command`,
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
      bridge_id VARCHAR(64) NULL,
      mac VARCHAR(17),
      label VARCHAR(255) DEFAULT 'New Doorlock',
      fw VARCHAR(50) NULL,
      status VARCHAR(20) DEFAULT 'enrolled',
      power_profile VARCHAR(20) DEFAULT 'eco',
      -- XF-48 Ask 1: the lock's OWN statement of what it can do, as a JSON
      -- array e.g. ["remote_unlock","pin_sync","audit"]. NULL until firmware
      -- reports it (M3+); until then capability is inferred — see
      -- effectiveCaps(). Never infer once the device has spoken.
      caps VARCHAR(255) NULL,
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

  // bridge_id = the bridge32 gateway a Thread lock is reached through (ozkey-11
  // §3). NULL = the lock is its own MQTT client (direct Wi-Fi, modes 2b/3/4).
  // Only the app learns this — it read the bridge's Thread dataset over BLE at
  // commissioning — so it must tell us at /pairings or the lock is unreachable.
  const [[{ hasBridgeId }]] = await pool.query(
    `SELECT COUNT(*) AS hasBridgeId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'bridge_id'`,
    [CONFIG.DB.database]
  );
  if (!hasBridgeId)
    await pool.query('ALTER TABLE locks ADD COLUMN bridge_id VARCHAR(64) NULL AFTER app_id');

  // XF-48 Ask 1 / ask (E). Additive migration for existing lab rows.
  const [[{ hasCaps }]] = await pool.query(
    `SELECT COUNT(*) AS hasCaps FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'caps'`,
    [CONFIG.DB.database]
  );
  if (!hasCaps)
    await pool.query('ALTER TABLE locks ADD COLUMN caps VARCHAR(255) NULL AFTER power_profile');

  // XF-57 (AN), operator directive 2026-08-03. The lock's OWN statement of its
  // transport, reported on every enroll and heartbeat. Before this the server
  // never knew — it inferred capability from `bridge_id`, and the app kept a
  // private copy written at commissioning that nothing corrected, so a lock
  // converted Thread -> Wi-Fi stayed "Thread" in the app permanently and every
  // remote unlock was refused. `wifi` | `thread`, NULL until the device reports.
  const [[{ hasTransport }]] = await pool.query(
    `SELECT COUNT(*) AS hasTransport FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'transport'`,
    [CONFIG.DB.database]
  );
  if (!hasTransport)
    await pool.query('ALTER TABLE locks ADD COLUMN transport VARCHAR(16) NULL AFTER power_profile');

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

  // MUST run AFTER the CREATE above. Ordered the other way round originally,
  // which worked only because this deployment already had the table: on a FRESH
  // schema the information_schema probe returns 0 (no table, so no column) and
  // the ALTER then fails with "Table 'x.enroll_tokens' doesn't exist" — an init
  // loop that never converges, so a brand-new install could never boot. Found
  // 2026-07-31 bootstrapping ozpms from this same code. The `locks` probe above
  // is safe only because `locks` is created earlier; keep every migration below
  // the CREATE it depends on.
  const [[{ hasTokenAppId }]] = await pool.query(
    `SELECT COUNT(*) AS hasTokenAppId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'enroll_tokens' AND column_name = 'app_id'`,
    [CONFIG.DB.database]
  );
  if (!hasTokenAppId)
    await pool.query('ALTER TABLE enroll_tokens ADD COLUMN app_id VARCHAR(80) NULL AFTER owner_id');

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

  // `lock_logs` is NOT created here — removed 2026-07-31, see the file header.
  // Any rows an older build left in this schema are deliberately NOT dropped by
  // code: destroying an operator's existing data is their decision, not a
  // side effect of a deploy. Drop it by hand when you are satisfied it is
  // migrated:  DROP TABLE lock_logs;

  // App-attributed control-plane audit trail: every action an app performs
  // through OZLOCK (register pairing, grant/revoke a key, remote unlock,
  // settings). This is "what an app did", NOT "what a door did" — it is the
  // §4.1 "security events" class and is RETAINED here on purpose: without it
  // an abuse report is uninvestigable. Kept deliberately when `lock_logs` went.
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

  // Transport routing (ozkey-11 §3). A bridged (Thread) lock never subscribes
  // to MQTT itself, so publishing to its device-scoped topic reaches nobody —
  // the command must go to its BRIDGE's topic, naming the lock in `target`.
  const [[lockRow]] = await pool.query('SELECT bridge_id FROM locks WHERE id = ?', [deviceId]);
  const bridgeId = lockRow && lockRow.bridge_id ? lockRow.bridge_id : null;

  let sent = 0;
  for (const job of queued) {
    // ozkey-05 §6.3: commands must never fire stale.
    if (job.expires_at && new Date(job.expires_at).getTime() < Date.now()) {
      await pool.query("UPDATE pending_queue SET status = 'expired' WHERE id = ?", [job.id]);
      logEvent('warn', `Queue #${job.id} (${job.action_type}) expired before ${deviceId} woke`);
      continue;
    }

    const commandTopic = bridgeId
      ? CONFIG.topicBridgeCommand(siteId, bridgeId)
      : CONFIG.topicCommand(siteId, deviceId);
    const envelope = {
      msg_id: `ozl-${job.id}-${Date.now()}`,
      device_id: deviceId,
      action: job.action_type,
      grant_id: job.grant_id,
      payload_hex: job.payload_hex,
      issued_at: new Date().toISOString(),
      source: 'ozlockserv',
    };
    // bridge32 demuxes on {target, payload} (CONTRACT-BRIDGE / ozkey-11 §3).
    // Send it a MINIMAL envelope: the bridge reads only these two fields and
    // rebuilds its own datagram from them, so msg_id/action/grant_id/issued_at/
    // source never cross the Thread hop and are pure overhead on a constrained
    // link. Keeping them nearly broke the product: PubSubClient's default
    // MQTT_MAX_PACKET_SIZE is 256 bytes and silently discards anything larger,
    // so the ~280-byte full envelope was dropped by every bridge without a
    // trace while short hand-made test publishes sailed through (found live
    // 2026-07-29). bridge32 now also calls setBufferSize(1024), but keeping the
    // wire small is the belt to that braces — a stock-configured bridge, or one
    // on an older build, still works.
    const publishBody = bridgeId
      ? { msg_id: envelope.msg_id, target: deviceId, payload: job.payload_hex }
      : envelope;

    const ok = mqttPublish(commandTopic, publishBody);
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
    logEvent(
      'sync',
      `${deviceId} wake -> burst ${job.action_type} #${job.id} down ${commandTopic}` +
        (bridgeId ? ` (via bridge ${bridgeId})` : '')
    );
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
      [CONFIG.SUB_ENROLL, CONFIG.SUB_HEARTBEAT],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.SUB_ENROLL} + ${CONFIG.SUB_HEARTBEAT}` +
              ' (door-event topic deliberately NOT subscribed — see header)'
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
        // XF-57: the heartbeat is the only message a lock in service sends
        // unprompted, so it is the only thing that can heal a stale row without
        // anyone visiting the door. Written on EVERY beat, not just on change —
        // we have no reliable "changed" signal and a conditional write here would
        // just be a second thing to get wrong.
        const id = deviceIdentity(obj);
        // `fw` too, and it was the same staleness bug as transport — caught on the
        // bench 2026-08-03 with a lock running 1.6 whose row still read 1.4. The
        // heartbeat has ALWAYS carried fw; only handleEnroll wrote it, and an
        // already-enrolled lock never re-enrolls, so the column froze at whatever
        // version first enrolled and never moved again.
        // It matters because `locks.fw` is how we and BANOI tell which contract a
        // device speaks. A frozen value answers that question wrongly and with
        // total confidence.
        const fw = typeof obj.fw === 'string' && obj.fw.length <= 50 ? obj.fw : null;
        await pool.query(
          `UPDATE locks
              SET last_seen_at = NOW(),
                  fw        = COALESCE(?, fw),
                  transport = COALESCE(?, transport),
                  caps      = COALESCE(?, caps)
            WHERE id = ?`,
          [fw, id.transport, id.caps, deviceId]
        );
        const sent = await flushQueueForDevice(siteId, deviceId);
        if (sent > 0) return; // flush already logged
        return; // quiet heartbeat
      }

      /* -- Door access transactions: NOT HANDLED, deliberately ---------------- *
       * Removed 2026-07-31 with `lock_logs` (see header). We no longer subscribe
       * to the `log` topic, so `kind === 'log'` cannot arrive here — but if a
       * future change re-adds that subscription, note what USED to be here:
       * besides the INSERT, this branch called
       *   logEvent('lock', `Door GRANTED — <detail> @ "<label>"`)
       * which put the door event in the rolling event ring served to the
       * dashboard terminal AND on stdout. Dropping only the table would have
       * left the same data in the log stream and looked like a fix. If door
       * events are ever wanted, they belong on OZPMSSERV/OZKEYSERV — not here.
       */
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

  // bridge_id = NULL here is deliberate (2026-08-03). Reaching handleEnroll means
  // the LOCK ITSELF spoke MQTT — which only a direct Wi-Fi lock can do; a Thread
  // lock behind a bridge has no uplink and never enrolls. So a self-enroll is proof
  // the lock is NOT bridged. Found by converting a Thread lock to Wi-Fi (T2-on-WiFi,
  // ozprov.py): the row kept its old bridge_id, and BOTH the caps inference
  // (effectiveCaps) and the app's transport routing (XF-55 §9.1) key off bridge_id,
  // so the now-WiFi lock was still classified Thread-behind-bridge and its unlock was
  // routed to the bridge topic — which no longer reaches it. A device that transitions
  // transports must not carry the old transport's binding.
  // XF-57 (AN): record what the lock says it IS, alongside the binding reset
  // above. Same COALESCE rule as the heartbeat — a pre-XF-57 firmware omits both
  // fields and must not blank them.
  const ident = deviceIdentity(obj);
  await pool.query(
    `UPDATE locks SET app_id = ?, mac = ?, label = ?, fw = ?, status = 'enrolled',
       bridge_id = NULL,
       transport = COALESCE(?, transport), caps = COALESCE(?, caps),
       heartbeat_s = COALESCE(heartbeat_s, ?), broker_username = ?, broker_secret = ?, last_seen_at = NOW()
     WHERE id = ?`,
    [appId, mac, label, obj.fw || null, ident.transport, ident.caps,
     CONFIG.DEFAULT_HEARTBEAT_S, brokerUsername, brokerSecret, deviceId]
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
async function registerPairing(appId, deviceId, label, bridgeId) {
  const [[existing]] = await pool.query('SELECT app_id, status FROM locks WHERE id = ?', [
    deviceId,
  ]);
  if (existing && existing.app_id && appId && existing.app_id !== appId) {
    const e = new Error(`device_id already paired to a different app`);
    e.httpStatus = 409;
    // XF-48 ask (I). NOT `e.code` — mysql2 and Node system errors already use
    // `.code` (ER_DUP_ENTRY, ECONNREFUSED), so writing there would sometimes
    // ship a driver's internal code to the app as if it were our contract.
    e.ozCode = 'device_paired_elsewhere';
    throw e;
  }
  // Fresh registration lands as 'registered' (awaiting the lock's first
  // contact); an already-enrolled lock keeps its status on re-register.
  await pool.query(
    `INSERT INTO locks (id, site_id, app_id, bridge_id, label, status, heartbeat_s)
       VALUES (?, ?, ?, ?, ?, 'registered', ?)
     ON DUPLICATE KEY UPDATE app_id = VALUES(app_id),
       bridge_id = COALESCE(VALUES(bridge_id), bridge_id),
       label = COALESCE(VALUES(label), label)`,
    [deviceId, CONFIG.SITE_ID, appId, bridgeId || null, label || 'New Doorlock', CONFIG.DEFAULT_HEARTBEAT_S]
  );
  logEvent(
    'pair',
    `Pairing registered: app ${appId || '(anon)'} ⇄ device ${deviceId}` +
      (bridgeId
        ? ` via bridge ${bridgeId} (Thread — no self-enroll, see ozkey-11 §3)`
        : ' — awaiting doorlock contact')
  );
  await recordAudit(appId, deviceId, 'pair', `registered pairing (label "${label || 'New Doorlock'}")`);
}

/* -- Pairing registration (trust-model v2, XF-42 §13.2) ---------------------- */
api.post('/pairings', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { app_id, device_id, label, bridge_id } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    const deviceId = device_id ? String(device_id).slice(0, 64) : null;
    // Optional: present only for a Thread lock reached through a bridge32
    // gateway (ozkey-11 §3). Absent = direct Wi-Fi lock, routed as before.
    const bridgeId = bridge_id ? String(bridge_id).slice(0, 64) : null;
    if (!appId || !deviceId) {
      return res
        .status(400)
        .json({
          ok: false,
          code: 'missing_fields',
          error: 'app_id and device_id are required (the app grants both)',
        });
    }
    await registerPairing(appId, deviceId, label, bridgeId);
    res.json({
      ok: true,
      device_id: deviceId,
      app_id: appId,
      bridge_id: bridgeId,
      provision_payload: buildProvisionPayload(appId, deviceId),
    });
  } catch (err) {
    // ozCode is OUR contract; err.httpStatus present means a deliberate throw.
    // Anything else is an unhandled fault and must not masquerade as a known code.
    res.status(err.httpStatus || 500).json({
      ok: false,
      code: err.ozCode || (err.httpStatus ? 'request_failed' : 'internal_error'),
      error: err.message,
    });
  }
});

api.get('/pairings/status', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = String(req.query.device_id || '');
    const [[row]] = await pool.query(
      'SELECT id, app_id, bridge_id, status, mac FROM locks WHERE id = ?',
      [deviceId]
    );
    if (!row)
      return res.status(404).json({ ok: false, code: 'pairing_not_found', error: 'no such pairing' });
    res.json({
      ok: true,
      device_id: row.id,
      app_id: row.app_id,
      bridge_id: row.bridge_id,
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
    // ozCode is OUR contract; err.httpStatus present means a deliberate throw.
    // Anything else is an unhandled fault and must not masquerade as a known code.
    res.status(err.httpStatus || 500).json({
      ok: false,
      code: err.ozCode || (err.httpStatus ? 'request_failed' : 'internal_error'),
      error: err.message,
    });
  }
});

/* -- Locks (the owner's fleet) ----------------------------------------------- */
api.get('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    // bridge_id + caps, same as GET /locks/:id (2026-08-02). Patching only the
    // single-lock read was not enough: the app builds its lock LIST from here, so
    // a lock could report remote_unlock when opened individually and still render
    // as BLE-only in the list the user actually taps. Both reads must agree.
    const [rows] = await pool.query(
      `SELECT id, site_id, app_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport
         FROM locks ORDER BY enrolled_at DESC`
    );
    const locks = rows.map((l) => {
      const { caps, source } = effectiveCaps(l);
      return { ...l, caps, caps_source: source };
    });
    res.json({ ok: true, locks });
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
    // bridge_id + caps added 2026-08-02. They were being SELECTed nowhere, so the
    // app had no way to learn a Thread lock is remotely reachable and fell back to
    // BLE — which a commissioned lock never advertises for. Symptom was "Do not
    // see doorlock" on a lock the server could have unlocked: effectiveCaps()
    // already inferred remote_unlock from bridge_id, we simply never published it.
    // This is XF-48 ask 1 (the capability field) landing on the read path.
    const [[lock]] = await pool.query(
      `SELECT id, app_id, site_id, mac, label, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport
         FROM locks WHERE id = ?`,
      [req.params.id]
    );
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${req.params.id} not found` });
    const { caps, source } = effectiveCaps(lock);
    // caps_source lets the app distinguish "the device told us" from "we inferred
    // it from a bound bridge" — XF-48 §9.5 wants the device's own report to win
    // once the state uplink exists, and silently swapping one for the other would
    // be undiagnosable from the app side.
    res.json({ ok: true, lock: { ...lock, caps, caps_source: source } });
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
        return res
          .status(400)
          .json({ ok: false, code: 'invalid_power_profile', error: 'invalid power_profile' });
      sets.push('power_profile = ?');
      params.push(power_profile);
    }
    if (heartbeat_s !== undefined) {
      // Clamp to the SAME 60–600 the firmware enforces (clampHeartbeatS,
      // doorlock.ino:285). The old Math.max(5, …) had no ceiling, so a PATCH of 5
      // or 86400 was stored and the row then disagreed with anything the lock would
      // actually run — the "row says X, device runs Y" class ftpos flagged in
      // XF-55 §11.2. One authority, one range.
      const hb = Number(heartbeat_s) || CONFIG.DEFAULT_HEARTBEAT_S;
      sets.push('heartbeat_s = ?');
      params.push(hb < 60 ? 60 : hb > 600 ? 600 : hb);
    }
    if (!sets.length)
      return res.status(400).json({ ok: false, code: 'nothing_to_update', error: 'nothing to update' });
    params.push(req.params.id);
    const [r] = await pool.query(`UPDATE locks SET ${sets.join(', ')} WHERE id = ?`, params);
    if (r.affectedRows === 0)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${req.params.id} not found` });
    logEvent('info', `Lock ${req.params.id} settings updated (${sets.join(', ')})`);
    res.json({ ok: true, id: req.params.id });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Clear the fleet (start fresh) ------------------------------------------- *
 * No FK cascade from locks → grants/pending_queue (they only carry a device_id
 * column), so wipe the dependents explicitly. DELETE /locks clears every lock
 * for the site; DELETE /locks/:id removes one. Lab/dev convenience.
 *
 * `lock_logs` is intentionally absent — this server no longer creates it, and
 * DELETEing a table that does not exist would throw on a fresh schema.
 */
async function purgeLockRows(conn, where, args) {
  await conn.query(`DELETE FROM grants WHERE ${where}`, args);
  await conn.query(`DELETE FROM pending_queue WHERE ${where}`, args);
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
    // Unpair the physical lock too (BANOI "Gỡ khoá" must reset the device,
    // not just the record): tell it to wipe NVS and return to ADVERTISING.
    // Best-effort — an offline lock misses it and needs the on-device reset.
    const [[lock]] = await conn.query('SELECT site_id FROM locks WHERE id = ?', [id]);
    if (lock) {
      mqttPublish(CONFIG.topicCommand(lock.site_id || CONFIG.SITE_ID, id), {
        op: 'factory_reset',
        ts: new Date().toISOString(),
      });
    }
    await conn.beginTransaction();
    await purgeLockRows(conn, 'device_id = ?', [id]);
    const [d] = await conn.query('DELETE FROM locks WHERE id = ?', [id]);
    await conn.commit();
    if (d.affectedRows === 0)
      return res.status(404).json({ ok: false, code: 'lock_not_found', error: `Lock ${id} not found` });
    logEvent('info', `Doorlock ${id} removed + factory_reset sent`);
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
      return res.status(400).json({
        ok: false,
        code: 'missing_fields',
        error: 'user_name and raw_value are required',
      });
    }
    if (type === 'fingerprint') {
      return res.status(422).json({
        ok: false,
        code: 'credential_type_unsupported',
        error: 'fingerprint credentials are on hold — DP codec supports pin/rfid only',
      });
    }
    if (!SUPPORTED_CRED_TYPES.includes(type)) {
      return res
        .status(400)
        .json({
          ok: false,
          code: 'credential_type_unsupported',
          error: `type must be one of: ${SUPPORTED_CRED_TYPES.join(', ')}`,
        });
    }

    const [[lock]] = await conn.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

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
      return res.status(400).json({ ok: false, code: 'bad_request', error: err.message });
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
/* -- XF-48 ask (E): capability enforcement, server-side ---------------------- *
 * ftpos raised this and they were right: removing the remote-unlock affordance
 * from BANOI fixes one build of one client. It does not close the path for older
 * app versions, other clients, or a direct API call — and after their fix the
 * exposure would be INVISIBLE to them, because the only thing that knew the rule
 * was the UI they had just changed.
 *
 * Authority order, and the order matters:
 *   1. `locks.caps` — what the LOCK reported about itself (firmware, M3+).
 *      A device's own statement, per XF-48 Ask 1. Never override it by
 *      inference; inference is what rots.
 *   2. Interim rule until caps ships: remote unlock requires a bound bridge.
 *
 * Why a bridge is the right proxy: a direct Wi-Fi lock wakes every 2-10 minutes,
 * and the unlock below carries a 60 s expiry precisely so a command can never
 * fire stale. So for an ECO lock the command already almost always expired
 * unheard — the door simply never opened and nothing said why. Enforcement does
 * not remove a capability; it converts a silent, undiagnosable expiry into a
 * refusal the app can render.
 */
const ENFORCE_CAPS = process.env.OZLOCK_ENFORCE_CAPS !== '0';

/**
 * XF-57 (AN). Pull the lock's self-reported identity out of an enroll/heartbeat
 * payload, for storage. Returns `{transport, caps}` where each is either a value
 * to write or NULL meaning "the device did not say" — callers COALESCE, so a
 * pre-XF-57 firmware (doorlock <= 1.5) never blanks a column it simply omits.
 *
 * Validated, not trusted: `transport` must be one of the two we know, and `caps`
 * must be an array of known strings. A device is authoritative about itself, not
 * about our schema — and this arrives over an MQTT broker that is anonymous-open
 * on the bench, so anything unrecognised is dropped rather than stored.
 */
const KNOWN_TRANSPORTS = ['wifi', 'thread'];
const KNOWN_CAPS = ['remote_unlock', 'assisted_unlock', 'pin_sync', 'audit'];

// XF-58. The "visitor at the door" unlock: the owner authorises remotely while on
// the phone, and the visitor's keypad touch completes it. 60 s — operator's call,
// and it is safe ONLY because doorlock-1.5 refuses the command without a recent
// touch. Without that lock-side check the window would be a probability rather
// than a requirement: a sleeping lock also wakes on its heartbeat timer, so a
// longer window would make an UNATTENDED open more likely, not less.
const ASSISTED_UNLOCK_MS = 60_000;

function deviceIdentity(obj) {
  const t = typeof obj.transport === 'string' ? obj.transport.toLowerCase() : null;
  const transport = KNOWN_TRANSPORTS.includes(t) ? t : null;

  let caps = null;
  if (Array.isArray(obj.caps)) {
    const clean = obj.caps.filter((c) => KNOWN_CAPS.includes(c));
    // An empty array after filtering is still a real statement ("I can do none
    // of these"), but an all-junk report is not — treat it as nothing said.
    if (clean.length === obj.caps.length) caps = JSON.stringify(clean);
  }
  return { transport, caps };
}

function effectiveCaps(lock) {
  // XF-58 (AS) — `bridge_id` is CONFIGURATION: is a bridge bound. Liveness must
  // NEVER enter this function. Two reasons, and the second is the stronger:
  //   1. remote_unlock would blink out during a bridge reboot, and the app would
  //      send the user to walk to a door that would have opened in a second.
  //   2. `caps` is now partly device-reported and CACHED in the app's local row
  //      (XF-57). A field worth caching must be stable; mixing a by-the-second
  //      signal in would make that cache wrong by design — XF-57's staleness bug
  //      again, but unfixable, because the truth would change faster than sync.
  // "Bound but currently unreachable" belongs in a SEPARATE `bridge_state` field
  // (live / stale / unknown), deferred to the (AC) bridges table. Do not reach
  // for a liveness signal here to "improve" the answer.
  const bridged = !!lock.bridge_id;

  // What we can deduce with no word from the device. Note `assisted_unlock` is
  // NOT here, and cannot be — see the rule below.
  const inferred = bridged
    ? ['remote_unlock', 'pin_sync', 'audit']
    : ['pin_sync', 'audit'];

  let device = null;
  if (lock.caps) {
    try {
      const c = JSON.parse(lock.caps);
      if (Array.isArray(c)) device = c;
    } catch (_) {
      // Malformed device report: fall through to inference rather than trust it.
    }
  }
  if (!device) return { caps: inferred, source: 'inferred' };

  const caps = [];

  // XF-57 — remote_unlock needs BOTH sides to agree, because each knows half:
  //   • the device knows its transport — a Wi-Fi lock sleeps and can never do
  //     remote unlock, whatever the deployment looks like;
  //   • the server knows whether a bridge is actually bound — a Thread lock is
  //     only remotely reachable THROUGH one, and the lock cannot tell whether
  //     its bridge is alive or was removed yesterday.
  // Trusting the device alone would mirror the bug XF-57 fixed: a Thread lock
  // whose bridge is gone would keep claiming remote_unlock and every attempt
  // would queue and expire unheard.
  if (device.includes('remote_unlock') && bridged) caps.push('remote_unlock');

  // XF-58 — assisted_unlock is DEVICE-ONLY and must NEVER be inferred.
  //
  // This is a safety interlock, not a modelling nicety. The whole guarantee of an
  // assisted unlock — "the door opens only if somebody is standing at it" — is
  // enforced in FIRMWARE (doorlock-1.5 refuses the command without a recent
  // keypad touch). A lock running anything older ignores `action` entirely and
  // forwards the frame straight to the MCU, so granting the capability by
  // inference would hand an unenforced unlock to exactly the firmware that cannot
  // enforce it. Only a device new enough to report caps at all is new enough to
  // honour them, so the report IS the version check.
  //
  // General rule worth keeping: a capability whose enforcement lives in firmware
  // may only ever be device-reported. Never infer one.
  if (device.includes('assisted_unlock')) caps.push('assisted_unlock');

  for (const cap of ['pin_sync', 'audit']) if (device.includes(cap)) caps.push(cap);
  return { caps, source: 'device' };
}

api.post('/locks/:id/unlock', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });
    // A bridged (Thread) lock has no MQTT uplink of its own, so handleEnroll can
    // never run for it and its status cannot advance past 'registered' — that is
    // the ozkey-10 uplink gap, not a half-finished pairing. Commands still reach
    // it, because the BRIDGE is the MQTT client. Direct locks keep the stricter
    // 'enrolled' bar, which for them still means "the lock has spoken to us".
    const reachable = lock.status === 'enrolled' || (lock.bridge_id && lock.status === 'registered');
    if (!reachable) {
      return res
        .status(409)
        .json({
          ok: false,
          code: 'lock_not_enrolled',
          error: `Lock ${deviceId} is not enrolled yet (status: ${lock.status})`,
          status_now: lock.status,
        });
    }

    // XF-48 ask (E) — capability gate. Checked BEFORE anything is queued, so a
    // refused unlock leaves no row to expire later and no audit entry implying
    // it was attempted at the door.
    const { caps, source } = effectiveCaps(lock);
    const assisted = !caps.includes('remote_unlock') && caps.includes('assisted_unlock');
    if (ENFORCE_CAPS && !caps.includes('remote_unlock') && !assisted) {
      logEvent(
        'warn',
        `Remote UNLOCK refused for "${lock.label}" — no remote_unlock capability (${source})`
      );
      return res.status(409).json({
        ok: false,
        code: 'remote_unlock_unsupported',
        error: 'remote_unlock_unsupported', // kept: ftpos shipped against error= for this one
        detail:
          'This lock cannot be unlocked remotely. Unlock is Bluetooth-at-the-door; ' +
          'its network link carries key management, PIN sync and audit on a periodic ' +
          'wake, not live commands. Add an OZKEY bridge to enable remote unlock.',
        caps,
        caps_source: source, // 'device' = the lock said so; 'inferred' = no bridge bound
        device_id: deviceId,
        ref: 'XF-48 §3',
      });
    }

    // §6.3: an unlock MUST NOT fire stale. Queue with an expiry; the flush drops
    // it if the lock doesn't wake in time.
    //
    // XF-58: an ASSISTED unlock additionally carries `action_type =
    // 'assisted-unlock'`, which doorlock-1.5 refuses unless the keypad was
    // touched in the last 30 s. The expiry alone would not be enough — it bounds
    // WHEN the command may run, never WHETHER anybody was at the door, and a
    // sleeping lock wakes on its heartbeat timer too. The device-side check is
    // what makes "someone must be there" a requirement instead of a probability.
    const payloadHex = toSpacedHex(buildUnlockFrame());
    const actionType = assisted ? 'assisted-unlock' : 'unlock';
    const windowMs = assisted ? ASSISTED_UNLOCK_MS : 60_000;
    const expiresAt = new Date(Date.now() + windowMs);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, payload_hex, status, expires_at)
       VALUES (?, ?, NULL, ?, ?, 'queued', ?)`,
      [deviceId, lock.site_id, actionType, payloadHex, expiresAt]
    );

    logEvent(
      'key',
      `${assisted ? 'ASSISTED' : 'Remote'} UNLOCK queued for "${lock.label}" ` +
        `(queue #${queueResult.insertId}, expires ${windowMs / 1000}s` +
        `${assisted ? ', needs a keypad touch' : ''})`
    );
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
      // XF-58: everything the app needs to run the countdown, so it never has to
      // hardcode our window. `mode: 'assisted'` is the app's cue to prompt
      // "bảo khách chạm vào bàn phím" and show the seconds ticking — on this path
      // `delivered` means the command is ARMED, not that the door opened, since
      // the lock will still refuse it if nobody touches.
      mode: assisted ? 'assisted' : 'remote',
      window_s: windowMs / 1000,
      requires_touch: assisted,
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
      return res.status(404).json({
        ok: false,
        code: 'grant_not_found',
        error: `Grant #${grantId} not found on ${deviceId}`,
      });
    if (grant.sync_status === 'revoked')
      return res.status(409).json({
        ok: false,
        code: 'grant_already_revoked',
        error: `Grant #${grantId} is already revoked`,
      });
    const [[dupe]] = await conn.query(
      `SELECT id FROM pending_queue
        WHERE grant_id = ? AND action_type = 'revoke-key' AND status = 'queued'`,
      [grantId]
    );
    if (dupe)
      return res.status(409).json({
        ok: false,
        code: 'revoke_already_queued',
        error: `Grant #${grantId} already has revoke queue #${dupe.id} pending`,
        queue_id: dupe.id,
      });

    let frame;
    try {
      frame = buildDeleteFrame({ type: grant.type, slotNumber: grant.slot_number });
    } catch (err) {
      return res.status(422).json({ ok: false, code: 'unprocessable', error: err.message });
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

/* -- Door transaction log — GONE on the hosted relay -------------------------- *
 * Answers 410, not 404 and not an empty list. BANOI calls this today
 * (`doorlock_service.dart:837-843` merges it into the event feed), so the
 * failure has to be *diagnosable*: an empty `log: []` would render as "this
 * door has never been opened", which is a lie, and a 404 reads as a bad URL.
 * 410 Gone says the resource intentionally no longer exists here.
 */
api.get('/locks/:id/log', (_req, res) => {
  res.status(410).json({
    ok: false,
    code: 'gone',
    error: 'gone',
    detail:
      'Door event history is not held by the hosted relay. This server stores ' +
      'no record of which lock opened, when, or by whom (Sovereign Edge v3 ' +
      '§4.1). Door events are available only from an operator-run OZPMSSERV / ' +
      'OZKEYSERV instance, or on-device.',
    since: '2026-07-31',
    ref: 'XF-48 §9.4',
  });
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
  if (!device_id)
    return res.status(400).json({ ok: false, code: 'missing_fields', error: 'device_id required' });
  try {
    const [[lock]] = await pool.query('SELECT site_id FROM locks WHERE id = ?', [device_id]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${device_id} not found` });
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
