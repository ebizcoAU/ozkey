/*
 * ============================================================================
 *  OZPMSSERV — OZPMS Property-Management Server (lab deployment)
 *  ---------------------------------------------------------------------------
 *  Role     : The OPERATOR'S OWN server, run by the property operator on their
 *             own infrastructure over their own doors. Forked from ozlockserv
 *             2026-07-31 (operator: "OZLOCKSERV IS OZPMS SERVER", XF-48 §9.4);
 *             rebuilt on ozserver-core in S1 (ozkey-50 §7), so it now shares
 *             ozlock's lock side: presence, XF-144 bridge failover, the opaque
 *             sealed relay, the write queue and the August fixes.
 *
 *             What still makes it a separate product, not a flag on ozlock:
 *               - OZLOCKSERV is the HOSTED RELAY we run for other people and
 *                 holds no door events at all (Sovereign Edge v3 §4.1).
 *               - OZPMSSERV may keep door events (`lock_logs`), because the
 *                 operator holds them over their own doors. It also keeps any
 *                 grants columns its DB already has: the core's irreversible
 *                 plaintext-column drop is ozlock's opt-in, not ours.
 *  Port     : 4420  (REST base /ozpmsserv/api)   [OZPMS_HTTP_PORT]
 *  Site     : 'pms' — must differ from ozlockserv's 'lab' (see CONFIG.SITE_ID)
 *  Broker   : lab Mosquitto @ mqtt://10.1.1.42:1883 [OZPMS_MQTT_URL]
 *             (S1: moved from 10.1.1.20, as ozlock did 2026-09-01)
 *  Database : MySQL (localhost / ozpms)          [OZPMS_DB_NAME]
 *
 *  S1 changes against the July fork, deliberately:
 *    - NO server-side Tuya frames and NO unsealed unlock (ozkey-50 §8.3). Grants,
 *      revokes and unlock take an app-sealed `envelope_hex` only, like ozlock.
 *    - Topics move to `ozkie/pms/…`; `ozkey/pms/…` stays subscribed as the
 *      legacy twin until bridges are reconfigured (ozlock's S16 path).
 *    - Lock broker secrets stay scrypt-hashed (S−1), now via the core.
 *
 *  Lab simplifications — NOT production-ready:
 *    - single seeded owner + site ('pms'). REST has NO auth yet: the lock API
 *      is mounted open only when OZPMS_LAB_OPEN_API=1 is set; otherwise (and
 *      always in production) the server refuses to start. Real tenancy and auth are ozkey-51 (S5). ozpms must not go
 *      on a customer VPS before then.
 *    - door events come only from the v1 plaintext `log` topic, which only
 *      Wi-Fi-direct v1 locks publish (bench locks are BLE/Thread via a bridge,
 *      so expect zero rows). Sealed door history is ozkey-51/S5.
 * ============================================================================
 */

'use strict';

const express = require('express');
const cors = require('cors');
const mysql = require('mysql2/promise');
const mqtt = require('mqtt');
const path = require('path');
const { createCore, util: coreUtil } = require('../../ozserver-core');
const { IS_PRODUCTION, detectLanIp, loadDotEnv, resolveSecret, enforceSecrets, normalizeMac, deviceIdFromMac } =
  coreUtil;

/* ---------------------------------------------------------------------------
 * S−1 (ozkey-50 §8.2): fail-closed secrets — env first, then a 0600 `.env`
 * beside this file, never the source. enforceSecrets() is the first thing
 * boot() does and exits on any required secret that is missing.
 * ------------------------------------------------------------------------- */
loadDotEnv(path.join(__dirname, '.env'));

const SECRETS = {
  DB_PASSWORD: resolveSecret(['OZPMS_DB_PASSWORD'], { required: 'always' }),
  // S1c: the Wi-Fi the provision payload hands a lock. No literal in source.
  PROVISION_WIFI_SSID: resolveSecret(['PROVISION_WIFI_SSID'], { required: 'always' }),
  PROVISION_WIFI_PASSWORD: resolveSecret(['PROVISION_WIFI_PASSWORD'], { required: 'always' }),
};

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
const MQTT_URL = process.env.OZPMS_MQTT_URL || 'mqtt://10.1.1.42:1883';

const CONFIG = {
  // Override to spin a verification instance beside the watcher-run :4420 one.
  HTTP_PORT: Number(process.env.OZPMS_HTTP_PORT) || 4420,
  SERVER_IP: process.env.OZPMS_SERVER_IP || detectLanIp(),
  // MUST NOT be 'lab'. Topics are site-pinned so several servers can share one
  // broker, but two servers on the SAME site both consume every heartbeat and
  // both flush the queue — the lock would receive each command twice.
  // OZLOCKSERV owns 'lab'; OZLODGE owns 'hotel'; this server owns 'pms'.
  SITE_ID: 'pms',
  DB: {
    host: 'localhost',
    user: 'root',
    password: SECRETS.DB_PASSWORD, // S−1: OZPMS_DB_PASSWORD, required in every mode
    database: process.env.OZPMS_DB_NAME || 'ozpms',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL,
  BROKER: {
    host: new URL(MQTT_URL).hostname,
    tcp_port: Number(new URL(MQTT_URL).port) || 1883,
    ws_port: 9001,
    ws_path: '/mqtt',
  },
  // ozkie/ is primary; ozkey/ (the July scheme) is the legacy twin during the
  // topic move (ozkey-50 §7 S1, ozlock's S16).
  SUB_ENROLL: 'ozkie/pms/locks/+/enroll',
  SUB_ENROLL_LEGACY: 'ozkey/pms/locks/+/enroll',
  SUB_HEARTBEAT: 'ozkie/pms/locks/+/heartbeat',
  SUB_HEARTBEAT_LEGACY: 'ozkey/pms/locks/+/heartbeat',
  SUB_MEMBER_REQUEST_REMOVE: 'ozkie/pms/members/+/request_remove',
  SUB_MEMBER_REQUEST_REMOVE_LEGACY: 'ozkey/pms/members/+/request_remove',
  SUB_MEMBER_ACK_REMOVE: 'ozkie/pms/members/+/ack_remove',
  SUB_MEMBER_ACK_REMOVE_LEGACY: 'ozkey/pms/members/+/ack_remove',
  SUB_UPLINK: 'ozkie/pms/locks/+/uplink',
  SUB_UPLINK_LEGACY: 'ozkey/pms/locks/+/uplink',
  SUB_PRESENCE_LOCKS: 'ozkie/pms/locks/+/presence',
  SUB_PRESENCE_LOCKS_LEGACY: 'ozkey/pms/locks/+/presence',
  SUB_PRESENCE_BRIDGES: 'ozkie/pms/bridges/+/presence',
  SUB_PRESENCE_BRIDGES_LEGACY: 'ozkey/pms/bridges/+/presence',
  SUB_BRIDGE_SCAN_RESULT: 'ozkie/pms/bridges/+/scan_result',
  SUB_PEER_STATUS: 'ozkie/pms/bridges/+/peer_status',
  // LEGACY door events: v1 Wi-Fi-direct locks only (ozdoorlock_core.h
  // publishLog()). See the header; don't build features on this path.
  SUB_DOOR_LOG: 'ozkie/pms/locks/+/log',
  SUB_DOOR_LOG_LEGACY: 'ozkey/pms/locks/+/log',
  onDoorLog: (siteId, deviceId, obj) => ingestLegacyDoorLog(siteId, deviceId, obj),
  topicCommand: (site, deviceId) => `ozkie/${site}/locks/${deviceId}/command`,
  topicBridgeCommand: (site, bridgeId) => `ozkie/${site}/bridges/${bridgeId}/command`,
  topicTime: (site) => `ozkie/${site}/time`,
  // Overridable for the regression suite (S1d); unset = 5 s.
  BRIDGE_RESET_TIMEOUT_MS: Number(process.env.OZPMS_BRIDGE_RESET_TIMEOUT_MS) || 5000,
  LOCK_RESET_TIMEOUT_MS: Number(process.env.OZPMS_LOCK_RESET_TIMEOUT_MS) || 5000,
  UTC_PUSH_REFRESH_MS: 10 * 60 * 1000,
  AUDIT_LOG_RETENTION_MS: 90 * 24 * 3600 * 1000,
  AUDIT_LOG_PURGE_INTERVAL_MS: 24 * 3600 * 1000,
  ENROLL_TOKEN_TTL_MS: 10 * 60 * 1000, // ozkey-05 §7.5
  DEFAULT_HEARTBEAT_S: 60,
  SERVICE_NAME: 'ozpmsserv',
  ENROLL_ISSUED_BY: 'OZPMS/',
  FAILOVER_GRACE_MS: Number(process.env.OZPMS_FAILOVER_GRACE_MS) || 2 * 60 * 1000,
  FAILOVER_SWEEP_MS: Number(process.env.OZPMS_FAILOVER_SWEEP_MS) || 30_000,
  // The operator's own data: never let a deploy drop grants columns.
  DROP_PLAINTEXT_GRANT_COLUMNS: false,
  ENFORCE_CAPS: process.env.OZPMS_ENFORCE_CAPS !== '0',
  HASH_LOCK_BROKER_SECRETS: true, // S−1, kept from the July fork
  PROVISION_WIFI_SSID: SECRETS.PROVISION_WIFI_SSID,
  PROVISION_WIFI_PASSWORD: SECRETS.PROVISION_WIFI_PASSWORD,
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
 * ozserver-core: one core per process; everything it needs is here.
 * ------------------------------------------------------------------------- */
const core = createCore({ CONFIG, logEvent, mysql, mqtt });
const { purgeOldAuditLog, isMqttConnected, initMqtt, initDatabase, pageParams, rangeWhere } = core;

/** OZPMS's own tables, after the core schema. */
async function initPmsTables() {
  // LEGACY door events (v1 Wi-Fi-direct locks only, see the header). Same
  // columns as the July fork, so an existing ozpms DB keeps its rows.
  await core.pool.query(`
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
}

/** CONFIG.onDoorLog: one plaintext door transaction from a v1 Wi-Fi-direct lock. */
async function ingestLegacyDoorLog(siteId, deviceId, obj) {
  const mac = normalizeMac(obj.mac) || null;
  const result = String(obj.result || 'unknown').slice(0, 20);
  const detail = String(obj.detail || '').slice(0, 255);
  const lockTs = obj.ts ? new Date(obj.ts).toISOString() : new Date().toISOString();
  await core.pool.query(
    'INSERT INTO lock_logs (device_id, site_id, mac, result, detail, lock_ts) VALUES (?, ?, ?, ?, ?, ?)',
    [deviceId, siteId, mac, result, detail, lockTs]
  );
  const [[lk]] = await core.pool.query('SELECT label FROM locks WHERE id = ?', [deviceId]);
  logEvent('lock', `Door ${result.toUpperCase()} — ${detail || 'no detail'} @ "${lk ? lk.label : deviceId}" (legacy log)`);
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
app.use('/ozpmsserv/api', api);
app.use('/ozpms/api', api);

function guardDb(res) {
  if (!core.pool) {
    res.status(503).json({ ok: false, error: 'Database not ready' });
    return false;
  }
  return true;
}

/* -- Health ---------------------------------------------------------------- */
api.get('/health', (req, res) => {
  res.json({
    ok: true,
    service: 'ozpmsserv',
    site: CONFIG.SITE_ID,
    db: !!core.pool,
    mqtt: isMqttConnected(),
    uptime_s: Math.floor(process.uptime()),
  });
});

/* -- The lock API (ozserver-core/api.js) -------------------------------------
 * OZPMS has no REST auth yet (ozkey-51). Until it does, the open lock API is
 * mounted ONLY when the lab says so explicitly (OZPMS_LAB_OPEN_API=1, in the
 * env or the 0600 .env). Anything else — unset, a customer VPS started without
 * NODE_ENV, or production — refuses to start (fail closed, ozkey-50 §7 S1).
 */
function ozpmsLabOnlyAccess() {
  if (process.env.OZPMS_LAB_OPEN_API !== '1' || IS_PRODUCTION) {
    throw new Error(
      'OZPMS has no REST auth yet (ozkey-51); refusing to mount the lock API. ' +
        'Lab only: set OZPMS_LAB_OPEN_API=1 (never in production)'
    );
  }
  logEvent('warn', 'OZPMS_LAB_OPEN_API=1: the lock API is OPEN (lab only, no REST auth until ozkey-51)');
  return (_req, _res, next) => next();
}
const ozpmsAuth = ozpmsLabOnlyAccess();
core.mountLockApi(api, {
  auth: ozpmsAuth,
  // No Nexus hook: Nexus is a registry only (§8a); whether ozpms locks are
  // registered there at all is an open operator question (ozkey-50 §9 Q7).
  purgeExtraRows: (conn, where, args) => conn.query(`DELETE FROM lock_logs WHERE ${where}`, args),
});

/* -- Door transaction log (LEGACY plaintext path; paginated + date range) ----- */
api.get('/locks/:id/log', ozpmsAuth, async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { where, params } = rangeWhere('device_id = ?', [req.params.id], req.query);
    const { limit, offset } = pageParams(req.query);
    const [[{ total }]] = await core.pool.query(`SELECT COUNT(*) AS total FROM lock_logs WHERE ${where}`, params);
    const [rows] = await core.pool.query(
      `SELECT * FROM lock_logs WHERE ${where} ORDER BY id DESC LIMIT ? OFFSET ?`,
      [...params, limit, offset]
    );
    res.json({ ok: true, log: rows, total, limit, offset });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Terminal feed: the event ring is the profile's (logEvent above) -------- */
api.get('/events', (req, res) => {
  const after = Number(req.query.after) || 0;
  res.json({ ok: true, events: eventRing.filter((e) => e.id > after) });
});

/* ---------------------------------------------------------------------------
 * Boot sequence
 * ------------------------------------------------------------------------- */
async function boot() {
  logEvent('info', 'OZPMSSERV booting — operator-owned property-management server (site: pms)');
  enforceSecrets();

  let attempts = 0;
  for (;;) {
    try {
      await initDatabase();
      await initPmsTables();
      break;
    } catch (err) {
      attempts++;
      logEvent('error', `MySQL init failed (attempt ${attempts}): ${err.message} — retry in 5s`);
      await new Promise((r) => setTimeout(r, 5000));
    }
  }

  initMqtt();

  // XF-125 P1: audit_log retention, once at boot then daily (same as ozlock).
  await purgeOldAuditLog();
  setInterval(purgeOldAuditLog, CONFIG.AUDIT_LOG_PURGE_INTERVAL_MS);

  app.listen(CONFIG.HTTP_PORT, () => {
    logEvent('info', `HTTP directory listening on http://localhost:${CONFIG.HTTP_PORT}/ozpmsserv/api`);
  });
}

process.on('unhandledRejection', (err) => {
  logEvent('error', `Unhandled rejection: ${err && err.message ? err.message : err}`);
});

// Same as ozlock (2026-09-03): log a synchronous crash before exiting, so a
// silent `node --watch` stall leaves a reason behind.
process.on('uncaughtException', (err) => {
  logEvent('error', `Uncaught exception: ${err && err.stack ? err.stack : err}`);
  process.exit(1);
});

if (require.main === module) {
  boot();
} else {
  module.exports = { CONFIG, normalizeMac, deviceIdFromMac };
}
