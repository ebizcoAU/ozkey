/*
 * ============================================================================
 *  OZKIT SERVER — SIMLOCK fleet backend (lab deployment)
 *  ---------------------------------------------------------------------------
 *  Role     : Subscribes to SIMLOCK N8 board telemetry over MQTT, provisions
 *             each board's X25519 identity via NEXUS (development mode,
 *             mirroring doorlock's app<->NEXUS flow — ozkey-34.md §2 — with
 *             this server playing the courier role the app plays there,
 *             since SIMLOCK has no BLE/app of its own), unseals sealed
 *             telemetry, and serves it over REST.
 *  Port     : 5400 (REST). Frontend reserved on 5410, not built yet.
 *  Broker   : mqtt.ebizco.com.au:8883 (TLS, auth) — dedicated ozkit broker,
 *             deliberately separate from the lab broker ozlock/ozlodge/ozpms
 *             use (operator's decision, 2026-08-17).
 *  Database : MySQL (localhost / ozkit)
 *
 *  Why the identity layer exists at all: SIMLOCK's MQTT credentials were a
 *  single shared plaintext string baked into every unit's firmware binary —
 *  trivially dumped with `esptool read_flash` + `strings`, and one leak
 *  compromises the whole fleet. This replaces that with per-board X25519
 *  identity: each board's telemetry is sealed (AES-256-GCM under an
 *  ECDH-derived key) rather than merely authenticated, mirroring doorlock's
 *  actual security model (payload confidentiality even though the broker/
 *  relay isn't trusted) rather than just swapping one shared secret for
 *  another.
 *
 *  Envelope format is a direct mirror of blelock/common/ozcrypto.h's
 *  ozEnvSeal()/ozEnvOpen() (also mirrored firmware-side in
 *  blelock/simlock/ozkit_crypto.h) — same field layout, so both sides agree
 *  on wire format without sharing a library:
 *    [1B version=0x02][8B counter BE][12B nonce = 4B random + 8B counter BE]
 *    [ciphertext][16B tag]
 *  AAD = version + counter + board_id. HKDF salt = board_id + "ozkit",
 *  info = "ozkit/board->server".
 * ============================================================================
 */

'use strict';

const express = require('express');
const cors = require('cors');
const mysql = require('mysql2/promise');
const mqtt = require('mqtt');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const http = require('http');

// ── Config ───────────────────────────────────────────────────────────────────
const CONFIG = {
  PORT: process.env.OZKIT_PORT || 5400,
  DB: {
    host: 'localhost',
    user: 'root',
    password: 'Cableman',
    database: 'ozkit',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL: 'mqtts://mqtt.ebizco.com.au:8883',
  MQTT_USER: 'simlock',
  MQTT_PASS: 'esp32c6n8',
  // NEXUS — a separate, real, already-built service (~/Documents/Dev/nexus/),
  // NOT ozlock/server. See ozkey-35.md §2's glossary for why that distinction
  // matters — the two have opposite policies on holding keys.
  NEXUS_BASE_URL: process.env.NEXUS_BASE_URL || 'http://localhost:4000',
  NEXUS_SUPPORT_BEARER_TOKEN: process.env.SUPPORT_BEARER_TOKEN || '',
};

if (!CONFIG.NEXUS_SUPPORT_BEARER_TOKEN) {
  console.warn('[BOOT] SUPPORT_BEARER_TOKEN not set — provisioning calls to NEXUS will 401. ' +
    'Set it in the environment (see ~/Documents/Dev/nexus/api/.env for the lab value).');
}

// ── This server's own long-term X25519 identity ─────────────────────────────
// Same pattern as ozlock/server's serverEcdhKeyPair (server.js:396-417) —
// persisted to disk so it survives a restart; a rotating server key would
// silently break every already-provisioned board's ability to seal to it,
// with no signal why.
const SERVER_ECDH_KEY_PATH = path.join(__dirname, '.server_ecdh_key.json');
let serverKeyPair = null; // { privateKey: KeyObject, publicKeyHex: string }

function x25519PublicKeyFromHex(hex) {
  return crypto.createPublicKey({
    key: { kty: 'OKP', crv: 'X25519', x: Buffer.from(hex, 'hex').toString('base64url') },
    format: 'jwk',
  });
}

function loadOrCreateServerKeyPair() {
  if (fs.existsSync(SERVER_ECDH_KEY_PATH)) {
    const { x, d } = JSON.parse(fs.readFileSync(SERVER_ECDH_KEY_PATH, 'utf8'));
    return {
      privateKey: crypto.createPrivateKey({ key: { kty: 'OKP', crv: 'X25519', x, d }, format: 'jwk' }),
      publicKeyHex: Buffer.from(x, 'base64url').toString('hex'),
    };
  }
  const { publicKey, privateKey } = crypto.generateKeyPairSync('x25519');
  const pubJwk = publicKey.export({ format: 'jwk' });
  const privJwk = privateKey.export({ format: 'jwk' });
  fs.writeFileSync(SERVER_ECDH_KEY_PATH, JSON.stringify({ x: pubJwk.x, d: privJwk.d }), { mode: 0o600 });
  console.log(`[BOOT] generated a new server identity -> ${SERVER_ECDH_KEY_PATH}`);
  return { privateKey, publicKeyHex: Buffer.from(pubJwk.x, 'base64url').toString('hex') };
}

// ── Envelope crypto — mirrors blelock/simlock/ozkit_crypto.h exactly ───────
const OZKIT_ENV_VER = 0x02;
const OZKIT_ENV_HDR = 21; // 1 ver + 8 counter + 12 nonce
const OZKIT_ENV_TAG = 16;
const OZKIT_ENV_MIN = OZKIT_ENV_HDR + OZKIT_ENV_TAG;

function ozkitEnvKey(sharedSecret, boardId, direction) {
  const salt = Buffer.from(boardId + 'ozkit', 'utf8');
  const info = Buffer.from(direction, 'utf8');
  return Buffer.from(crypto.hkdfSync('sha256', sharedSecret, salt, info, 32));
}

function ozkitEnvAad(boardId, counter) {
  const idBuf = Buffer.from(boardId, 'utf8');
  const aad = Buffer.alloc(9 + idBuf.length);
  aad.writeUInt8(OZKIT_ENV_VER, 0);
  aad.writeBigUInt64BE(BigInt(counter), 1);
  idBuf.copy(aad, 9);
  return aad;
}

/** Open a board->server envelope. Returns the plaintext Buffer, or throws. */
function ozkitEnvOpen(key, boardId, env) {
  if (env.length < OZKIT_ENV_MIN || env.readUInt8(0) !== OZKIT_ENV_VER) {
    throw new Error('malformed envelope (bad length or version)');
  }
  const counter = env.readBigUInt64BE(1);
  const nonce = env.subarray(9, 21);
  if (nonce.readBigUInt64BE(4) !== counter) {
    throw new Error('nonce/counter mismatch');
  }
  const ctLen = env.length - OZKIT_ENV_HDR - OZKIT_ENV_TAG;
  const ciphertext = env.subarray(OZKIT_ENV_HDR, OZKIT_ENV_HDR + ctLen);
  const tag = env.subarray(OZKIT_ENV_HDR + ctLen, OZKIT_ENV_HDR + ctLen + OZKIT_ENV_TAG);
  const aad = ozkitEnvAad(boardId, counter);

  const decipher = crypto.createDecipheriv('aes-256-gcm', key, nonce);
  decipher.setAAD(aad);
  decipher.setAuthTag(tag);
  const pt = Buffer.concat([decipher.update(ciphertext), decipher.final()]);
  return { plaintext: pt, counter };
}

// ── MySQL ────────────────────────────────────────────────────────────────────
let pool = null;

async function initDatabase() {
  const admin = await mysql.createConnection({ host: CONFIG.DB.host, user: CONFIG.DB.user, password: CONFIG.DB.password });
  await admin.query(`CREATE DATABASE IF NOT EXISTS \`${CONFIG.DB.database}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`);
  await admin.end();

  pool = mysql.createPool(CONFIG.DB);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS boards (
      mac             VARCHAR(12)  PRIMARY KEY,
      board_id        VARCHAR(32)  NOT NULL,
      board_pub_hex   CHAR(64)     NULL,
      provisioned_at  TIMESTAMP    NULL,
      last_ip         VARCHAR(45)  NULL,
      last_seen_at    TIMESTAMP    NULL,
      last_status     JSON         NULL,
      created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
      updated_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  `);

  console.log(`[BOOT] database '${CONFIG.DB.database}' ready`);
}

function guardDb(res) {
  if (!pool) { res.status(503).json({ ok: false, error: 'database not ready' }); return false; }
  return true;
}

// ── NEXUS client — POST /api/v1/dev/lock-keys ──────────────────────────────
// Development-mode key issuance. Real, built, verified endpoint — see
// ~/Documents/Dev/nexus/api/src/routes/dev.js (read directly before
// designing against it, not just the spec doc). Idempotent per MAC.
async function fetchDevKeypairFromNexus(mac) {
  const res = await fetch(`${CONFIG.NEXUS_BASE_URL}/api/v1/dev/lock-keys`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${CONFIG.NEXUS_SUPPORT_BEARER_TOKEN}`,
    },
    body: JSON.stringify({ mac, app_id: 'ozkit-server' }),
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(`NEXUS /dev/lock-keys ${res.status}: ${body.message || body.code || 'unknown error'}`);
  }
  return body; // { pub, priv }
}

// ── Provisioning a board over its own local REST API ────────────────────────
async function provisionBoard(mac, boardId, ip) {
  console.log(`[PROVISION] ${boardId} (${mac}) at ${ip} — requesting keypair from NEXUS`);
  const { pub, priv } = await fetchDevKeypairFromNexus(mac);

  const body = JSON.stringify({ pub, priv, server_pub: serverKeyPair.publicKeyHex });
  await new Promise((resolve, reject) => {
    const req = http.request(
      { host: ip, port: 80, path: '/api/provision_key', method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) },
        // 5s was too tight — a real provision does 2x X25519 scalar mults
        // plus 4 NVS putString()s (flash writes) on top of WiFi round-trip
        // latency, and cost us a stuck board once already (server timed
        // out, board had already succeeded, board_pub_hex never got saved).
        timeout: 20000 },
      (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => {
          if (res.statusCode !== 200) return reject(new Error(`board /api/provision_key ${res.statusCode}: ${data}`));
          resolve(data);
        });
      }
    );
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.write(body);
    req.end();
  });

  await pool.query(
    `UPDATE boards SET board_pub_hex = ?, provisioned_at = NOW() WHERE mac = ?`,
    [pub, mac]
  );
  console.log(`[PROVISION] ${boardId} (${mac}) — done, telemetry now sealed`);
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
let mqttClient = null;

function mqttConnect() {
  mqttClient = mqtt.connect(CONFIG.MQTT_URL, {
    clientId: `ozkit-server-${Math.random().toString(16).slice(2, 8)}`,
    username: CONFIG.MQTT_USER,
    password: CONFIG.MQTT_PASS,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
    rejectUnauthorized: false, // bench default — no CA bundle pinned yet, matches firmware's setInsecure()
  });

  mqttClient.on('connect', () => {
    console.log('[MQTT] connected');
    mqttClient.subscribe('test/boards/+/#', { qos: 1 });
  });
  mqttClient.on('error', (err) => console.error('[MQTT] error:', err.message));
  mqttClient.on('message', onMqttMessage);
}

async function onMqttMessage(topic, payload) {
  const m = topic.match(/^test\/boards\/([^/]+)\/(status|connection)$/);
  if (!m) return;
  const [, boardId, kind] = m;

  if (kind === 'connection') {
    // Plaintext by firmware design (LWT payload, fixed at connect time —
    // see ozkit_crypto.h's header comment for why this one topic stays
    // unsealed). Just presence tracking.
    try {
      const { status } = JSON.parse(payload.toString('utf8'));
      await pool.query(`UPDATE boards SET last_seen_at = NOW() WHERE board_id = ?`, [boardId]);
      console.log(`[MQTT] ${boardId} connection: ${status}`);
    } catch (_) { /* not fatal — presence tracking is best-effort */ }
    return;
  }

  // kind === 'status'. Two possible shapes on the wire: plaintext JSON
  // (board not yet provisioned — starts with '{') or a sealed envelope
  // (starts with the version byte 0x02). Never guess silently between them.
  const isPlaintext = payload.length > 0 && payload[0] === 0x7b; // '{'
  if (isPlaintext) {
    let data;
    try { data = JSON.parse(payload.toString('utf8')); } catch (e) {
      console.error(`[MQTT] ${boardId}: plaintext status not valid JSON — ${e.message}`);
      return;
    }
    await handleUnprovisionedStatus(boardId, data);
    return;
  }

  await handleSealedStatus(boardId, payload);
}

async function handleUnprovisionedStatus(boardId, data) {
  const mac = data.mac;
  const ip = data.wifi && data.wifi.ip;
  if (!mac || mac.length !== 12) {
    console.warn(`[MQTT] ${boardId}: unprovisioned status missing/malformed mac (${mac}) — cannot provision yet`);
    return;
  }

  const [[row]] = await pool.query(`SELECT provisioned_at FROM boards WHERE mac = ?`, [mac]);
  if (!row) {
    await pool.query(
      `INSERT INTO boards (mac, board_id, last_ip, last_seen_at, last_status) VALUES (?, ?, ?, NOW(), ?)
       ON DUPLICATE KEY UPDATE board_id = VALUES(board_id), last_ip = VALUES(last_ip), last_seen_at = NOW(), last_status = VALUES(last_status)`,
      [mac, boardId, ip, JSON.stringify(data)]
    );
  } else {
    await pool.query(`UPDATE boards SET last_ip = ?, last_seen_at = NOW(), last_status = ? WHERE mac = ?`,
      [ip, JSON.stringify(data), mac]);
  }

  if (row && row.provisioned_at) return; // already provisioned — a stray unsealed publish would be a firmware bug, not ours to fix here
  if (!ip) { console.warn(`[MQTT] ${boardId}: no wifi.ip in status yet — will retry next heartbeat`); return; }

  try {
    await provisionBoard(mac, boardId, ip);
  } catch (err) {
    console.error(`[PROVISION] ${boardId} (${mac}) failed: ${err.message}`);
  }
}

async function handleSealedStatus(boardId, envelope) {
  const [[row]] = await pool.query(`SELECT mac, board_pub_hex FROM boards WHERE board_id = ?`, [boardId]);
  if (!row || !row.board_pub_hex) {
    console.warn(`[MQTT] ${boardId}: got a sealed publish but this server has no pubkey on file for it — cannot unseal`);
    return;
  }

  try {
    const boardPub = x25519PublicKeyFromHex(row.board_pub_hex);
    const sharedSecret = crypto.diffieHellman({ privateKey: serverKeyPair.privateKey, publicKey: boardPub });
    const key = ozkitEnvKey(sharedSecret, boardId, 'ozkit/board->server');
    const { plaintext, counter } = ozkitEnvOpen(key, boardId, envelope);
    const data = JSON.parse(plaintext.toString('utf8'));
    await pool.query(`UPDATE boards SET last_ip = ?, last_seen_at = NOW(), last_status = ? WHERE mac = ?`,
      [data.wifi && data.wifi.ip, JSON.stringify(data), row.mac]);
    console.log(`[MQTT] ${boardId}: unsealed status ok, counter=${counter}`);
  } catch (err) {
    console.error(`[MQTT] ${boardId}: unseal failed — ${err.message}`);
  }
}

// ── REST API ─────────────────────────────────────────────────────────────────
const app = express();
app.use(cors());
app.use(express.json());
const api = express.Router();
app.use('/api', api);

api.get('/boards', async (req, res) => {
  if (!guardDb(res)) return;
  const [rows] = await pool.query(
    `SELECT mac, board_id, provisioned_at, last_ip, last_seen_at, last_status FROM boards ORDER BY last_seen_at DESC`
  );
  res.json({ ok: true, boards: rows });
});

api.get('/boards/:id', async (req, res) => {
  if (!guardDb(res)) return;
  const [[row]] = await pool.query(
    `SELECT mac, board_id, board_pub_hex, provisioned_at, last_ip, last_seen_at, last_status FROM boards WHERE board_id = ? OR mac = ?`,
    [req.params.id, req.params.id]
  );
  if (!row) return res.status(404).json({ ok: false, error: 'unknown board' });
  res.json({ ok: true, board: row });
});

api.get('/version', (req, res) => {
  res.json({ ok: true, service: 'ozkit-server', server_pub: serverKeyPair.publicKeyHex });
});

// ── Startup ──────────────────────────────────────────────────────────────────
async function main() {
  serverKeyPair = loadOrCreateServerKeyPair();
  console.log(`[BOOT] server identity ${serverKeyPair.publicKeyHex.slice(0, 12)}…`);
  await initDatabase();
  mqttConnect();
  app.listen(CONFIG.PORT, () => console.log(`[BOOT] ozkit-server listening on :${CONFIG.PORT}`));
}

main().catch((err) => {
  console.error('[BOOT] fatal:', err);
  process.exit(1);
});
