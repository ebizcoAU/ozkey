/*
 * ============================================================================
 *  OZLOCKSERV — OZLOCK Rendezvous Directory (lab deployment)
 *  ---------------------------------------------------------------------------
 *  Role     : Market-A personal cloud, per ozkey-05: MQTT rendezvous + mini
 *             directory that holds doorlock <-> owner-account pairing.
 *  Port     : 4400  (REST base /ozlockserv/api)
 *  Broker   : TalkPOS Mosquitto @ mqtt://10.1.1.42:1883 (lab stand-in for EMQX;
 *             2026-09-01: moved from 10.1.1.20, old host went unreachable)
 *  Database : MySQL (localhost / ozlock)
 *
 *  Responsibilities (ozkey-04 §6, ozkey-05 §6)
 *    1. Mint single-use enrollment tokens (BANOI "Add Doorlock" begins here)
 *    2. Enroll locks: verify token, bind device -> site/owner, issue broker
 *       credentials, ack on the command topic
 *    3. Relay app-sealed Tuya 55 AA DPID envelopes (grant/revoke), queue
 *       them, flush on the lock's wake (ozkie/<site>/locks/<id>/heartbeat,
 *       legacy ozkey/<site>/... also accepted during the S16 migration —
 *       ozkey-18 §S16) — the server never builds a frame or sees a PIN/RFID
 *       (ozkey-13, S4 cutover 2026-08-08)
 *
 *  NOT a responsibility, deliberately — DOOR EVENTS ARE NEVER INGESTED.
 *    Removed 2026-07-31 (operator decision, XF-48 §9.4). This server is the
 *    HOSTED RELAY: it is run by us, for other people, over their doors. The
 *    Sovereign Edge whitepaper v3 §4.1 data inventory says we hold connection
 *    metadata (7 d) and security events (90 d) and "explicitly not which lock
 *    opened, when, or by whom" — and `lock_logs` was exactly that. It is gone:
 *    no table, no `log` subscription, no ingest, no query endpoint.
 *
 *    Same rule XF-47 Ask 7 set for `grants.raw_value` — and as of the ozkey-13
 *    S3/S4 cutover (2026-08-08, XF-69) it's gone the same way: no column, no
 *    server-side frame builder (`buildCredentialFrame`/`buildDeleteFrame`
 *    deleted). The app seals DPID 21-24 client-side; this server relays the
 *    envelope opaque. Door events live on OZPMSSERV / OZKEYSERV — the
 *    OPERATOR'S OWN servers, over their own doors, which is not a sovereignty
 *    breach. The XF-47 §8(b) log work (seq / recorded_at / sync_batch /
 *    window_from / window_to) targets those, not this.
 *
 *    DO NOT REINSTATE as a flag or a shorter retention. The guarantee is only
 *    credible because the hosted build cannot do it at all.
 *
 *  Lab simplifications / PRODUCTION READINESS — NOT DONE YET (ozkey-05 §10
 *  migration steps 3-5 pending). None of these are lab-only quirks to shrug
 *  off; each is a real gap that must close before any non-lab deployment:
 *    - single seeded owner + site ('lab'); REST is unauthenticated
 *    - **Broker ACLs are not configured or enforced.** Verified live
 *      2026-08-08 (ozkey-15 §8.1, S8/S9): `mosquitto_pub` with a fabricated
 *      username and wrong password still published successfully — anyone who
 *      can reach the broker can publish/subscribe to ANY topic, including
 *      another device's command topic or another app's members/* topic.
 *      This is a bigger deal after ozkey-15's S8/S9 (app-to-app relay):
 *      that spec's own trust model states plainly "No authentication
 *      changes — app_id and MQTT ACLs already handle routing" — which
 *      assumes ACLs exist. They don't yet.
 *      **CORRECTED 2026-08-12 (ozkey-23 §10.2a) — "the wiring is there" was
 *      true of locks only, not the fleet.** Locks mint + store + ack broker
 *      credentials (`locks.broker_username`/`broker_secret`) and firmware now
 *      actually presents them (`doorlock-1.57`; every prior firmware minted
 *      and stored them, then connected anonymously anyway — found and fixed
 *      firmware-side). Bridges had NO mint at all until this same pass closed
 *      it: `bridges.broker_username`/`broker_secret`, static per-device,
 *      same model as locks (minted on first presence,
 *      `handleBridgePresence()`). Apps (BANOI) have the identical gap — no
 *      mint exists, confirmed by ftpos (`XFtposDecisions-96.md` §4/§6) — but
 *      get a DIFFERENT model, operator's call 2026-08-12: REST-authenticated,
 *      short-lived JWT issuance (`POST /auth/challenge` + `/auth/token`),
 *      not a static secret. `docs/ozkey-24.md` — designed, corrected by
 *      firmware (§9: `app_id` IS the app's X25519 public key, hex-encoded —
 *      NOT a bearer string beside a separate identity; see
 *      `blelock/common/ozcrypto.h:414/260`), and BUILT + live-verified
 *      against the real DB same day. No `apps` table: the challenge/token
 *      flow reads `locks.app_id` directly, which has been an app
 *      public-key store since before anyone named it that. Kept as its own
 *      doc/section precisely so it never blocked the bridge fix, which
 *      shipped first. **Hold on enabling EMQX ACLs until all three paths —
 *      locks, bridges, apps — are deployed**
 *      (operator instruction 2026-08-12): enabling ACLs before every
 *      principal can authenticate drops the whole fleet at once, instantly
 *      (ozkey-23 §10.1a).
 *      **Configuring real ACLs is a FUTURE task, deliberately not done now**
 *      (operator instruction 2026-08-08) — noted here so it isn't
 *      rediscovered from scratch, not as something to act on yet. Also note
 *      production is planned to run EMQX, not this lab's Mosquitto (see
 *      "Broker" above) — so this is a spec/requirements note for whoever
 *      provisions that broker, not a config file that exists in this repo
 *      to edit.
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
const fs = require('fs');
const path = require('path');
const jwt = require('jsonwebtoken'); // ozkey-24: app broker-auth JWT issuance

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
  // Override to spin a verification instance beside the watcher-run :4400 one.
  HTTP_PORT: Number(process.env.OZLOCK_HTTP_PORT) || 4400,
  SERVER_IP: process.env.OZLOCK_SERVER_IP || detectLanIp(),
  SITE_ID: 'lab', // single-tenant lab deployment (ozkey-05 §1.3)
  // ozkey-24: HMAC secret ozlockserv signs app broker-auth JWTs with; EMQX's
  // JWT auth plugin must hold the SAME secret to verify them. LAB DEFAULT
  // ONLY — production must set OZLOCK_JWT_SECRET (same posture as the
  // 'labwifi-secret' Wi-Fi password in buildProvisionPayload below: a
  // deliberately loud placeholder, not a value anyone should ship with).
  JWT_SIGNING_SECRET: process.env.OZLOCK_JWT_SECRET || 'lab-only-INSECURE-jwt-secret-CHANGE-IN-PRODUCTION',
  AUTH_NONCE_TTL_MS: 60_000, // ozkey-24 §3.2: challenge nonce lifetime
  AUTH_JWT_TTL_S: 3600, // ozkey-24 §3.2: issued JWT lifetime
  // ozkey-P2: how long POST /bridges/:id/reset waits for the bridge's own
  // presence signal before giving up and reporting 'unknown'. factoryReset()
  // is called synchronously right after the presence publish in firmware —
  // no async delay on the bridge's side — so this is round-trip + broker
  // latency budget, not a real processing wait.
  BRIDGE_RESET_TIMEOUT_MS: 5000,
  // ozkey-41 §5/§6: doorlock-1.96/bridge32-1.40 publish the reset outcome to
  // `locks/<id>/presence` synchronously, right before the wipe (or on
  // refusal) — same latency shape as the bridge's own signal, so the same
  // bound applies.
  LOCK_RESET_TIMEOUT_MS: 5000,
  // ozkey-27 §9 (firmware, 2026-08-13): the ozkey-20 §23.1 utc push below
  // fired only on the presence 'online' transition — a server restart while
  // a bridge was already online, a missed presence message, or a reconnect
  // not classified as fresh 'online' all permanently lose that bridge's
  // time source (no NTP fallback since bridge32-1.36). This is the cadence
  // `refreshOnlineBridgeTimePushes()` re-pushes utc to every online bridge
  // on (XF-138 P5: previously piggybacked on the now-retired Thread
  // liveness heartbeat; now its own explicit periodic sweep).
  UTC_PUSH_REFRESH_MS: 10 * 60 * 1000,
  // XF-125 P1: audit_log is the file header's "security events" class and was
  // documented (2026-07-31) as retained per the Sovereign Edge whitepaper's
  // 90-day target — but nothing ever enforced it; grepped clean, no cron, no
  // age-based DELETE anywhere. This is that enforcement, finally wired up.
  AUDIT_LOG_RETENTION_MS: 90 * 24 * 3600 * 1000,
  AUDIT_LOG_PURGE_INTERVAL_MS: 24 * 3600 * 1000,
  // nexus-14 ask #3: DELETE /locks/:id tells Nexus's lock_registry to
  // tombstone the MAC, so a decommissioned lock's stored pubkey doesn't
  // silently go on serving as if it were still live. No baked-in default
  // for the key — it is Nexus's secret, not ozlock's, and must never be
  // committed into this repo; unset means the call is skipped (logged), not
  // a hard failure, since Nexus notification is best-effort from here.
  NEXUS_URL: process.env.OZLOCK_NEXUS_URL || 'http://localhost:4000',
  NEXUS_SERVER_KEY: process.env.OZLOCK_SERVER_API_KEY || null,
  DB: {
    host: 'localhost',
    user: 'root',
    password: 'Cableman',
    database: 'ozlock',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL: 'mqtt://10.1.1.42:1883',
  BROKER: {
    host: '10.1.1.42',
    tcp_port: 1883,
    ws_port: 9001,
    ws_path: '/mqtt',
  },
  // ozkey-04 §9 topic scheme (site-prefixed, device-scoped, room-free).
  // Site-pinned (NOT wildcard) so multiple servers can share one broker —
  // OZLODGESERV (site 'hotel', ozkey-07) publishes device-scoped on the same
  // topic root; each server must only consume its own site.
  //
  // S16 (ozkey-18, 2026-08-10, TRADEMARK — not a style preference): OZKEY is
  // already someone else's product, so the wire-visible topic root moves to
  // `ozkie/`. This overrides the choice S12 made to leave the topic root
  // alone — S12 was right that a shared-protocol rename is a bigger action
  // than a product rename, S16 is that bigger action, done deliberately.
  // Migration shape (do not skip): publishers use `ozkie/` ONLY;
  // subscribers accept BOTH `ozkie/` and legacy `ozkey/` so update order
  // across servers/bridges/locks never matters. Drop the legacy *_LEGACY
  // subscriptions and the `ozk(?:ey|ie)` regex alternation once every
  // component is confirmed on `ozkie/` — that is a follow-up, not this one.
  // NOT touched: ozcrypto.h's `"ozkey/app->lock"` etc — those are HKDF
  // domain-separation strings, not topic names; wrong file for this change,
  // wrong kind of string, see ozkey-18 S16 for why they must stay put.
  SUB_ENROLL: 'ozkie/lab/locks/+/enroll',
  SUB_ENROLL_LEGACY: 'ozkey/lab/locks/+/enroll',
  SUB_HEARTBEAT: 'ozkie/lab/locks/+/heartbeat',
  SUB_HEARTBEAT_LEGACY: 'ozkey/lab/locks/+/heartbeat',
  // SUB_LOG removed 2026-07-31 — see the header. We do not subscribe to
  // `<root>/<site>/locks/+/log` at all, so door events are never delivered
  // to this process. Locks may still publish there; nothing here consumes it.
  //
  // S8/S9 (ozkey-15 §3, async orchestrated removal): app-to-app messages
  // between banoi2 (member) and banoi1 (admin). The topic is already
  // addressed to the specific recipient's device_id, so the MQTT broker
  // delivers publish -> subscribe directly between the two apps with no
  // help from this process — same pattern as any other MQTT topic both a
  // publisher and a subscriber happen to share. ozlockserv subscribes only
  // to observe/log for visibility (ozkey-15 §2: "pure relay, no state, no
  // persistence"), not to republish.
  SUB_MEMBER_REQUEST_REMOVE: 'ozkie/lab/members/+/request_remove',
  SUB_MEMBER_REQUEST_REMOVE_LEGACY: 'ozkey/lab/members/+/request_remove',
  SUB_MEMBER_ACK_REMOVE: 'ozkie/lab/members/+/ack_remove',
  SUB_MEMBER_ACK_REMOVE_LEGACY: 'ozkey/lab/members/+/ack_remove',
  // V1 (ozkey-17 §6, ozkey-14.md "Update 2026-08-09 late"): the lock->app
  // uplink channel, published under the LOCK'S OWN topic (bridge32-1.9
  // relays it there, not under the bridge's topic) — but this is NOT
  // covered by SUB_ENROLL/SUB_HEARTBEAT above despite both being
  // `locks/+/...`: MQTT subscriptions match the full topic string, so
  // subscribing to `locks/+/heartbeat` does not receive `locks/+/uplink`.
  // Verified live 2026-08-10: a synthetic publish to this topic produced
  // zero server-side activity before this subscription was added — the
  // "server already subscribes locks/+/... wildcards" assumption in
  // ozkey-14.md was wrong. This is the real V1 finding.
  SUB_UPLINK: 'ozkie/lab/locks/+/uplink',
  SUB_UPLINK_LEGACY: 'ozkey/lab/locks/+/uplink',
  // ozkey-20 R1 (LWT presence, not built by firmware yet): retained
  // {"state":"online"|"offline","reason":"lwt"} on connect/disconnect, for
  // both locks and bridges — spec names one topic shape,
  // `{locks|bridges}/<id>/presence`, so two wildcard subs cover it.
  SUB_PRESENCE_LOCKS: 'ozkie/lab/locks/+/presence',
  SUB_PRESENCE_LOCKS_LEGACY: 'ozkey/lab/locks/+/presence',
  SUB_PRESENCE_BRIDGES: 'ozkie/lab/bridges/+/presence',
  SUB_PRESENCE_BRIDGES_LEGACY: 'ozkey/lab/bridges/+/presence',
  // XF-144 (2026-09-01): bridge-side targeted scan, proposed by operator,
  // "no objection" from firmware on this shape. Deliberately NOT folded
  // into `.../uplink` — that topic is lock-scoped by every existing
  // consumer, and a scan result is the bridge reporting on itself, not
  // something a lock said. Structurally the same shape as
  // `.../bridges/<id>/presence`, one more thing a bridge publishes about
  // itself. The request direction needs no new topic — it rides the
  // existing `topicBridgeCommand()` below, same as pairctl/time/unlock.
  SUB_BRIDGE_SCAN_RESULT: 'ozkie/lab/bridges/+/scan_result',
  // XF-144 Phase 2 (2026-09-02, PM-approved): a Thread-detecting bridge
  // reports a DIFFERENT (dead) peer bridge's id in the payload's `peer`
  // field — the wildcard here is the REPORTING bridge, not the subject.
  // Same "bridge reporting on itself/its own observation" shape as
  // scan_result, one more thing a bridge publishes unprompted.
  SUB_PEER_STATUS: 'ozkie/lab/bridges/+/peer_status',
  topicCommand: (site, deviceId) => `ozkie/${site}/locks/${deviceId}/command`,
  // A bridged lock has no MQTT client of its own — the bridge is its gateway
  // and subscribes to its OWN topic, then demuxes onto its BLE link by
  // `target`. ozkey-11 §3.
  topicBridgeCommand: (site, bridgeId) => `ozkie/${site}/bridges/${bridgeId}/command`,
  // ozkey-33: site-wide retained clock, for Wi-Fi-direct locks — they have
  // no bridge to relay ozkey-27 §9's `utc` push and NTP is blocked on this
  // network. Deliberately NOT under `.../command`: a retained payload on a
  // command topic is redelivered as a replayed action on every reconnect,
  // where here the whole point is that a lock waking up gets the retained
  // value immediately, for free, on subscription alone.
  topicTime: (site) => `ozkie/${site}/time`,
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

// S4 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08: the server
// building a credential frame at all was the plaintext-storage breach the
// whitepaper named. credentialValueBytes()/buildCredentialFrame()/
// buildDeleteFrame() are gone — the app always sends a pre-sealed
// envelope_hex now (A1-A5, ftpos shipped), the server only relays it opaque.
// See migrations/S3_drop_raw_value.sql for the paired schema cut.

// S10 (ozkey-17 §4/§9, XF-72..78, XF-120 §2 step 2): buildUnlockFrame()
// removed 2026-08-21 — it built DP 1, an invented DP with no authentication
// (XF-120 §1.2b: on a lock whose profile happened to carry DP 1, ANY sender
// able to reach the command topic could open the door, no bond, no key
// check). Deletable once both REST-unlock callers ship sealing: `unlock()`
// (XF-120 §4/§8, bench-verified DP 76 opening a real lock) and
// `assistedUnlock()`, the one caller that fix missed (XF-120 §9, caught
// while scoping this exact deletion) — both landed `ftpos 34bc1e6`. This
// was "the last server-composed Tuya frame in the system" (ozkey-17 §4);
// deleting it makes `envelope_hex` unconditionally required below.

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

/** ozkey-23 §10.2a: mint-once, same shape as the lock's `handleEnroll()`
 *  (`brokerUsername = id`, `brokerSecret = makeSecret(16, 'ozl_')`). Returns
 *  {broker_username, broker_secret, minted: bool} — `minted` tells the
 *  caller whether this is the first time (worth pushing/returning) or a
 *  repeat lookup of an already-provisioned principal. */
async function getOrMintBridgeCredentials(bridgeId) {
  const [[row]] = await pool.query('SELECT broker_username, broker_secret FROM bridges WHERE id = ?', [
    bridgeId,
  ]);
  if (row && row.broker_secret) return { ...row, minted: false };
  const broker_username = bridgeId;
  const broker_secret = makeSecret(16, 'ozl_');
  await pool.query(
    `INSERT INTO bridges (id, broker_username, broker_secret) VALUES (?, ?, ?)
       ON DUPLICATE KEY UPDATE broker_username = VALUES(broker_username),
         broker_secret = VALUES(broker_secret)`,
    [bridgeId, broker_username, broker_secret]
  );
  return { broker_username, broker_secret, minted: true };
}

/* ---------------------------------------------------------------------------
 * ozkey-24 — App broker auth: REST-authenticated JWT over a dedicated
 * per-app X25519 identity (approved 2026-08-12: dedicated keypair, ECDH
 * challenge, POST /apps/register, short-lived JWT).
 * ------------------------------------------------------------------------- */

/** Wire format for every X25519 public key in this flow: 64 lowercase hex
 *  chars (32 raw bytes) — matches `locks`/`bridges` hex-secret conventions
 *  elsewhere in this file. Converted to a Node KeyObject via JWK import
 *  (OKP/X25519), the only clean path for RAW key bytes in Node's crypto —
 *  SPKI/DER would need manual ASN.1 wrapping for no benefit here. */
function x25519PublicKeyFromHex(hex) {
  return crypto.createPublicKey({
    key: { kty: 'OKP', crv: 'X25519', x: Buffer.from(hex, 'hex').toString('base64url') },
    format: 'jwk',
  });
}

// ozkey-24 §3.2/§4.3(a): the server's own LONG-TERM X25519 identity for the
// ECDH challenge — every registered app computes the same shared secret
// against this one static public key. Persisted to disk so it survives a
// restart (a rotating server key would silently invalidate every app's
// ability to prove itself, with no signal why). File holds JWK `x`/`d` only
// — same "lab shortcut, loudly flagged" posture as JWT_SIGNING_SECRET
// above; production should manage this via a real secrets store, not a
// plaintext file beside the code.
const SERVER_ECDH_KEY_PATH = path.join(__dirname, '.server_ecdh_key.json');
let serverEcdhKeyPair = null; // set by loadOrCreateServerEcdhKeyPair() at boot

function loadOrCreateServerEcdhKeyPair() {
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
  fs.writeFileSync(
    SERVER_ECDH_KEY_PATH,
    JSON.stringify({ x: pubJwk.x, d: privJwk.d }),
    { mode: 0o600 }
  );
  logEvent('info', `ozkey-24: generated a new server ECDH identity -> ${SERVER_ECDH_KEY_PATH}`);
  return { privateKey, publicKeyHex: Buffer.from(pubJwk.x, 'base64url').toString('hex') };
}

// ozkey-24 §3.2: single-use, short-lived challenge nonces. In-memory by
// design, not a DB table — a nonce that doesn't survive a restart is
// correct behaviour (the app just requests a fresh one), and durability
// here would only mean a stale nonce outliving a crash.
const authNonces = new Map(); // nonce(hex) -> { appId, expiresAt }

function pruneAuthNonces() {
  const now = Date.now();
  for (const [n, v] of authNonces) if (v.expiresAt < now) authNonces.delete(n);
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

  // bridge_id = the bridge gateway a bridged (BLE-native) lock is reached
  // through (ozkey-11 §3). NULL = the lock is its own MQTT client (direct
  // Wi-Fi, modes 2b/3/4). Only the app learns this — it paired the lock to
  // the bridge over BLE at commissioning — so it must tell us at /pairings
  // or the lock is unreachable.
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
  // whose real transport changed stayed misreported in the app permanently
  // and every remote unlock was refused. `wifi` | `ble`, NULL until the
  // device reports (XF-138 P5: `thread` retired along with Thread support).
  const [[{ hasTransport }]] = await pool.query(
    `SELECT COUNT(*) AS hasTransport FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'transport'`,
    [CONFIG.DB.database]
  );
  if (!hasTransport)
    await pool.query('ALTER TABLE locks ADD COLUMN transport VARCHAR(16) NULL AFTER power_profile');

  // ozkey-20 R5 (liveness/health/fault-attribution, 2026-08-10): observed
  // state, replacing the likelyOnline() inference. `presence`/
  // `presence_reason`/`presence_at` are R6's fault-attribution OUTPUT
  // (computeFaultAttribution() below writes them), not a raw signal —
  // `presence_reason` holds one of the R6 table's verdict strings
  // (bridge_offline, lock_unreachable, battery_low, ...). Inputs feeding it
  // (R1 presence topics, R4 heartbeat health fields) don't fully exist on
  // the wire yet — this is the receiving side built ahead of the sender,
  // same pattern as SUB_UPLINK before V1 shipped, so it lights up with zero
  // further server change once firmware catches up.
  const [[{ hasPresence }]] = await pool.query(
    `SELECT COUNT(*) AS hasPresence FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'presence'`,
    [CONFIG.DB.database]
  );
  if (!hasPresence)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN presence         ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
        ADD COLUMN presence_reason  VARCHAR(32) NULL,
        ADD COLUMN presence_at      DATETIME NULL,
        ADD COLUMN battery_pct      TINYINT NULL,
        ADD COLUMN pending_uplinks  SMALLINT NOT NULL DEFAULT 0,
        ADD COLUMN roster_epoch     INT UNSIGNED NOT NULL DEFAULT 0
    `);

  // XF-146 (2026-09-01, found live on LockA — real bug, not theoretical):
  // `presence` used to serve two purposes that turned out to conflict —
  // the RAW signal `handleLockPresence()` writes directly, and the DERIVED
  // verdict `recomputeAndStorePresence()` computes from it — sharing one
  // column. A bridge's own brief MQTT reconnect blip (its LWT firing and
  // clearing seconds later, nothing to do with the actual BLE link to the
  // lock) triggered two re-verdicts back to back: the first correctly read
  // `bridge_offline` and wrote `presence='offline'`; the second, once the
  // bridge recovered, re-read `lock.presence` for its "is there still a
  // direct signal" check — and got back its OWN prior write, misread as a
  // fresh direct report, permanently re-confirming `lock_unreachable` with
  // no path back except a brand-new physical reconnect (which, confirmed
  // on firmware's own serial log, never even happened — the real BLE link
  // was healthy throughout). `direct_presence` is the fix: the raw signal
  // now lives in its own column, written ONLY by `handleLockPresence()`,
  // never touched by the derivation step — so a transient bridge-level
  // blip can no longer permanently poison what the lock itself last
  // reported. Backfilled from `presence` so no lock reads as spuriously
  // unknown right after this migration runs.
  const [[{ hasDirectPresence }]] = await pool.query(
    `SELECT COUNT(*) AS hasDirectPresence FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'direct_presence'`,
    [CONFIG.DB.database]
  );
  if (!hasDirectPresence) {
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN direct_presence ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown'
    `);
    await pool.query(`UPDATE locks SET direct_presence = presence`);
  }

  // XF-144 (2026-09-01, PM directive): multi-bridge failover. Deliberately
  // NOT replacing `locks.bridge_id` — per this session's own recommendation
  // (agreed by PM): every reachability/queue/caps code path already keys
  // off that single column, tested and working live. `bridge_id` stays the
  // denormalized "currently active" pointer; this table is the additive
  // ranked-candidate list failover reads from. Composite PK (lock_id,
  // bridge_id) — a lock can appear once per bridge it's ever been paired
  // to, priority 0 = current/primary, higher = older/fallback.
  const [[{ hasLockBridges }]] = await pool.query(
    `SELECT COUNT(*) AS hasLockBridges FROM information_schema.tables
      WHERE table_schema = ? AND table_name = 'lock_bridges'`,
    [CONFIG.DB.database]
  );
  if (!hasLockBridges) {
    await pool.query(`
      CREATE TABLE lock_bridges (
        lock_id                      VARCHAR(64) NOT NULL,
        bridge_id                    VARCHAR(64) NOT NULL,
        priority                     SMALLINT NOT NULL DEFAULT 0,
        last_confirmed_reachable_at  DATETIME NULL,
        PRIMARY KEY (lock_id, bridge_id),
        INDEX idx_lock_priority (lock_id, priority)
      ) ENGINE=InnoDB
    `);
    // Non-destructive backfill (XF-144's own migration requirement):
    // every lock's CURRENT bridge_id becomes its priority-0 row — existing
    // single-bridge assignments are represented exactly as they are today,
    // nothing is inferred or guessed for locks that have never had a
    // second bridge.
    await pool.query(`
      INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
      SELECT id, bridge_id, 0, presence_at FROM locks WHERE bridge_id IS NOT NULL
    `);
  }

  // last_mech_result/last_mech_at are NOT in ozkey-20's own R5 schema block,
  // even though R6's attribution table references last_mech_result — a real
  // gap between R5 and R6 as specified. Filling it rather than silently
  // working around it: these store R4's heartbeat field once it exists.
  // Guarded separately from the block above: an earlier restart could have
  // already added `presence` before this block existed, which would leave
  // this guard permanently false if it shared the `hasPresence` flag.
  const [[{ hasMechResult }]] = await pool.query(
    `SELECT COUNT(*) AS hasMechResult FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'last_mech_result'`,
    [CONFIG.DB.database]
  );
  if (!hasMechResult)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN last_mech_result VARCHAR(16) NULL,
        ADD COLUMN last_mech_at     DATETIME NULL
    `);

  // ozkey-20 §5a (added 2026-08-11 on firmware review) — the DL MCU link was
  // missing from the whole model. mcu_link_up is the R6 verdict input
  // (mcu_link_down ranks above battery_low); mcu_last_frame_s is kept for
  // observability only. Own guard, same reason as hasMechResult above — do
  // not fold into an existing flag.
  const [[{ hasMcuLink }]] = await pool.query(
    `SELECT COUNT(*) AS hasMcuLink FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'mcu_link_up'`,
    [CONFIG.DB.database]
  );
  if (!hasMcuLink)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN mcu_link_up      TINYINT(1) NULL,
        ADD COLUMN mcu_last_frame_s SMALLINT NULL
    `);

  // XF-119 §6/§9.3 ask 2: the RAW `reason` off the most recent
  // `locks/<id>/presence` message, distinct from `presence_reason` (which is
  // computeFaultAttribution()'s own derived R6 vocabulary and does not
  // include `factory_reset` at all — confirmed while building this, see
  // handleLockPresence()). Lets DELETE /locks/:id answer "has this lock
  // already told us it reset?" from state already held, instead of opening
  // a reset-wait window nothing can ever close for a lock that reset itself
  // (BOOT hold, DL-MCU button) with the app never involved. Cleared to NULL
  // by the same handler whenever a message carries no `reason` (the
  // canonical `online` message per ozpresence.h) — so a stale factory_reset
  // cannot outlive the lock coming back, same non-staleness property XF-119
  // §6 already relies on for the retained MQTT value itself.
  const [[{ hasLastResetReason }]] = await pool.query(
    `SELECT COUNT(*) AS hasLastResetReason FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'last_reset_reason'`,
    [CONFIG.DB.database]
  );
  if (!hasLastResetReason)
    await pool.query('ALTER TABLE locks ADD COLUMN last_reset_reason VARCHAR(32) NULL');

  // XF-122 §5 ask 3: there is no generic Tuya DP map — DP numbers mean
  // different things per product (DP 76 is `unlock_ble` on Luona,
  // `fill_light` on Tuya's own Wi-Fi-Lock-Pro standard). `profile` names
  // which pinned DP map the lock's firmware build resolves verbs against
  // (`tuya-luona-ds013-t3`, …); `tuya_pid` is the lock's own MCU-reported
  // product id, used to detect a build/PID mismatch. Both ride the enroll
  // payload (`handleEnroll()`) alongside the existing fw/caps/verbs fields.
  // The app needs them to survive its own restart without re-pairing —
  // it shows "Detected: <model>" from `tuya_pid` and must not have to talk
  // to the lock again just to redraw that screen (XF-122 §7).
  const [[{ hasTuyaPid }]] = await pool.query(
    `SELECT COUNT(*) AS hasTuyaPid FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'tuya_pid'`,
    [CONFIG.DB.database]
  );
  if (!hasTuyaPid)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN tuya_pid VARCHAR(32) NULL,
        ADD COLUMN profile  VARCHAR(64) NULL
    `);

  // XF-125 P0 (server half): the lock's audit ring is pull-only (F2/F9) —
  // it keeps every event and waits for `query_events`, which is a BLE-only
  // exchange the server never sees (confirmed by reading
  // ozdoorlock_core.h:6103-6161 — the response goes out over
  // ozNotifySealedTo(), not MQTT). Nothing anywhere checks that a reader
  // still exists, and an app's MQTT session can die silently while events
  // pile up unread. These three columns are what makes that detectable:
  //   seq_highwater      last seq the lock has WRITTEN (its hb["seq_highwater"])
  //   dropped_before_seq oldest seq rotation destroyed (its hb["dropped_before_seq"])
  //   last_pulled_seq     highest seq any app has CONFIRMED pulling (ours —
  //                       PATCH /locks/:id `last_pulled_seq`, since there's
  //                       no wire signal to observe: query_events never
  //                       touches MQTT at all)
  // A lock is at risk exactly when dropped_before_seq has passed
  // last_pulled_seq — records were destroyed that nobody ever confirmed
  // reading. See eventsAtRisk() below.
  const [[{ hasSeqHighwater }]] = await pool.query(
    `SELECT COUNT(*) AS hasSeqHighwater FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'seq_highwater'`,
    [CONFIG.DB.database]
  );
  if (!hasSeqHighwater)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN seq_highwater      INT UNSIGNED NULL,
        ADD COLUMN dropped_before_seq INT UNSIGNED NULL,
        ADD COLUMN last_pulled_seq    INT UNSIGNED NULL
    `);

  // XF-127: the lock has reported `has_doorbell` on every heartbeat since
  // XF-107, derived from its own DP map (`tuya-luona-ds013-t3` selects DP
  // 53) — not a guess. Server never stored or served it, so the app (which
  // has no other source and defaults to "no doorbell" on purpose — a false
  // positive strands someone at the door, a false negative only costs a
  // step) could never offer a remote wake for a Wi-Fi lock that actually
  // has one. Blocked verifying XF-126's safety fix, which needs a wake path
  // to even reach the assisted-unlock gate under test.
  const [[{ hasDoorbellCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasDoorbellCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'has_doorbell'`,
    [CONFIG.DB.database]
  );
  if (!hasDoorbellCol)
    await pool.query('ALTER TABLE locks ADD COLUMN has_doorbell TINYINT(1) NULL');

  // XF-127 §8 ask 2 (the audit) turned this one up too, same shape as
  // has_doorbell: already on the wire for Thread locks (bridge32's verbatim
  // beacon relay), never ingested. `g_profileMismatch` is set when the Tuya
  // 0x08 DP census finds hardware that disagrees with the pinned profile —
  // exactly the "DP 76 means unlock on Luona, a lamp on Tuya-standard"
  // class of danger XF-122 exists to prevent, so it's worth closing
  // alongside has_doorbell rather than only reporting it.
  const [[{ hasProfileMismatchCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasProfileMismatchCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'profile_mismatch'`,
    [CONFIG.DB.database]
  );
  if (!hasProfileMismatchCol)
    await pool.query('ALTER TABLE locks ADD COLUMN profile_mismatch TINYINT(1) NULL');

  // XF-134: `bonds` is a device fact (count of entries in the lock's own
  // NVS bond table), already live on the wire for Thread locks today —
  // same verbatim-beacon-relay-onto-heartbeat mechanism as
  // seq_highwater/has_doorbell/profile_mismatch, just never ingested.
  // DIAGNOSTIC ONLY, per XF-133's own reply: a count has no per-member
  // identity, so it can never answer "does member X hold a bond" — that
  // still needs the membership roster XF-129 §10 established doesn't
  // exist. This is "how many keys exist on this door right now," useful
  // for an ops/support view, never a security decision input.
  const [[{ hasBondsCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasBondsCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'bonds'`,
    [CONFIG.DB.database]
  );
  if (!hasBondsCol) await pool.query('ALTER TABLE locks ADD COLUMN bonds TINYINT UNSIGNED NULL');

  await pool.query(`
    CREATE TABLE IF NOT EXISTS bridges_presence (
      bridge_id   VARCHAR(64) PRIMARY KEY,
      presence    ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
      presence_at DATETIME NULL
    ) ENGINE=InnoDB`);

  // ozkey-23 §10.1a/10.2a: bridges were structurally unauthenticatable — no
  // column existed anywhere to hold broker credentials for one, so firmware
  // (bridge32.ino) had nothing to present even after the lock-side bug was
  // fixed. This table exists ONLY to close that gap (credential storage);
  // it is deliberately not a bridge "registry" — (AZ)'s factory-reset route
  // still needs no row, and bridge identity/liveness stay owned by
  // bridges_presence/the Thread-liveness table, not here. A row is created
  // the first time a bridge is seen online (handleBridgePresence()), the
  // same "device_id IS the bearer handle" trust model locks already use.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS bridges (
      id              VARCHAR(64) PRIMARY KEY,
      broker_username VARCHAR(64),
      broker_secret   VARCHAR(64),
      first_seen_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // ozkey-23 §10.1a/10.2a, XF-96 §4/§6: BANOI has the same gap bridges had —
  // ftpos confirmed the app connects with client-id only, no username/
  // password, because no server-side mint exists for it either.
  // ozkey-24, CORRECTED 2026-08-12 by firmware (§9.2, verified against
  // blelock/common/ozcrypto.h before accepting): `app_id` is not a bearer
  // string alongside a separate X25519 identity — it IS the identity,
  // hex-encoded. `ozBond0Evaluate()` (ozcrypto.h:414) hex-decodes `app_id`
  // directly into the 32 raw pubkey bytes it bonds against; the bond struct
  // says the same thing explicitly (`uint8_t pub[32]; // the member's
  // X25519 public key == its app_id`, ozcrypto.h:260). Every lock a member
  // has ever bonded to already trusts these exact bytes. So there is no
  // second key to register and no `apps` table to hold one — `locks.app_id`
  // (written by registerPairing() at `POST /pairings`, since before this
  // file could spell "X25519") has been an app public-key store from day
  // one, without anyone having named it that. The `apps` table and
  // `POST /apps/register` this comment used to describe are GONE — first
  // written, then reverted the same day, before either ran against
  // production. Only the challenge/token/JWT flow below survived, reading
  // `locks.app_id` directly instead of a dedicated table.
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
      type ENUM('pin','rfid','fingerprint'),
      slot_number INT,
      sync_status VARCHAR(50) DEFAULT 'pending',
      issued_by VARCHAR(50) DEFAULT 'owner',
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // S3 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08:
  // migrations/S3_drop_raw_value.sql, folded in here so every environment
  // running this code — not just the one it was run against by hand —
  // converges to the same schema. Irreversible on purpose: no plaintext
  // PIN/RFID should be recoverable from server-side storage after cutover.
  const [[{ hasRawValue }]] = await pool.query(
    `SELECT COUNT(*) AS hasRawValue FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'grants' AND column_name = 'raw_value'`,
    [CONFIG.DB.database]
  );
  if (hasRawValue) await pool.query('ALTER TABLE grants DROP COLUMN raw_value');

  // ozkey-29 cutover, executed 2026-08-13 (operator: "the residential server
  // retains nothing beyond the pairing relationship" — same irreversible-on-
  // purpose posture as S3 above, same table, next column over). `user_name`
  // was never the server's to hold — the lock never receives it either
  // (ozkey-29 §11.1, verified against ozdoorlock_core.h), so there was no
  // path where storing it server-side did anything but create the
  // transaction log the Sovereign Edge paper commits to never having.
  // `date_from`/`date_to` are dropped alongside it — confirmed redundant,
  // never read back by any server logic (XF-95 §2); the lock already
  // receives and enforces its own copy inside the sealed credential
  // envelope. Queue expiry (where it exists) uses `pending_queue.expires_at`,
  // untouched by this.
  for (const col of ['user_name', 'date_from', 'date_to']) {
    const [[{ hasCol }]] = await pool.query(
      `SELECT COUNT(*) AS hasCol FROM information_schema.columns
        WHERE table_schema = ? AND table_name = 'grants' AND column_name = ?`,
      [CONFIG.DB.database, col]
    );
    if (hasCol) await pool.query(`ALTER TABLE grants DROP COLUMN ${col}`);
  }

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

  // ozkey-13 §8 S2/S6 (relay-opaque migration, XF-69): a sealed-envelope job
  // (app built + AES-GCM sealed the DP frame client-side) carries an opaque
  // envelope instead of a server-built one. `envelope_hex` holds it;
  // `msg_type` says which of payload_hex/envelope_hex is live for this row so
  // flushQueueForDevice() doesn't have to guess from NULL-ness. Additive
  // migration, same pattern as the `locks` columns above — MUST run after the
  // CREATE it depends on (see the enroll_tokens comment for why).
  const [[{ hasEnvelopeHex }]] = await pool.query(
    `SELECT COUNT(*) AS hasEnvelopeHex FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'pending_queue' AND column_name = 'envelope_hex'`,
    [CONFIG.DB.database]
  );
  if (!hasEnvelopeHex)
    await pool.query('ALTER TABLE pending_queue ADD COLUMN envelope_hex TEXT NULL AFTER payload_hex');

  const [[{ hasMsgType }]] = await pool.query(
    `SELECT COUNT(*) AS hasMsgType FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'pending_queue' AND column_name = 'msg_type'`,
    [CONFIG.DB.database]
  );
  if (!hasMsgType)
    await pool.query(
      "ALTER TABLE pending_queue ADD COLUMN msg_type VARCHAR(20) NOT NULL DEFAULT 'legacy_payload' AFTER envelope_hex"
    );

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

function mqttPublish(topic, payload, opts = {}) {
  if (!mqttClient || !mqttClient.connected) {
    logEvent('warn', `MQTT offline — dropped publish to ${topic}`);
    return false;
  }
  mqttClient.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), {
    qos: 1,
    retain: !!opts.retain,
  });
  return true;
}

// ozkey-33: refresh cadence for the retained time topic — same period as
// the existing bridge `utc` push (CONFIG.UTC_PUSH_REFRESH_MS) so there is
// one clock-freshness budget across both paths, not two to keep in sync.
let timeRefreshTimer = null;
// XF-144 (2026-09-01): the multi-bridge failover sweep's own timer —
// deliberately separate from timeRefreshTimer, see its arming site for why.
let bridgeFailoverTimer = null;

function publishRetainedTime(why) {
  const nowUtc = Math.floor(Date.now() / 1000);
  // Signed minutes east of UTC. `Date.getTimezoneOffset()` returns minutes
  // WEST of UTC (Node's local zone, i.e. this host's) — negate it to match
  // the sign convention firmware asked for.
  const tzMinutes = -new Date().getTimezoneOffset();
  const ok = mqttPublish(
    CONFIG.topicTime(CONFIG.SITE_ID),
    { utc: nowUtc, tz: tzMinutes },
    { retain: true }
  );
  if (ok) logEvent('info', `Published retained time (utc=${nowUtc}, tz=${tzMinutes}) to ${CONFIG.topicTime(CONFIG.SITE_ID)} (${why})`);
}

// ozkey-31 §2: deviceId -> the most recently PUBLISHED, not-yet-confirmed
// grant-key command for that device. The uplink's plaintext outcome code
// carries no grant_id to correlate against directly, but the lock processes
// credential commands synchronously and one at a time (ozAwaitMcuAck blocks
// before any reply), so "this device's most recent unconfirmed grant" is
// what a fresh outcome code is answering, not a guess dressed up as one.
const lastGrantSentByDevice = new Map(); // deviceId -> { grantId, queueId }

// XF-125 P0/§3: a lock is AT RISK exactly when rotation has destroyed events
// (`dropped_before_seq`) that no app has ever confirmed pulling
// (`last_pulled_seq`, via PATCH /locks/:id — there is no wire signal to
// observe instead, since `query_events` is a BLE-only exchange the server
// never sees, confirmed against ozdoorlock_core.h:6103-6161).
// This is the precise "silent data loss" case firmware's own comment names
// (heartbeat handler, "a consumer whose cursor is below dropped_before_seq
// has provably missed records") — not a fuzzy staleness threshold, an exact
// check against a fact firmware already computes and reports.
function eventsAtRisk(lock) {
  // XF directive S7, found live 2026-08-23: `!= null` is true for 0, but
  // firmware's own wire contract (XFtposDecisions-126 §13.2 area /
  // ozdoorlock_core.h `hb["dropped_before_seq"]`) documents 0 as the "no
  // records lost yet" sentinel, not a real loss. `!= null` was flagging
  // every healthy lock with a fresh ring buffer as at-risk on its very
  // first heartbeat carrying the field — confirmed live: LockB logged
  // "events AT RISK — rotation destroyed records up to seq 0" the instant
  // it started reporting, having lost nothing. `> 0` is null-safe in JS
  // (`null > 0` is false) and only trips on an actual positive loss count.
  return (
    lock.dropped_before_seq > 0 &&
    (lock.last_pulled_seq == null || lock.dropped_before_seq > lock.last_pulled_seq)
  );
}

// XF-131 Ask 2: `eventsAtRisk` only fires once rotation has actually
// destroyed something unread — the LATE warning. This is the EARLY one:
// the lock has events (`seq_highwater > 0`) nobody has confirmed pulling
// yet, whether or not any have been lost. A backlog nobody can see is the
// same failure as no backlog at all (the ticket's own framing) — this is
// what makes it visible before it becomes unrecoverable. `> 0` guard for
// the same reason as eventsAtRisk: a lock with zero events ever written
// and last_pulled_seq still null must read false, not true.
function eventsPending(lock) {
  return (
    lock.seq_highwater > 0 &&
    (lock.last_pulled_seq == null || lock.seq_highwater > lock.last_pulled_seq)
  );
}

// Log once per episode, not once per heartbeat — a lock stuck at-risk would
// otherwise re-log on every beat for as long as nobody reads it, which is
// exactly the kind of noise that makes a real alert easy to tune out.
const eventsAtRiskLogged = new Set(); // deviceId

/** Drain queued actions for a device; expired unlock-style rows are skipped.
 * `onSent(job, msgId)`, if given, fires for each job actually published —
 * lets a caller that just queued a specific job (e.g. `DELETE /locks/:id`)
 * learn the `msg_id` that went out for it, without every other caller
 * needing to care (ozkey-41 §5.3 correlation). */
// XF-155 (2026-09-03): found live while investigating App's report of a
// physical-lock regression after a burst of near-simultaneous revoke
// requests. Multiple callers can invoke this for the SAME deviceId
// concurrently (every write endpoint fires it fire-and-forget right after
// its own commit — grant, revoke, unlock, bond verbs — and App's expiry
// sweep did exactly that, 6-8 times within ~3s for one lock). With no
// per-device serialization, each concurrent call independently SELECTs the
// same still-'queued' rows before any of them has a chance to mark one
// 'sent', so the SAME command gets published multiple times. Confirmed
// live: 6 concurrent revoke requests produced 3-4x duplicate publishes of
// EVERY one of them on the real command topic — exactly the kind of
// self-inflicted BLE-link hammering XF-142/143/149/152 already documented
// from other causes, just never traced to this one before. Same fix shape
// as `handleBridgePresence()`'s XF-152 fix: chain concurrent calls for one
// deviceId onto that device's own promise-queue tail so they process
// strictly in arrival order — the second call's SELECT then correctly sees
// the first call's jobs already 'sent'.
const flushQueueQueues = new Map(); // deviceId -> Promise (tail of chain)

function flushQueueForDevice(siteId, deviceId, onSent) {
  const prev = flushQueueQueues.get(deviceId) || Promise.resolve();
  const next = prev.catch(() => {}).then(() => flushQueueForDeviceImpl(siteId, deviceId, onSent));
  flushQueueQueues.set(deviceId, next);
  return next;
}

async function flushQueueForDeviceImpl(siteId, deviceId, onSent) {
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
      // XF-155 (2026-09-03): a revoke-key job that expires unsent — the
      // device never woke to receive it in time — used to leave the grants
      // row silently stuck at 'revoking' forever, with nothing anywhere
      // pointing at it. Surface it the same way an explicit lock-reported
      // failure does.
      if (job.action_type === 'revoke-key' && job.grant_id) {
        await pool.query("UPDATE grants SET sync_status = 'revoke_failed' WHERE id = ?", [
          job.grant_id,
        ]);
        await recordAudit(
          null,
          deviceId,
          'revoke_failed',
          `revoke-key grant #${job.grant_id}: queue #${job.id} expired unsent`
        );
      }
      continue;
    }

    const sealed = job.msg_type === 'sealed_envelope';

    const commandTopic = bridgeId
      ? CONFIG.topicBridgeCommand(siteId, bridgeId)
      : CONFIG.topicCommand(siteId, deviceId);
    const envelope = {
      msg_id: `ozl-${job.id}-${Date.now()}`,
      device_id: deviceId,
      action: job.action_type,
      grant_id: job.grant_id,
      ...(sealed ? { envelope_hex: job.envelope_hex } : { payload_hex: job.payload_hex }),
      issued_at: new Date().toISOString(),
      source: 'ozlockserv',
    };
    // bridge32 demuxes on {target, payload}/{target, envelope_hex}
    // (CONTRACT-BRIDGE / ozkey-11 §3, extended ozkey-13 §8 S7/BR1/F7). Send
    // it a MINIMAL envelope: the bridge reads only target + one content
    // field and rebuilds its own datagram from them, so msg_id/action/
    // grant_id/issued_at/source never cross the Thread hop and are pure
    // overhead on a constrained link. Keeping them nearly broke the product:
    // PubSubClient's default MQTT_MAX_PACKET_SIZE is 256 bytes and silently
    // discards anything larger, so the ~280-byte full envelope was dropped
    // by every bridge without a trace while short hand-made test publishes
    // sailed through (found live 2026-07-29). bridge32 now also calls
    // setBufferSize(1024), but keeping the wire small is the belt to that
    // braces — a stock-configured bridge, or one on an older build, still
    // works. The bridge never decodes `envelope_hex` — pure pass-through,
    // same as `payload` always was; only the field name differs.
    const publishBody = bridgeId
      ? (sealed
          ? { msg_id: envelope.msg_id, target: deviceId, envelope_hex: job.envelope_hex }
          : { msg_id: envelope.msg_id, target: deviceId, payload: job.payload_hex })
      : envelope;

    const ok = mqttPublish(commandTopic, publishBody);
    if (!ok) break;

    await pool.query("UPDATE pending_queue SET status = 'sent' WHERE id = ?", [job.id]);
    if (job.grant_id) {
      // ozkey-31 §2 / XF-155 (2026-09-03): a publish is evidence the SERVER
      // sent something, not that the lock stored it — the exact gap that let
      // four failed remote grants sit recorded as 'synced' this week while
      // ozControlOpen rejected every one of them. Applies equally to
      // revoke-key, which used to optimistically flip straight to 'revoked'
      // here on publish alone (App found this live: a revoke against a
      // genuinely-dead lock reported "Đã thu hồi" exactly as fast as a real
      // one, no confirmation the credential was ever cleared). Stay in the
      // current in-flight state ('pending' for grant-key, 'revoking' for
      // revoke-key — set by the caller before this runs) and remember which
      // grant + which action this device's next uplink outcome code
      // (handleUplinkOutcome, below) should be attributed to; a reported
      // failure is the only state a publish can ever earn on its own now.
      lastGrantSentByDevice.set(deviceId, {
        grantId: job.grant_id,
        queueId: job.id,
        actionType: job.action_type,
      });
    }
    sent++;
    if (onSent) onSent(job, envelope.msg_id);
    logEvent(
      'sync',
      `${deviceId} wake -> burst ${job.action_type} #${job.id} down ${commandTopic}` +
        (bridgeId ? ` (via bridge ${bridgeId})` : '')
    );
  }
  return sent;
}

/** S8/S9 (ozkey-15 §3, async orchestrated removal). `request_remove` (banoi2
 * -> banoi1) and `ack_remove` (banoi1 -> banoi2) are already addressed to the
 * specific recipient's device_id in the topic path, so the broker delivers
 * publish -> subscribe directly between the two apps — nothing here needs to
 * republish anything. (It also MUST NOT: this process is itself subscribed
 * to both wildcards, so publishing back onto the same topic would be a
 * self-triggering infinite loop, not a relay.) This only logs for
 * visibility — no DB write, no `pending_queue`/`grants` involvement — per
 * §2 "pure relay, no state, no persistence". `targetDeviceId` is the
 * recipient named in the topic path (the admin's device_id for
 * request_remove, the member's for ack_remove).
 *
 * "Basic payload structure" check only, per §3 — not full validation. Who
 * may publish is an MQTT ACL matter (§"No authentication changes"), not
 * this code's job.
 */
function logMemberRelay(targetDeviceId, kind, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on members/${targetDeviceId}/${kind}: "${payload.slice(0, 60)}"`);
    return;
  }
  if (!obj.request_id || !obj.target_lock_id || !obj.target_member_app_id) {
    logEvent(
      'warn',
      `members/${targetDeviceId}/${kind} missing request_id/target_lock_id/target_member_app_id`
    );
    return;
  }
  const memberTag = String(obj.target_member_app_id).slice(0, 12) + '…';
  if (kind === 'request_remove') {
    logEvent(
      'key',
      `Removal request ${obj.request_id}: member ${memberTag} -> admin ${targetDeviceId} ` +
        `for lock ${obj.target_lock_id}`
    );
  } else {
    logEvent(
      'key',
      `Removal ACK ${obj.request_id}: status=${obj.status || '?'} -> member ${targetDeviceId} ` +
        `for lock ${obj.target_lock_id}`
    );
  }
}

/** XF-138 P5 relay channel (2026-08-31): a CH584M lock's physical/local
 * factory reset has no other delivery path — no Thread, no MQTT of its own —
 * so it rides the bridge's relay of the lock's `member` characteristic onto
 * this same uplink topic, as a plaintext `{"type":"reset",...}` message
 * rather than the usual sealed `envelope_hex` wrapper (a reset lock has no
 * bond left to seal anything with anyway, same reasoning as the plaintext
 * `code` field elsewhere on this pipeline). Deliberately NOT
 * `DELETE /locks/:id`'s `purgeLockRows()` — this must keep the row and its
 * history (including this very event), just clear ownership so the lock is
 * re-commissionable. Returns true if the payload was a reset message
 * (handled either way, including "unknown device"), false otherwise, so the
 * caller knows whether to fall through to the normal sealed-uplink path.
 */
async function handleUplinkReset(deviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return false;
  }
  if (obj.type !== 'reset') return false;

  const [[lock]] = await pool.query('SELECT app_id, bridge_id FROM locks WHERE id = ?', [
    deviceId,
  ]);
  if (!lock) {
    logEvent('warn', `Reset notification from unknown lock ${deviceId} — ignored`);
    return true;
  }
  await pool.query(
    "UPDATE locks SET app_id = NULL, bridge_id = NULL, status = 'registered' WHERE id = ?",
    [deviceId]
  );
  logEvent(
    'key',
    `Lock ${deviceId} reported factory reset via relay channel — ownership cleared ` +
      `(was app=${lock.app_id || '(none)'}, bridge=${lock.bridge_id || '(none)'})`
  );
  await recordAudit(
    lock.app_id,
    deviceId,
    'reset',
    'device-reported factory reset via relay channel — ownership cleared'
  );
  return true;
}

/** XF-138/139 P5 (2026-08-31, PM decision): the durable fix for
 * `caps_source` staying permanently `'inferred'` on a CH584M lock —
 * `publishEnroll()`'s full identity/capability document (the same shape
 * `handleEnroll()` already parses via `deviceIdentity()`), relayed by the
 * bridge over the reused `member` characteristic onto this same uplink
 * topic, since a CH584M lock has no MQTT session of its own to publish it
 * on directly.
 *
 * Deliberately NOT `handleEnroll()` — reaching that function has always
 * meant "the lock itself spoke MQTT," proof of being un-bridged, and it
 * unconditionally sets `bridge_id = NULL` on that basis (its own header
 * comment). Calling it here would silently unpair every bridged lock the
 * instant it reported its capabilities. This only ever touches
 * `caps`/`transport` on an already-bonded (`app_id` set) lock — it does
 * not enroll, does not touch `bridge_id`/`status`/broker credentials.
 *
 * Detected by content shape (`caps` present as an array), not a `type`
 * field — firmware's payload is `addIdentity()`'s existing document, which
 * (like the enroll payload it's shared with) carries no `type`
 * discriminator of its own.
 */
async function handleUplinkCaps(deviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return false;
  }
  if (!Array.isArray(obj.caps)) return false;

  const [[lock]] = await pool.query('SELECT app_id FROM locks WHERE id = ?', [deviceId]);
  if (!lock) {
    logEvent('warn', `Capability report from unknown lock ${deviceId} — ignored`);
    return true;
  }
  // Directive: "this report should only come from bonded locks anyway" — an
  // unbonded lock has no owner to attribute the audit record to, and
  // nothing server-side should trust a capability claim from a lock that
  // isn't paired to anyone yet.
  if (!lock.app_id) {
    logEvent('warn', `Capability report from unbonded lock ${deviceId} — ignored`);
    return true;
  }

  const ident = deviceIdentity(obj);
  if (!ident.caps) {
    logEvent('warn', `Capability report from ${deviceId} — no recognised caps in payload, ignored`);
    return true;
  }

  await pool.query('UPDATE locks SET caps = ?, transport = COALESCE(?, transport) WHERE id = ?', [
    ident.caps,
    ident.transport,
    deviceId,
  ]);
  logEvent('info', `Lock ${deviceId} capability report via relay channel: caps=${ident.caps}`);
  await recordAudit(lock.app_id, deviceId, 'caps_update', `device-reported caps=${ident.caps}`);
  return true;
}

/** V1 (ozkey-17 §6a/§6c, normative wire wrapper corrected 2026-08-10):
 * lock->app uplink's wrapper is `{from, to, envelope_hex}` — "read by the
 * bridge for routing only" per §6c, i.e. addressing metadata, same as a
 * postal envelope's To/From. `kind`/`reason`/`bonds`/anything else lives
 * INSIDE `envelope_hex`, sealed, and is never touched. (Earlier revision
 * of this function assumed a top-level `msg_id` — that field does not
 * exist in the actual wire format; §6c is authoritative over the general
 * §6 prose that suggested it.)
 *
 * Persisted via recordAudit(), not just logEvent() — per the operator's
 * directive this is retained for OZPMS/OZLODGE audit-trail compliance,
 * unlike S8/S9's member-relay observation which is deliberately
 * ephemeral-only. `to` is read directly from the wrapper when present
 * (it's the actual routing address ozSemanticDispatch put there); falls
 * back to recordAudit()'s own `locks.app_id` lookup otherwise, so a
 * malformed/legacy payload still gets an audit row.
 */
async function logUplinkMetadata(fromDeviceId, payloadBuf, payloadStr) {
  const size = payloadBuf.length;
  let to = null;
  let isRealUplink = false;
  try {
    const obj = JSON.parse(payloadStr);
    if (typeof obj.to === 'string') to = obj.to;
    // XF-138 P5 (2026-08-31): the bridge now relays EVERY notification on the
    // lock's `member` characteristic here, not just genuine uplink content —
    // ordinary phone<->lock chatter (e.g. a `list_bonds` reply) rides the
    // same BLE notify broadcast and lands on this same topic as a side
    // effect, since BLE notify goes to every subscriber, not just the
    // requester (found live, firmware's own post above). Only the normative
    // §6c wrapper actually carries `envelope_hex` — anything else reaching
    // here is overheard chatter, not real uplink content, and must stay
    // `logEvent()`-only per this function's own long-standing rule (see
    // above: "unlike S8/S9's member-relay observation which is deliberately
    // ephemeral-only"). Without this check, every overheard exchange would
    // silently pollute the compliance-purposed audit_log table.
    isRealUplink = typeof obj.envelope_hex === 'string';
  } catch (_) {
    // Not JSON — fine, log size/from anyway. Not a warn like the other
    // topics: firmware/bridge versions may still be settling on this
    // still-new wire shape.
  }
  logEvent(
    'info',
    `Uplink from ${fromDeviceId} to ${to || '(unresolved)'}: ${size}B (content sealed, not read)` +
      (isRealUplink ? '' : ' — no envelope_hex, treating as overheard chatter, not persisting')
  );
  if (isRealUplink) await recordAudit(to, fromDeviceId, 'uplink', `size=${size}B`);
}

/** ozkey-31 §2. The uplink wrapper's `code` field is plaintext by design
 * (ozdoorlock_core.h ~2903, "1.64 — the outcome CODE travels in the clear,
 * the detail does not") specifically so a relay that never opens the
 * envelope can still see WHICH STAGE failed. As of doorlock-1.65 every code
 * the lock emits this way — ENVELOPE_BAD_HEX, ENVELOPE_NOT_OPENED,
 * MCU_TIMEOUT, COUNTER_REPLAY — is a refusal; success (`UNLOCK_OK`) only
 * ever rides BLE's notifyStatus(), which this server never sees. So this
 * treats ANY non-empty `code` as bad news rather than matching a fixed
 * whitelist — a whitelist is one more place a new refusal string firmware
 * adds later walks straight past unnoticed, the exact bug class this doc is
 * about. If firmware ever starts sending a positive confirmation on this
 * same field, this function needs to stop treating `code` as failure-only.
 */
async function handleUplinkOutcome(fromDeviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return; // logUplinkMetadata already warned about non-JSON uplinks
  }
  if (typeof obj.code !== 'string' || !obj.code) return;

  const pending = lastGrantSentByDevice.get(fromDeviceId);
  if (!pending) {
    logEvent(
      'warn',
      `${fromDeviceId} reported outcome ${obj.code} — no pending grant on record to attribute it to`
    );
    return;
  }
  lastGrantSentByDevice.delete(fromDeviceId);

  // XF-155 (2026-09-03): distinguish which verb failed — 'revoke-key' gets
  // its own visible state ('revoke_failed') rather than reusing 'failed',
  // which read ambiguous ("failed to issue" vs "failed to revoke") once
  // revoke started tracking through this same path instead of optimistically
  // marking itself 'revoked' on publish alone.
  const isRevoke = pending.actionType === 'revoke-key';
  const failedStatus = isRevoke ? 'revoke_failed' : 'failed';
  await pool.query('UPDATE grants SET sync_status = ? WHERE id = ?', [failedStatus, pending.grantId]);
  logEvent(
    'warn',
    `Grant #${pending.grantId} (queue #${pending.queueId}, ${pending.actionType}) on ${fromDeviceId} FAILED — lock reported ${obj.code}`
  );
  await recordAudit(
    null,
    fromDeviceId,
    isRevoke ? 'revoke_failed' : 'grant_failed',
    `${pending.actionType} grant #${pending.grantId}: ${obj.code}`
  );
}

/* ---------------------------------------------------------------------------
 * ozkey-20 R5/R6 — observed presence + fault attribution
 *
 * Built ahead of firmware's R1/R2/R4 (LWT, bridge liveness table, health
 * payload) — none of those exist on the wire yet, same position SUB_UPLINK
 * was in before V1 shipped. The receiving/attribution logic is complete and
 * live-testable with synthetic MQTT publishes now; it starts producing real
 * verdicts with zero further server change the moment firmware catches up.
 * ------------------------------------------------------------------------- */

const BATTERY_LOW_PCT = 15; // ozkey-20 R6 table's own figure, not derived here.

/** ozkey-20 R6, evaluated exactly as the doc's table specifies — top-down,
 * first match wins. `bridgePresence` is looked up by the caller so this
 * function stays a pure function of its inputs (easy to unit-test/verify
 * with synthetic rows, matching this session's "verify, don't assume"
 * discipline). Q2 (flap damping) is NOT implemented here — server-side
 * hysteresis is agreed (ozkey-20 reply) but is a separate, later addition;
 * this is the bare attribution logic the doc calls "the deliverable".
 *
 * XF-138 P5 (2026-08-31): every lock now carries its own direct presence
 * signal into this function — Wi-Fi-direct locks via their own LWT, bridged
 * (BLE-native) locks via the bridge's connect/disconnect relay onto
 * `.../presence` — both written by `handleLockPresence()` before this
 * runs. The old Thread-specific `isThread`/`thread_age_s` branch (dead
 * reckoning a specific lock's reachability from a Thread mesh child-table
 * age, since a Thread lock had no direct signal of its own) is gone along
 * with Thread support itself: `bridgePresence === 'offline'` still catches
 * "the whole bridge is down" as the stronger, bridge-level fault, and
 * `direct_presence === 'offline'` now uniformly catches "this specific
 * lock says it's unreachable," for bridged and direct locks alike.
 *
 * XF-146 (2026-09-01): reads `lock.direct_presence`, NOT `lock.presence` —
 * see the schema migration's comment for why sharing one column between
 * "raw signal" and "derived verdict" created a self-reinforcing stuck-
 * offline bug the moment a bridge had any transient blip unrelated to the
 * actual lock link.
 */
function computeFaultAttribution(lock, bridgePresence) {
  const isBridged = !!lock.bridge_id;

  if (isBridged && bridgePresence === 'offline') return 'bridge_offline';
  if (lock.direct_presence === 'offline') return isBridged ? 'lock_unreachable' : 'lock_offline_wifi';

  // "either" rows — only reached once the lock is confirmed reachable over
  // its own transport (the checks above). mcu_link_down sits above
  // battery_low deliberately (ozkey-20 §5a, added 2026-08-11 on firmware
  // review): a dead DL MCU link is total loss of function — no remote, no
  // PIN, no card, no fingerprint — where a low battery is still a working
  // lock. `=== 0` (not falsy) so a never-reported NULL doesn't trigger it.
  if (lock.mcu_link_up === 0) return 'mcu_link_down';
  if (lock.battery_pct != null && lock.battery_pct < BATTERY_LOW_PCT) return 'battery_low';
  if (lock.last_mech_result && lock.last_mech_result !== 'ok') return 'mech_fault';
  if (lock.pending_uplinks > 0) return 'pending_sync';
  return 'ok';
}

/** Re-derives `presence`/`presence_reason` for one lock from whatever raw
 * inputs are currently in its row + its bridge's row, and writes both.
 * Called after any event that could change the verdict (presence,
 * heartbeat, a bridged lock's own connect/disconnect relay).
 *
 * XF-138 P5 (2026-08-31): `presence` used to be re-DERIVED for any bridged
 * lock (the Thread case had no direct signal of its own to preserve — see
 * computeFaultAttribution's header). Now every lock, bridged or not, gets a
 * direct presence write from `handleLockPresence()` before this runs, so
 * this simply trusts that direct signal unless a stronger, independently-
 * computed fault (bridge_offline, etc.) overrides it to 'offline'.
 *
 * XF-146 (2026-09-01): falls back to `lock.direct_presence`, NOT
 * `lock.presence` — this function's own OUTPUT column must never also be
 * its input, or a transient bridge-level fault (which clears on its own
 * seconds later) permanently overwrites the real, still-healthy direct
 * signal with no way back short of a brand-new physical reconnect. See the
 * schema migration and `computeFaultAttribution()` for the full story —
 * found live on LockA, confirmed against firmware's own serial log
 * showing the actual BLE link never dropped at all.
 */
async function recomputeAndStorePresence(deviceId) {
  const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
  if (!lock) return;

  let bridgePresence = 'unknown';
  if (lock.bridge_id) {
    const [[bp]] = await pool.query('SELECT presence FROM bridges_presence WHERE bridge_id = ?', [
      lock.bridge_id,
    ]);
    bridgePresence = bp ? bp.presence : 'unknown';
  }

  const reason = computeFaultAttribution(lock, bridgePresence);
  const unreachable = reason === 'bridge_offline' || reason === 'lock_unreachable' || reason === 'lock_offline_wifi';
  const presence = unreachable ? 'offline' : lock.direct_presence;

  // XF-144/XF-152 (2026-09-03): XF-152's periodic MQTT unsubscribe/resubscribe
  // forces a redundant retained redelivery every 30s, re-triggering this
  // function for every lock behind a bridge even when nothing changed.
  // Unconditionally bumping presence_at on every no-op re-verdict silently
  // restarted XF-144's failover sweep's own 2-minute grace clock (keyed off
  // this exact column) every 30s, so it could mathematically never reach its
  // threshold — a lock whose only recovery path was the sweep got starved
  // indefinitely. Only advance presence_at on an actual state transition.
  if (lock.presence === presence && lock.presence_reason === reason) {
    return { presence, reason };
  }

  await pool.query('UPDATE locks SET presence = ?, presence_reason = ?, presence_at = NOW() WHERE id = ?', [
    presence,
    reason,
    deviceId,
  ]);
  return { presence, reason };
}

/** ozkey-20 R1 + ozkey-41: `locks/<id>/presence`. `reason` is one of
 * `lwt` (retained, broker-published Will), no-reason (retained, published by
 * the lock on every connect — §4.2), or a reset outcome — `factory_reset`
 * (retained, before the wipe), `factory_reset_denied` / `no_bond` (not
 * retained). Reset outcomes also carry `msg_id`, echoed from the request,
 * for `pendingLockResets` correlation (§5.3) — a Thread lock's outcome
 * arrives on this exact same topic, relayed verbatim by bridge32 (§2.1), so
 * no transport branching is needed here. */
async function handleLockPresence(deviceId, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on locks/${deviceId}/presence: "${payload.slice(0, 60)}"`);
    return;
  }
  const state = obj.state === 'online' || obj.state === 'offline' ? obj.state : 'unknown';

  // XF-144 (2026-09-02): reconciliation-on-connect. `via` (bridge's own
  // device_id, added to ozBuildLockPresence() this session) is stamped on
  // BOTH the online and offline publish for audit symmetry — Firmware's own
  // caveat, respected here: gate strictly on `state === 'online'`. A bridge
  // reporting `via` on an OFFLINE message is saying "I lost this lock," not
  // "I now own it" — treating that as a claim would misfire on every normal
  // disconnect. Closes the race found live tonight: a recovering bridge
  // silently reclaiming a lock the server had already failed over
  // elsewhere, with nobody telling the DB the ground truth changed.
  // Fetched unconditionally now (not just inside the online branch below) —
  // the new ownership gate a few lines down needs `bridge_id` for OFFLINE
  // `via` reports too (a non-owning bridge's own disconnect/lwt as it
  // complies with `release_lock`), not just online claims.
  const [[lock]] = await pool.query('SELECT bridge_id, presence FROM locks WHERE id = ?', [deviceId]);
  let effectiveBridgeId = lock ? lock.bridge_id : null;

  if (obj.via && state === 'online') {
    if (lock && lock.bridge_id !== obj.via) {
      const oldBridgeId = lock.bridge_id;
      // XF-144 (2026-09-02, second pass): the naive "most recent connect
      // wins" version of this check caused a real, confirmed dual-BLE-slot
      // collision live tonight — a recovering Bridge 1 silently reclaimed
      // LockA/LockC from a healthy, actively-working Bridge 2 (both bridges
      // held real live connections simultaneously, verified via both
      // bridges' own logs, see XF-151). Firmware's proposed fix, built here:
      // gate on the LOCK's own current presence, not the bridge's general
      // health. `presence === 'online'` means the CURRENT bridge_id is
      // confirmed still working right now — a new claimant in that state is
      // racing a healthy assignment, not recovering a dead one, and gets
      // rejected + told to let go. `presence !== 'online'` (offline/unknown,
      // e.g. bridge_offline) means the current assignment is already
      // known-bad — this is the legitimate-failover case, unaffected,
      // same accept-and-reconcile behavior as before.
      //
      // Deliberately does NOT touch `PATCH /locks/:id` (manual "Ghép khoá
      // vào Bridge" re-pairing) — that's a separate, explicit, human-driven
      // code path that unconditionally overwrites `bridge_id` today and
      // still should; this gate only governs automatic reconciliation
      // triggered by a bridge's own presence report.
      if (lock.presence === 'online') {
        mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, obj.via), {
          op: 'release_lock',
          id: deviceId,
        });
        logEvent(
          'key',
          `XF-144 reconciliation REJECTED: lock ${deviceId} claim from ${obj.via} refused — ` +
            `current bridge ${oldBridgeId} still confirmed healthy (presence=online). ` +
            `release_lock sent to ${obj.via} to prevent a dual-connection collision.`
        );
        await recordAudit(
          null,
          deviceId,
          'bridge_reconcile_rejected',
          `claim from ${obj.via} rejected — ${oldBridgeId} still healthy; release_lock sent to ${obj.via}`
        );
      } else {
        await pool.query(
          'UPDATE locks SET bridge_id = ?, mcu_link_up = NULL, mcu_last_frame_s = NULL WHERE id = ?',
          [obj.via, deviceId]
        );
        await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
          deviceId,
          obj.via,
        ]);
        await pool.query(
          `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
             VALUES (?, ?, 0, NOW())
           ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
          [deviceId, obj.via]
        );
        logEvent(
          'key',
          `XF-144 reconciliation-on-connect: lock ${deviceId} bridge_id corrected from ` +
            `${oldBridgeId || '(none)'} to ${obj.via} (current bridge was not confirmed healthy)`
        );
        await recordAudit(
          null,
          deviceId,
          'bridge_reconcile',
          `bridge_id corrected to ${obj.via} via reconciliation-on-connect (was ${oldBridgeId || '(none)'})`
        );
        effectiveBridgeId = obj.via;
      }
    }
  }

  // XF-151 (2026-09-02): found live, real hardware — a rejected (or simply
  // non-owning) bridge's own report was still overwriting `direct_presence`
  // unconditionally below, poisoning the raw signal for a lock it doesn't
  // actually hold. Exact sequence that happened: Bridge 2's stale claim got
  // correctly REJECTED above (bridge_id stayed on the real owner, Bridge 1),
  // but the code still fell through and wrote `direct_presence='online'`
  // from Bridge 2's rejected message; moments later Bridge 2 complied with
  // `release_lock` and disconnected, publishing its OWN offline/lwt — which
  // ALSO got written unconditionally, flipping the lock to a false
  // stale-offline read even though Bridge 1 (the real, working owner) never
  // lost the connection. A remote unlock was refused as a result.
  //
  // Fix: only trust a presence report for the raw signal if it either
  // carries no `via` at all (Wi-Fi-direct locks, or firmware predating this
  // field — unconditional write preserved exactly as before, no regression)
  // or `via` matches the lock's EFFECTIVE bridge_id (the current owner,
  // already updated above if this message just legitimately won the
  // reconciliation). A report from any other bridge — rejected, or a
  // straggler from a bridge that was never the owner — is real signal
  // about THAT bridge, but not about this lock's actual reachability, and
  // must not overwrite it.
  // Only the raw-signal write is gated — `pendingLockResets` below still
  // runs regardless of which bridge relayed the message. A reset outcome is
  // a fact about the LOCK itself (device-initiated), not about a bridge's
  // reachability, and whoever currently holds the connection when a reset
  // fires is the one that relays it — that isn't necessarily the bridge_id
  // on record if it's already stale (the exact class of gap XF-150 found).
  // Not skipping it here would risk a real reset waiter hanging on a
  // timeout instead of resolving cleanly.
  const ownershipMismatch = !!(obj.via && lock && effectiveBridgeId && obj.via !== effectiveBridgeId);
  if (ownershipMismatch) {
    logEvent(
      'info',
      `Presence report for lock ${deviceId} from non-owning bridge ${obj.via} ` +
        `(current owner: ${effectiveBridgeId}) — ignored, raw signal not overwritten`
    );
  } else {
    // XF-119 §6/§9.3 ask 2: raw reason, not the derived R6 one — cleared to
    // NULL by the canonical online message (no `reason` field, ozpresence.h),
    // so this can never outlive a re-pair.
    //
    // XF-146 (2026-09-01): writes `direct_presence`, NOT the public
    // `presence` column — that one belongs exclusively to
    // `recomputeAndStorePresence()`'s derived verdict now. This is the raw
    // signal only; see that function and the schema migration for why
    // sharing one column between "what was directly reported" and "what we
    // last concluded" created a stuck-offline bug on a transient bridge
    // blip.
    await pool.query('UPDATE locks SET direct_presence = ?, last_reset_reason = ? WHERE id = ?', [
      state,
      obj.reason || null,
      deviceId,
    ]);
    const result = await recomputeAndStorePresence(deviceId);
    if (result)
      logEvent(
        'info',
        `Presence: lock ${deviceId} -> ${state}${obj.reason ? ` (${obj.reason})` : ''}, verdict=${result.reason}`
      );
  }

  // ozkey-41 §5.2: `no_bond` means the sender's request couldn't even be
  // decrypted — the lock is ALREADY unowned, which for a removal is the
  // desired end state, not a refusal. Resolve it the same as a confirmed
  // reset. `lwt` and a plain online/reconnect message are not reset
  // outcomes and are deliberately not resolved here.
  if (pendingLockResets.has(deviceId)) {
    if (obj.reason === 'factory_reset' || obj.reason === 'no_bond') {
      notifyLockResetWaiters(deviceId, obj.msg_id, 'reset_confirmed', 'presence_confirmed');
    } else if (obj.reason === 'factory_reset_denied') {
      notifyLockResetWaiters(deviceId, obj.msg_id, 'reset_denied', 'presence_denied');
    }
  }
}

/** ozkey-20 R1, bridge half. Same payload shape, `bridges_presence` instead
 * of `locks`. A bridge going offline is the aggregation trigger (R6
 * "mandatory") — every Thread lock behind it gets re-verdicted in one pass
 * rather than waiting for each lock's own next signal. */
// ozkey-P2 (courier rule, XF-84 — "the server knows it sent the message; it
// does not know the bridge executed it"): POST /bridges/:id/reset used to
// return ok:true the instant the MQTT publish succeeded, which reports the
// SERVER's success, not the BRIDGE's. bridge32's only wire-level signal for
// a successful reset is publishing `{"state":"offline","reason":
// "factory_reset"}` to its own presence topic immediately before wiping
// (bridge32.ino ~733-740, confirmed by reading the firmware — a DENIED
// reset, by contrast, only Serial.printf()s and returns; nothing is
// published, so denial cannot be distinguished from silence on the wire —
// see the route below). This map lets a pending REST request wait on that
// one real signal instead of guessing from the publish alone.
const pendingBridgeResets = new Map(); // bridgeId -> Set<(verdict) => void>

// ozkey-27 §9: last time we successfully published `utc` to a given bridge
// (ms, Date.now()), so a caller can tell "recently confirmed" from "stale"
// without a new timer.
const lastUtcPushAt = new Map(); // bridgeId -> ms

function pushUtcToBridge(bridgeId, why) {
  const nowUtc = Math.floor(Date.now() / 1000);
  mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), { utc: nowUtc });
  lastUtcPushAt.set(bridgeId, Date.now());
  logEvent('info', `Pushed utc=${nowUtc} to bridge ${bridgeId} (${why})`);
}

// XF-138 P5 (2026-08-31): `pushUtcToBridge()` used to get its ongoing
// refresh cadence for free by piggybacking on the Thread-liveness message
// each bridge sent periodically — removed along with Thread support (no
// lock uses it anymore, and that message was Thread-mesh-specific). A
// bridge still needs fresh time on an ongoing basis (it's what relays a
// connected lock's clock, per today's whole time-sync fix), so this is now
// its own explicit periodic sweep of every currently-online bridge, run on
// the same `timeRefreshTimer` cadence as the retained-time-topic refresh
// rather than a second independent timer.
async function refreshOnlineBridgeTimePushes() {
  const [bridges] = await pool.query("SELECT bridge_id FROM bridges_presence WHERE presence = 'online'");
  for (const { bridge_id } of bridges) {
    pushUtcToBridge(bridge_id, 'periodic refresh sweep, ozkey-27 §9');
  }
}

// XF-144 (2026-09-01, PM directive): grace period before a lock's assigned
// bridge is treated as failed enough to fail over. Deliberately NOT
// instant — the entire reason this feature exists (XF-146, same session)
// is that a transient ~9s bridge-level MQTT blip, completely unrelated to
// the actual BLE link, produced a false `bridge_offline` reading. Reacting
// to that instantly here would have started a real failover — connecting
// a backup bridge to a lock whose original bridge was fine seconds later
// — on the exact class of signal already proven unreliable this session.
// 2 minutes is comfortably longer than every transient blip observed
// tonight (the worst was ~17s of confusion end-to-end) while still being
// fast enough to matter for an actually-dead bridge.
const FAILOVER_GRACE_MS = 2 * 60 * 1000;

/** Periodic sweep, same cadence as the time-refresh timer. For any lock
 * whose CURRENT bridge has read `bridge_offline` for at least
 * `FAILOVER_GRACE_MS` (a real, sustained outage — not a blip), looks up
 * the next-priority candidate in `lock_bridges` that is itself currently
 * online, and — only then, on-demand, never speculatively — promotes it
 * to be the new active `bridge_id`, exactly the same effect as a manual
 * `PATCH /locks/:id {bridge_id:...}` would have. Does nothing if no
 * online candidate exists — a lock with no working fallback just stays
 * `bridge_offline`, honestly, rather than routing to a bridge nobody has
 * confirmed can actually reach it.
 */
async function runBridgeFailoverSweep() {
  const [stuck] = await pool.query(
    `SELECT id, bridge_id FROM locks
      WHERE bridge_id IS NOT NULL AND presence_reason = 'bridge_offline'
        AND presence_at <= DATE_SUB(NOW(), INTERVAL ? SECOND)`,
    [FAILOVER_GRACE_MS / 1000]
  );
  for (const lock of stuck) {
    let [[next]] = await pool.query(
      `SELECT lb.bridge_id FROM lock_bridges lb
         JOIN bridges_presence bp ON bp.bridge_id = lb.bridge_id AND bp.presence = 'online'
        WHERE lb.lock_id = ? AND lb.bridge_id != ?
        ORDER BY lb.priority ASC
        LIMIT 1`,
      [lock.id, lock.bridge_id]
    );
    let viaScanConfirm = false;
    // XF-144 (2026-09-02): a lock that has only ever lived on one bridge has
    // no `lock_bridges` candidate row at all — confirmed live (LockB) to get
    // stuck here forever otherwise, even with other healthy bridges in range
    // that can actually see it. Fall back to a live scan-confirm before
    // giving up; still on-demand only, no speculative hot standbys.
    if (!next) {
      const { results } = await scanForLockAcrossBridges(lock.id);
      const candidates = results
        .filter((r) => r.found && r.bridge_id !== lock.bridge_id)
        .sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
      if (candidates.length) {
        next = { bridge_id: candidates[0].bridge_id };
        viaScanConfirm = true;
      }
    }
    if (!next) continue; // no confirmed-reachable fallback candidate — nothing safe to do
    const oldBridgeId = lock.bridge_id;
    await pool.query(
      `UPDATE locks SET bridge_id = ?, mcu_link_up = NULL, mcu_last_frame_s = NULL,
         direct_presence = 'unknown', presence = 'unknown', presence_reason = NULL
       WHERE id = ?`,
      [next.bridge_id, lock.id]
    );
    // Same demote-then-promote shape PATCH /locks/:id already uses (XF-138
    // P5) — without this, the old bridge is left stuck at priority 0
    // alongside the new one (found live on real hardware, XF-144
    // 2026-09-02: a real auto-failover left BOTH ozb-98a316a7e638 and
    // ozb-206ef116f390 at priority 0 for LockA), and a future sweep's
    // `ORDER BY priority ASC LIMIT 1` could arbitrarily re-pick the
    // already-failed bridge on a tie.
    await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
      lock.id,
      next.bridge_id,
    ]);
    await pool.query(
      `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
         VALUES (?, ?, 0, NOW())
       ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
      [lock.id, next.bridge_id]
    );
    // XF-144 (2026-09-02): the DB write above updates routing INTENT only —
    // Firmware confirmed the new bridge never actually adopts the lock
    // without this, so a "successful" failover was reachable-looking
    // (caps still said remote_unlock:true) while genuinely broken
    // (forwardOverBle() requires the lock in the bridge's own g_bleLocks
    // allowlist first). Same shape as unpair_lock (XF-147): fire-and-
    // forget, no result topic — the real confirmation is the subsequent
    // BLE connect + presence-online event this sweep/server already
    // watches for, same as a manual "Ghép khoá vào Bridge" pairing.
    mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, next.bridge_id), {
      op: 'pair_lock',
      id: lock.id,
    });
    logEvent(
      'key',
      `XF-144 auto-failover: lock ${lock.id} moved from bridge ${oldBridgeId} ` +
        `(bridge_offline ${FAILOVER_GRACE_MS / 1000}s+) to bridge ${next.bridge_id}, ` +
        `pair_lock dispatched${viaScanConfirm ? ' (via scan-confirm, no lock_bridges history)' : ''}`
    );
    await recordAudit(
      null,
      lock.id,
      'bridge_failover',
      `auto-failover from ${oldBridgeId} to ${next.bridge_id} after sustained bridge_offline (pair_lock dispatched` +
        `${viaScanConfirm ? ', candidate found via scan-confirm — no prior lock_bridges history)' : ')'}`
    );
  }
}

function waitForBridgeResetVerdict(bridgeId, timeoutMs) {
  return new Promise((resolve) => {
    const settle = (verdict, cause) => {
      clearTimeout(timer);
      const set = pendingBridgeResets.get(bridgeId);
      if (set) {
        set.delete(settle);
        if (!set.size) pendingBridgeResets.delete(bridgeId);
      }
      resolve({ verdict, cause });
    };
    // `cause` distinguishes "we waited the full budget and heard nothing"
    // from "the bridge went offline for an unrelated/ambiguous reason and
    // we stopped waiting early" — both resolve 'unknown', but conflating
    // the two in a log line would be exactly the kind of imprecision this
    // fix exists to remove.
    const timer = setTimeout(() => settle('unknown', 'timeout'), timeoutMs);
    if (!pendingBridgeResets.has(bridgeId)) pendingBridgeResets.set(bridgeId, new Set());
    pendingBridgeResets.get(bridgeId).add(settle);
  });
}

/** XF-144 (2026-09-01): a lock has no opinion on which bridge is "best" for
 * it — only a bridge's own targeted BLE scan measures real bridge-to-lock
 * link quality (RSSI), which is what routing should actually be based on,
 * per the operator's reasoning for this feature (replacing the phone's own
 * BLE-scan RSSI ranking, which only ever measured "can the PHONE hear the
 * bridge"). Keyed by the TARGET device_id being scanned for, not by
 * bridge_id — several bridges each report back about the same target, and
 * every response arriving inside the window is collected, not just the
 * first, so the caller can compare all candidates that actually answered.
 */
const pendingLockScans = new Map(); // deviceId -> Array<{bridge_id, found, rssi}>

function handleBridgeScanResult(bridgeId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${bridgeId}/scan_result: "${payloadStr.slice(0, 60)}"`);
    return;
  }
  const deviceId = typeof obj.target === 'string' ? obj.target : null;
  if (!deviceId) return;
  const pending = pendingLockScans.get(deviceId);
  if (!pending) {
    // Not an error — a bridge answering after the window closed (or a scan
    // nobody requested) is expected under real network timing, not a bug.
    logEvent(
      'info',
      `Scan result from bridge ${bridgeId} for ${deviceId} arrived with no pending request — dropped`
    );
    return;
  }
  pending.push({
    bridge_id: bridgeId,
    found: obj.found === true,
    rssi: Number.isFinite(obj.rssi) ? obj.rssi : null,
  });
}

// XF-144 (2026-09-01, post-live-verification): a real bridge's scan_result
// consistently arrived ~150-170ms AFTER a bare `timeoutMs`-length wait here —
// confirmed via `/api/events` timestamps against BOTH real bench bridges,
// twice, not a one-off ("... arrived with no pending request — dropped").
// The requested `duration_s` is a scan-hardware budget on the bridge's side;
// it does not include the MQTT publish round-trip or the bridge's own
// processing before it publishes. Waiting exactly `timeoutMs` here raced
// every real response and silently dropped 100% of them — this margin is
// the fix, not a new discovery of "responses are slow."
const SCAN_RESULT_GRACE_MS = 3000;

/** Broadcasts a targeted scan request to every currently-online bridge,
 * waits `timeoutMs` (plus SCAN_RESULT_GRACE_MS network/processing margin)
 * for whatever `scan_result` responses arrive, and returns all of them (not
 * just the best) so the caller applies its own ranking. Verified live
 * against real firmware (`bridgeBLE-0.34`) on both bench bridges as of
 * 2026-09-01 — see XF-144 for the dropped-response bug this margin fixes.
 */
async function scanForLockAcrossBridges(deviceId, timeoutMs = 8000) {
  const [bridges] = await pool.query("SELECT bridge_id FROM bridges_presence WHERE presence = 'online'");
  if (!bridges.length) return { requested: 0, results: [] };

  pendingLockScans.set(deviceId, []);
  for (const { bridge_id } of bridges) {
    mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridge_id), {
      op: 'scan',
      target: deviceId,
      duration_s: Math.ceil(timeoutMs / 1000),
    });
  }
  await new Promise((resolve) => setTimeout(resolve, timeoutMs + SCAN_RESULT_GRACE_MS));
  const results = pendingLockScans.get(deviceId) || [];
  pendingLockScans.delete(deviceId);
  return { requested: bridges.length, results };
}

function notifyBridgeResetWaiters(bridgeId, verdict, cause) {
  const set = pendingBridgeResets.get(bridgeId);
  if (!set) return;
  for (const settle of Array.from(set)) settle(verdict, cause);
}

// ozkey-41 §5: the lock sibling of pendingBridgeResets, built once firmware
// actually shipped a wire-level ack for locks (doorlock-1.96/bridge32-1.40 —
// `locks/<id>/presence`, `reason` one of `factory_reset`/
// `factory_reset_denied`/`no_bond`). Unlike the bridge map, a lock waiter
// also carries the request's `msg_id` — §5.3: a real ack and the app's own
// 3-minute escape hatch both end with the entry disappearing, and `msg_id`
// is the only thing on the wire that tells them apart, so a waiter must only
// settle on a matching id.
const pendingLockResets = new Map(); // deviceId -> Set<{ msgId, settle }>

function waitForLockResetVerdict(deviceId, msgId, timeoutMs) {
  return new Promise((resolve) => {
    const waiter = {
      msgId,
      settle: (verdict, cause) => {
        clearTimeout(timer);
        const set = pendingLockResets.get(deviceId);
        if (set) {
          set.delete(waiter);
          if (!set.size) pendingLockResets.delete(deviceId);
        }
        resolve({ verdict, cause });
      },
    };
    const timer = setTimeout(() => waiter.settle('unknown', 'timeout'), timeoutMs);
    if (!pendingLockResets.has(deviceId)) pendingLockResets.set(deviceId, new Set());
    pendingLockResets.get(deviceId).add(waiter);
  });
}

function notifyLockResetWaiters(deviceId, msgId, verdict, cause) {
  const set = pendingLockResets.get(deviceId);
  if (!set) return;
  // XF-115 §3: today every Thread-relayed reset outcome arrives with no
  // msg_id at all (fixed once firmware ships bridge32/doorlock carrying it
  // through — see ozkey-41 §10 discussion). An id-less message can't be
  // correlated by id, so fall back to device-only matching — but ONLY when
  // there is exactly one waiter, since that's the sole case with nothing to
  // disambiguate. With 2+ concurrent waiters for the same device (e.g. a
  // rapid double DELETE), an id-less message is genuinely ambiguous and
  // must settle none of them rather than guessing and resolving the wrong
  // one.
  if (!msgId) {
    if (set.size === 1) for (const waiter of set) waiter.settle(verdict, cause);
    return;
  }
  for (const waiter of Array.from(set)) {
    if (waiter.msgId && waiter.msgId !== msgId) continue;
    waiter.settle(verdict, cause);
  }
}

// XF-152 (2026-09-02): mqttClient.on('message', async ...) fires one
// listener invocation per incoming packet without awaiting the previous
// one — no per-bridge ordering guarantee. A rapid offline/online flap pair
// for the SAME bridge (confirmed live: 11+ brief MQTT reconnect cycles on
// one bridge in a single session) can have their handleBridgePresenceImpl()
// DB writes complete out of delivery order, leaving `bridges_presence`
// stuck on the stale value even though the bridge is genuinely healthy —
// confirmed live twice: once via a stuck sweep candidate, once via
// GET /bridges showing a healthy bridge as offline for 4.5+ minutes.
// Fix: chain every call for a given bridgeId onto the tail of that
// bridge's own promise queue, so overlapping calls process strictly in
// arrival (enqueue) order instead of racing on the event loop. The
// `.catch(() => {})` keeps one failed call from wedging the queue for
// every future presence report on that bridge.
const bridgePresenceQueues = new Map(); // bridgeId -> Promise (tail of chain)

function handleBridgePresence(bridgeId, payload) {
  const prev = bridgePresenceQueues.get(bridgeId) || Promise.resolve();
  const next = prev.catch(() => {}).then(() => handleBridgePresenceImpl(bridgeId, payload));
  bridgePresenceQueues.set(bridgeId, next);
  return next;
}

async function handleBridgePresenceImpl(bridgeId, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${bridgeId}/presence: "${payload.slice(0, 60)}"`);
    return;
  }
  const state = obj.state === 'online' || obj.state === 'offline' ? obj.state : 'unknown';
  await pool.query(
    `INSERT INTO bridges_presence (bridge_id, presence, presence_at) VALUES (?, ?, NOW())
     ON DUPLICATE KEY UPDATE presence = VALUES(presence), presence_at = VALUES(presence_at)`,
    [bridgeId, state]
  );
  logEvent('info', `Presence: bridge ${bridgeId} -> ${state}${obj.reason ? ` (${obj.reason})` : ''}`);

  // ozkey-P2: resolve any REST call currently waiting on this bridge's reset
  // outcome. `reason==='factory_reset'` is the one positive signal that
  // exists; going offline for any OTHER reason while a reset is pending
  // (dropped connection, unrelated reboot, ...) is genuinely ambiguous —
  // could be mid-reset, could be nothing to do with it — so it resolves as
  // 'unknown' immediately rather than making the caller wait out the full
  // timeout for a state that already can't get more informative.
  // ozkey-25 §5.2/§5.3: firmware's `bridge32-1.34` adds a real denial
  // signal — {"state":"online",...,"reason":"factory_reset_denied"},
  // deliberately NOT retained (a refusal is an event, not a liveness
  // state; see their reply for why retaining it would be a bug). That's
  // why this checks `reason` on BOTH online and offline, not just offline
  // as before — the denial arrives on the opposite state from a confirm.
  if (pendingBridgeResets.has(bridgeId)) {
    if (state === 'offline' && obj.reason === 'factory_reset') {
      notifyBridgeResetWaiters(bridgeId, 'reset_confirmed', 'presence_confirmed');
    } else if (state === 'online' && obj.reason === 'factory_reset_denied') {
      notifyBridgeResetWaiters(bridgeId, 'reset_denied', 'presence_denied');
    } else if (state === 'offline') {
      notifyBridgeResetWaiters(bridgeId, 'unknown', 'offline_unrelated_reason');
    }
    // else: online with no matching reason (e.g. a routine reconnect) —
    // not informative either way, keep waiting for the real timeout.
  }

  // ozkey-20 §23.1 (2026-08-12, firmware's live-test finding): UDP 123 is
  // blocked on this network (see the NTP memory note), so a freshly
  // connected bridge has no clock until something injects one. `bridge32`
  // >=1.20 already accepts `{"utc": <unix_seconds>}` on its own command
  // topic — this was previously a manual bench injection; push it
  // proactively the moment we observe the bridge online rather than
  // waiting to be asked, same "use the mechanism that already exists"
  // discipline as everything else in this document.
  if (state === 'online') {
    pushUtcToBridge(bridgeId, 'presence online, ozkey-20 §23.1');

    // ozkey-23 §10.2a: mint once, push once — same "v1 plaintext ack, bench
    // only" posture handleEnroll() already uses for locks (the broker
    // doesn't enforce credentials yet, so an unauthenticated publish is how
    // the credential-bearing publish itself gets delivered the first time).
    // Firmware persists to NVS and presents on future connects; we don't
    // resend to an already-provisioned bridge.
    const bridgeCreds = await getOrMintBridgeCredentials(bridgeId);
    if (bridgeCreds.minted) {
      mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), {
        op: 'broker_credentials',
        broker_username: bridgeCreds.broker_username,
        broker_secret: bridgeCreds.broker_secret,
      });
      logEvent('pair', `Minted + pushed broker credentials to bridge ${bridgeId} (first sighting)`);
    }
  }

  // Aggregation: re-verdict every lock behind this bridge in one pass, so
  // "bridge offline" produces one log line here and N correct per-lock
  // verdicts, not N separate discoveries on each lock's own next report.
  const [locks] = await pool.query('SELECT id FROM locks WHERE bridge_id = ?', [bridgeId]);
  for (const { id } of locks) await recomputeAndStorePresence(id);
  if (locks.length)
    logEvent('info', `Presence: re-verdicted ${locks.length} lock(s) behind bridge ${bridgeId}`);
}

/** XF-144 Phase 2 (2026-09-02, PM-approved): a Thread-detecting bridge's
 * fast-path report that a DIFFERENT bridge (`peer`, in the payload — NOT
 * `reportingBridgeId`, the topic's own wildcard) has gone dark, ~45-60s
 * after last heard rather than waiting out MQTT's own LWT+timeout chain
 * (typically ~2 minutes). Deliberately just a thin wrapper around the
 * SAME `handleBridgePresence()` an LWT-driven offline already calls — zero
 * new state machine, reuses every downstream effect that function already
 * has (writes `bridges_presence`, re-verdicts every lock behind that
 * bridge) for free. This is the whole point of Phase 2's fast-path fix:
 * once `bridges_presence` reflects reality this early, the Phase-1 reject-
 * gate (`handleLockPresence()`'s `via` check) naturally sees an accurate
 * `lock.presence` by the time the Thread-triggered takeover's own claim
 * arrives, instead of incorrectly refusing it as "current bridge still
 * healthy" against stale data.
 *
 * A false positive (Thread mesh partition, not an actual dead bridge) is
 * self-limiting, not a lasting corruption — `bridges_presence` gets
 * overwritten by any subsequent REAL report from that bridge, same
 * self-correcting shape every other presence signal in this system
 * already has.
 */
async function handlePeerStatus(reportingBridgeId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${reportingBridgeId}/peer_status: "${payloadStr.slice(0, 60)}"`);
    return;
  }
  if (obj.event !== 'peer_down' || !obj.peer) return;
  logEvent(
    'key',
    `XF-144 peer_down: bridge ${reportingBridgeId} reports peer ${obj.peer} down — fast-tracking bridges_presence`
  );
  await handleBridgePresence(obj.peer, JSON.stringify({ state: 'offline', reason: 'peer_down' }));
}

function initMqtt() {
  mqttClient = mqtt.connect(CONFIG.MQTT_URL, {
    clientId: `ozlockserv-${Math.random().toString(16).slice(2, 8)}`,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });

  mqttClient.on('connect', () => {
    logEvent('info', `MQTT online — broker ${CONFIG.MQTT_URL}`);
    // S16: subscribe to both ozkie/ (new) and ozkey/ (legacy) roots during
    // the migration — drop the *_LEGACY entries in a follow-up once every
    // publisher (firmware, bridge, this server, ozpmsserv) is confirmed on
    // ozkie/ only.
    mqttClient.subscribe(
      [
        CONFIG.SUB_ENROLL,
        CONFIG.SUB_ENROLL_LEGACY,
        CONFIG.SUB_HEARTBEAT,
        CONFIG.SUB_HEARTBEAT_LEGACY,
        CONFIG.SUB_MEMBER_REQUEST_REMOVE,
        CONFIG.SUB_MEMBER_REQUEST_REMOVE_LEGACY,
        CONFIG.SUB_MEMBER_ACK_REMOVE,
        CONFIG.SUB_MEMBER_ACK_REMOVE_LEGACY,
        CONFIG.SUB_UPLINK,
        CONFIG.SUB_UPLINK_LEGACY,
        CONFIG.SUB_PRESENCE_LOCKS,
        CONFIG.SUB_PRESENCE_LOCKS_LEGACY,
        CONFIG.SUB_PRESENCE_BRIDGES,
        CONFIG.SUB_PRESENCE_BRIDGES_LEGACY,
        CONFIG.SUB_BRIDGE_SCAN_RESULT,
        CONFIG.SUB_PEER_STATUS,
      ],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.SUB_ENROLL} + ${CONFIG.SUB_HEARTBEAT} + ` +
              `${CONFIG.SUB_MEMBER_REQUEST_REMOVE} + ${CONFIG.SUB_MEMBER_ACK_REMOVE} + ` +
              `${CONFIG.SUB_UPLINK} + ${CONFIG.SUB_PRESENCE_LOCKS} + ` +
              `${CONFIG.SUB_PRESENCE_BRIDGES} + ${CONFIG.SUB_BRIDGE_SCAN_RESULT} + ` +
              `${CONFIG.SUB_PEER_STATUS}` +
              ' (+ legacy ozkey/ roots during S16 migration)' +
              ' (door-event topic deliberately NOT subscribed — see header)'
          );
      }
    );

    // ozkey-33: publish immediately so a cold start (or a reconnect) never
    // leaves the retained topic stale, then keep it fresh on the same
    // cadence as the bridge `utc` push. Clear any timer from a prior
    // `connect` (a broker reconnect fires this handler again) so repeated
    // reconnects don't stack intervals.
    publishRetainedTime('MQTT connect');
    if (timeRefreshTimer) clearInterval(timeRefreshTimer);
    timeRefreshTimer = setInterval(() => {
      publishRetainedTime('periodic refresh');
      refreshOnlineBridgeTimePushes();
    }, CONFIG.UTC_PUSH_REFRESH_MS);

    // XF-144: a separate, much shorter-period timer than the UTC refresh
    // above — the failover sweep's own condition (FAILOVER_GRACE_MS, 2 min)
    // already provides the real debounce against reacting to a blip;
    // running the SWEEP itself only every 10 minutes (the UTC cadence)
    // would add up to 10 more minutes of delay on top of that grace period
    // for an actually-dead bridge, which defeats the point. 30s is frequent
    // enough to fail over promptly once the grace period genuinely elapses
    // without being a meaningful DB load.
    if (bridgeFailoverTimer) clearInterval(bridgeFailoverTimer);
    bridgeFailoverTimer = setInterval(() => {
      runBridgeFailoverSweep().catch((err) =>
        logEvent('error', `XF-144 failover sweep failed: ${err.message}`)
      );
      // XF-152 (2026-09-03): a THIRD incident, distinct from the write-
      // ordering race fixed above — a bridge's own retained presence topic
      // was already correctly 'online' at the broker, but this server's
      // subscription silently never delivered it, with no reconnect/error/
      // offline event logged anywhere in between. Root cause not confirmed
      // (transport-level — QoS/broker/network, not app code); republishing
      // the same payload fixed it live. Mitigation, root-cause-agnostic:
      // a bare re-SUBSCRIBE to an already-active topic filter does NOT
      // force retained redelivery in practice against this broker
      // (verified live — a duplicate subscribe produced zero redelivered
      // messages). UNSUBSCRIBE then SUBSCRIBE is unambiguously a fresh
      // subscription from the broker's point of view, so it does — this
      // self-heals a silently dropped presence message within ~30s instead
      // of sitting stale indefinitely. Not a diagnosis of why the original
      // delivery was missed.
      const presenceTopics = [
        CONFIG.SUB_PRESENCE_LOCKS,
        CONFIG.SUB_PRESENCE_LOCKS_LEGACY,
        CONFIG.SUB_PRESENCE_BRIDGES,
        CONFIG.SUB_PRESENCE_BRIDGES_LEGACY,
      ];
      mqttClient.unsubscribe(presenceTopics, (err) => {
        if (err) return logEvent('error', `XF-152 presence unsubscribe (resync) failed: ${err.message}`);
        mqttClient.subscribe(presenceTopics, { qos: 1 }, (err2) => {
          if (err2) logEvent('error', `XF-152 presence re-subscribe (resync) failed: ${err2.message}`);
        });
      });
    }, 30_000);
  });

  mqttClient.on('reconnect', () => logEvent('warn', 'MQTT reconnecting...'));
  mqttClient.on('error', (err) => logEvent('error', `MQTT error: ${err.message}`));
  mqttClient.on('offline', () => logEvent('warn', 'MQTT broker offline'));

  mqttClient.on('message', async (topic, payloadBuf) => {
    const payload = payloadBuf.toString('utf8').trim();
    try {
      // S16: accept either the new `ozkie/` root or legacy `ozkey/` during
      // the migration window (see the SUB_* comment near CONFIG). Drop the
      // `(?:ozkey|ozkie)` alternation back to a plain `ozkie` once every
      // publisher is confirmed off the legacy root.
      const m = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/(enroll|heartbeat|log)$/);
      if (!m) {
        // S8/S9 (ozkey-15 §3): observe-only, see the SUB_MEMBER_* comment above.
        const mm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/members\/([^/]+)\/(request_remove|ack_remove)$/);
        if (mm) {
          logMemberRelay(mm[2], mm[3], payload);
          return;
        }
        // V1 (ozkey-17 §6/§6a/§6c): lock->app uplink, published under the
        // lock's own topic. Wire wrapper is `{from, to, envelope_hex}`
        // (§6c, normative) — "content you never parse" means never
        // touching `envelope_hex`; `from`/`to`/size are the addressing
        // metadata, recorded to audit_log for OZPMS/OZLODGE compliance,
        // exactly like grant/revoke/unlock already are.
        const um = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/uplink$/);
        if (um) {
          if (await handleUplinkReset(um[2], payload)) return;
          if (await handleUplinkCaps(um[2], payload)) return;
          await logUplinkMetadata(um[2], payloadBuf, payload);
          await handleUplinkOutcome(um[2], payload);
          return;
        }
        // ozkey-20 R1: retained LWT presence, lock or bridge. Pure metadata
        // (operational, per §6a's own class table) — updates `presence`
        // directly, no content to protect.
        const plm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/presence$/);
        if (plm) {
          await handleLockPresence(plm[2], payload);
          return;
        }
        const pbm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/presence$/);
        if (pbm) {
          await handleBridgePresence(pbm[2], payload);
          return;
        }
        const bsm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/scan_result$/);
        if (bsm) {
          handleBridgeScanResult(bsm[2], payload);
          return;
        }
        const psm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/peer_status$/);
        if (psm) {
          await handlePeerStatus(psm[2], payload);
          return;
        }
        return;
      }
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
        // ozkey-20 R4 (health payload) — read opportunistically. Firmware
        // doesn't send battery_pct/pending_uplinks/last_mech_result yet
        // (only roster_epoch exists on the wire so far, per a live check
        // against ozdoorlock_core.h 2026-08-10) — COALESCE means an absent
        // field leaves the column untouched rather than clobbering it to
        // NULL/0, same discipline as fw/transport/caps above.
        const batteryPct = Number.isFinite(obj.battery_pct) ? Math.max(0, Math.min(100, obj.battery_pct)) : null;
        const pendingUplinks = Number.isFinite(obj.pending_uplinks) ? obj.pending_uplinks : null;
        const lastMechResult = typeof obj.last_mech_result === 'string' ? obj.last_mech_result.slice(0, 16) : null;
        const rosterEpoch = Number.isFinite(obj.roster_epoch) ? obj.roster_epoch : null;
        // ozkey-20 §5a (2026-08-11) — mcu_link_up/mcu_last_frame_s. Same
        // opportunistic COALESCE discipline: absent leaves the column
        // untouched, doesn't clobber a last-known value to NULL.
        const mcuLinkUp = typeof obj.mcu_link_up === 'boolean' ? (obj.mcu_link_up ? 1 : 0) : null;
        const mcuLastFrameS = Number.isFinite(obj.mcu_last_frame_s) ? obj.mcu_last_frame_s : null;
        // ozkey-32 §5, operator's call 2026-08-14: the lock is authoritative
        // for its own name (it can be renamed over BLE with no connectivity,
        // which the server would never see any other way), `locks.label` is
        // a cache. Not on the wire yet — firmware ships it on request — same
        // opportunistic COALESCE discipline as fw/transport/battery_pct etc.
        // above: absent leaves the column untouched, doesn't clobber a
        // last-known label to NULL.
        const name = typeof obj.name === 'string' && obj.name.length ? obj.name.slice(0, 255) : null;
        // XF-125 P0 — opportunistic, same discipline as everything above:
        // absent (a lock's `publishHeartbeat()` doesn't send these yet — only
        // the Thread beacon, verbatim-relayed onto this same topic by
        // bridge32, does today) leaves the columns untouched.
        const seqHighwater = Number.isFinite(obj.seq_highwater) ? obj.seq_highwater : null;
        const droppedBeforeSeq = Number.isFinite(obj.dropped_before_seq) ? obj.dropped_before_seq : null;
        // XF-127 — same opportunistic COALESCE discipline as everything
        // above: absent leaves the column untouched, doesn't clobber a
        // known `true` back to unknown.
        const hasDoorbell = typeof obj.has_doorbell === 'boolean' ? (obj.has_doorbell ? 1 : 0) : null;
        // XF-127 §8 ask 2 — same wire (Thread beacon, verbatim-relayed onto
        // `heartbeat`) as seq_highwater/dropped_before_seq/has_doorbell;
        // not yet sent on the direct-Wi-Fi publishHeartbeat() either.
        const profileMismatch = typeof obj.profile_mismatch === 'boolean' ? (obj.profile_mismatch ? 1 : 0) : null;
        // XF-127 §11 (reproduced on the bench, blocks XF-126 verification):
        // flagged as a real gap in §9 and deliberately left out of that
        // ticket's scope — XF-122 §5 ask 3 only wrote these at enrol. A
        // Thread lock learns its PID from the MCU seconds AFTER enrolling,
        // and a lock reflashed to a different profile without a full
        // re-enrol keeps reporting the new one forever while this stayed
        // frozen at NULL/stale. Same opportunistic COALESCE as every field
        // above — a lock that stops reporting doesn't blank a known value.
        const tuyaPidHb = typeof obj.tuya_pid === 'string' && obj.tuya_pid.length <= 32 ? obj.tuya_pid : null;
        const profileHb = typeof obj.profile === 'string' && obj.profile.length <= 64 ? obj.profile : null;
        // XF-134: diagnostic-only device fact, see the migration comment.
        const bonds = Number.isFinite(obj.bonds) ? obj.bonds : null;
        await pool.query(
          `UPDATE locks
              SET last_seen_at    = NOW(),
                  fw              = COALESCE(?, fw),
                  transport       = COALESCE(?, transport),
                  caps            = COALESCE(?, caps),
                  battery_pct     = COALESCE(?, battery_pct),
                  pending_uplinks = COALESCE(?, pending_uplinks),
                  last_mech_result = COALESCE(?, last_mech_result),
                  last_mech_at    = CASE WHEN ? IS NOT NULL THEN NOW() ELSE last_mech_at END,
                  roster_epoch    = COALESCE(?, roster_epoch),
                  mcu_link_up     = COALESCE(?, mcu_link_up),
                  mcu_last_frame_s = COALESCE(?, mcu_last_frame_s),
                  label           = COALESCE(?, label),
                  seq_highwater   = COALESCE(?, seq_highwater),
                  dropped_before_seq = COALESCE(?, dropped_before_seq),
                  has_doorbell    = COALESCE(?, has_doorbell),
                  profile_mismatch = COALESCE(?, profile_mismatch),
                  tuya_pid        = COALESCE(?, tuya_pid),
                  profile         = COALESCE(?, profile),
                  bonds           = COALESCE(?, bonds)
            WHERE id = ?`,
          [fw, id.transport, id.caps, batteryPct, pendingUplinks, lastMechResult, lastMechResult, rosterEpoch, mcuLinkUp, mcuLastFrameS, name, seqHighwater, droppedBeforeSeq, hasDoorbell, profileMismatch, tuyaPidHb, profileHb, bonds, deviceId]
        );
        if (droppedBeforeSeq !== null) {
          const [[riskRow]] = await pool.query(
            'SELECT dropped_before_seq, last_pulled_seq FROM locks WHERE id = ?',
            [deviceId]
          );
          const atRisk = riskRow && eventsAtRisk(riskRow);
          if (atRisk && !eventsAtRiskLogged.has(deviceId)) {
            eventsAtRiskLogged.add(deviceId);
            logEvent(
              'warn',
              `XF-125: ${deviceId} events AT RISK — rotation destroyed records up to ` +
                `seq ${riskRow.dropped_before_seq}, no app has confirmed past ` +
                `${riskRow.last_pulled_seq ?? '(never)'}`
            );
          } else if (!atRisk && eventsAtRiskLogged.has(deviceId)) {
            eventsAtRiskLogged.delete(deviceId);
            logEvent('info', `XF-125: ${deviceId} events risk cleared — a consumer caught up`);
          }
        }
        // ozkey-20 R5/R6: a heartbeat is direct proof of reachability for a
        // Wi-Fi-direct lock (no bridge_id) — set presence straight to
        // 'online' the way an R1 LWT eventually will, then let R6 compute
        // whatever reason applies (battery/mech/pending) on top of that. A
        // bridged lock's presence is NOT touched here — its reachability
        // comes from the bridge's own connect/disconnect relay, not its
        // heartbeat.
        //
        // XF-146 (2026-09-01): writes `direct_presence`, not `presence` —
        // same fix as `handleLockPresence()`, same reason: this is a raw
        // signal, and `recomputeAndStorePresence()` (called right below)
        // now reads `direct_presence` exclusively for that check, never
        // its own prior derived output.
        const [[hbLockRow]] = await pool.query('SELECT bridge_id FROM locks WHERE id = ?', [deviceId]);
        if (hbLockRow && !hbLockRow.bridge_id) {
          await pool.query("UPDATE locks SET direct_presence = 'online' WHERE id = ?", [deviceId]);
        }
        if (hbLockRow) await recomputeAndStorePresence(deviceId);
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
  // XF-122 §5 ask 3. COALESCE, not a bare overwrite: a pre-2.14 firmware
  // enrolling (or re-enrolling) sends neither field, and that must leave
  // whatever this row already knows untouched rather than blanking it —
  // same discipline as transport/caps above, for the same reason.
  const tuyaPid = typeof obj.tuya_pid === 'string' && obj.tuya_pid.length <= 32 ? obj.tuya_pid : null;
  const profile = typeof obj.profile === 'string' && obj.profile.length <= 64 ? obj.profile : null;
  await pool.query(
    `UPDATE locks SET app_id = ?, mac = ?, label = ?, fw = ?, status = 'enrolled',
       bridge_id = NULL,
       transport = COALESCE(?, transport), caps = COALESCE(?, caps),
       tuya_pid = COALESCE(?, tuya_pid), profile = COALESCE(?, profile),
       heartbeat_s = COALESCE(heartbeat_s, ?), broker_username = ?, broker_secret = ?, last_seen_at = NOW()
     WHERE id = ?`,
    [appId, mac, label, obj.fw || null, ident.transport, ident.caps, tuyaPid, profile,
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
        ? ` via bridge ${bridgeId} (bridged — no self-enroll, see ozkey-11 §3)`
        : ' — awaiting doorlock contact')
  );
  await recordAudit(appId, deviceId, 'pair', 'registered pairing');
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
    // ozkey-23 §10.2a, XF-96 §4/§6: app broker auth is NOT minted here —
    // it's a separate identity/auth flow (ozkey-24: POST /apps/register +
    // /auth/challenge + /auth/token, below), independent of pairing a
    // particular lock.
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

/* -- ozkey-24: app broker auth (REST-authenticated JWT over the app's
 *    existing identity) --------------------------------------------------
 * CORRECTED 2026-08-12 by firmware (§9.2, verified against
 * blelock/common/ozcrypto.h:414/260 before accepting): `app_id` IS the
 * X25519 public key, hex-encoded — not a bearer string alongside a
 * separate identity. There is no registration step, because `locks.app_id`
 * (written since before this file could spell "X25519", at every
 * `POST /pairings`) has been an app public-key store from day one. The
 * `apps` table and `POST /apps/register` this section used to contain are
 * gone — first written, reverted the same day, before either touched
 * production. */

const APP_PUBKEY_RE = /^[0-9a-f]{64}$/;

/** Shared by /auth/challenge and /auth/token: is this app_id known to the
 * system at all (i.e. does it appear on at least one lock's pairing), and
 * is it shaped like the X25519 public key it's supposed to be? ozkey-24
 * §9.4's caveat, live: some lab-era app_id values are NOT valid hex (e.g.
 * "admin-test-device") — reject those explicitly rather than let them
 * reach crypto.diffieHellman() and throw. */
async function findKnownAppPubkey(appId) {
  if (!APP_PUBKEY_RE.test(appId)) return { ok: false, code: 'invalid_app_id' };
  const [[row]] = await pool.query('SELECT 1 FROM locks WHERE app_id = ? LIMIT 1', [appId]);
  if (!row) return { ok: false, code: 'app_unknown' };
  return { ok: true };
}

/** ozkey-24 §3.2: step 1 of the challenge — issue a single-use nonce bound
 * to this app_id. The server's own public key rides along so the app
 * doesn't need a separate lookup to compute the ECDH shared secret. */
api.post('/auth/challenge', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const appId = req.body && req.body.app_id ? String(req.body.app_id).trim().toLowerCase() : null;
    if (!appId) return res.status(400).json({ ok: false, code: 'missing_fields', error: 'app_id is required' });

    const known = await findKnownAppPubkey(appId);
    if (!known.ok) {
      const msg =
        known.code === 'invalid_app_id'
          ? 'app_id must be 64 hex chars — it IS the X25519 public key (ozkey-24 §9.2)'
          : 'app_id not found on any paired lock — pair at least one lock first (POST /pairings)';
      return res.status(known.code === 'invalid_app_id' ? 400 : 404).json({ ok: false, code: known.code, error: msg });
    }

    pruneAuthNonces();
    const nonce = crypto.randomBytes(16).toString('hex');
    authNonces.set(nonce, { appId, expiresAt: Date.now() + CONFIG.AUTH_NONCE_TTL_MS });

    res.json({
      ok: true,
      nonce,
      expires_in: CONFIG.AUTH_NONCE_TTL_MS / 1000,
      server_pubkey: serverEcdhKeyPair.publicKeyHex,
    });
  } catch (err) {
    res.status(500).json({ ok: false, code: 'internal_error', error: err.message });
  }
});

/** ozkey-24 §3.2/§4.3(a): step 2 — verify possession of the private key
 * matching `app_id` itself via ECDH(app_priv, server_pub), HMAC'd over the
 * nonce, and issue a short-lived JWT on success. Nonce is deleted on first
 * use regardless of outcome — a failed proof does not get a second try at
 * the same nonce. */
api.post('/auth/token', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { app_id, nonce, proof } = req.body || {};
    const appId = app_id ? String(app_id).trim().toLowerCase() : null;
    if (!appId || !nonce || !proof)
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'app_id, nonce and proof are required' });

    const entry = authNonces.get(nonce);
    authNonces.delete(nonce); // single-use — consumed whether or not proof verifies
    if (!entry || entry.appId !== appId || entry.expiresAt < Date.now()) {
      return res
        .status(401)
        .json({ ok: false, code: 'invalid_or_expired_nonce', error: 'request a fresh challenge' });
    }

    const known = await findKnownAppPubkey(appId);
    if (!known.ok) return res.status(404).json({ ok: false, code: known.code, error: 'app_id no longer known' });

    let proofBuf, expectedBuf;
    try {
      const sharedSecret = crypto.diffieHellman({
        privateKey: serverEcdhKeyPair.privateKey,
        publicKey: x25519PublicKeyFromHex(appId), // app_id itself, decoded — ozkey-24 §9.2
      });
      expectedBuf = crypto.createHmac('sha256', sharedSecret).update(nonce).digest();
      proofBuf = Buffer.from(String(proof), 'hex');
    } catch (_) {
      return res.status(400).json({ ok: false, code: 'malformed_proof', error: 'proof must be hex' });
    }
    if (proofBuf.length !== expectedBuf.length || !crypto.timingSafeEqual(proofBuf, expectedBuf)) {
      return res.status(401).json({ ok: false, code: 'invalid_proof', error: 'proof did not verify' });
    }

    const token = jwt.sign({ scope: 'mqtt' }, CONFIG.JWT_SIGNING_SECRET, {
      subject: appId,
      expiresIn: CONFIG.AUTH_JWT_TTL_S,
    });
    logEvent('pair', `ozkey-24: issued MQTT JWT for app ${appId} (${CONFIG.AUTH_JWT_TTL_S}s)`);
    res.json({ ok: true, jwt: token, expires_in: CONFIG.AUTH_JWT_TTL_S });
  } catch (err) {
    res.status(500).json({ ok: false, code: 'internal_error', error: err.message });
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
/** XF-144 (2026-09-02, PM-approved): "does this lock have a confirmed-online
 * fallback bridge on record" — deterministic and cheap (`lock_bridges` +
 * `bridges_presence` only), not scan-confirmed (PM's own call: a live scan
 * per `GET /locks` call is too heavy for a list endpoint). Lets the app show
 * a "no backup bridge" warning without guessing reachability itself.
 */
async function backupAvailableSet(lockIds = null) {
  const [rows] = await pool.query(
    `SELECT DISTINCT lb.lock_id
       FROM lock_bridges lb
       JOIN bridges_presence bp ON bp.bridge_id = lb.bridge_id AND bp.presence = 'online'
      WHERE lb.priority > 0` + (lockIds ? ' AND lb.lock_id IN (?)' : ''),
    lockIds ? [lockIds] : []
  );
  return new Set(rows.map((r) => r.lock_id));
}

api.get('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    // bridge_id + caps, same as GET /locks/:id (2026-08-02). Patching only the
    // single-lock read was not enough: the app builds its lock LIST from here, so
    // a lock could report remote_unlock when opened individually and still render
    // as BLE-only in the list the user actually taps. Both reads must agree.
    // presence/presence_reason (ozkey-20 R6 verdict) exposed here so the app can
    // decide Thread-vs-BLE revoke without re-deriving liveness itself — this is
    // the field the app team asked about (2026-08-10): whether a lock is still
    // reachable via Thread/bridge before falling back to asking the user for BLE.
    const [rows] = await pool.query(
      `SELECT id, site_id, app_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
         FROM locks ORDER BY enrolled_at DESC`
    );
    const backupSet = await backupAvailableSet();
    const locks = rows.map((l) => {
      const { caps, source } = effectiveCaps(l);
      return {
        ...l,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(l),
        events_pending: eventsPending(l),
        backup_available: backupSet.has(l.id),
      };
    });
    res.json({ ok: true, locks });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** XF-144 Phase 3 (2026-09-02, App's own ask): a bridge with zero currently-
 * assigned locks (e.g. every lock behind it already failed over elsewhere)
 * is invisible to `GET /locks` — nothing there mentions a `bridge_id` no
 * lock currently points at. App derives all its bridge-health UI by joining
 * its own locally-known bridge list against this, by `bridge_id` — so this
 * exists purely to expose `bridges_presence` directly, not to be a bridge
 * registry (deliberately not what the `bridges` table is either — see its
 * own header comment). No new columns, no new derivation — same raw rows
 * `computeFaultAttribution()` already reads.
 */
api.get('/bridges', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query('SELECT bridge_id, presence, presence_at FROM bridges_presence ORDER BY bridge_id');
    res.json({ ok: true, bridges: rows });
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
      `SELECT id, app_id, site_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
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
    const backupSet = await backupAvailableSet([lock.id]);
    res.json({
      ok: true,
      lock: {
        ...lock,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(lock),
        events_pending: eventsPending(lock),
        backup_available: backupSet.has(lock.id),
      },
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** XF-144 (2026-09-01): "scan advisor," not a full pairer — deliberately
 * does NOT trigger pairing itself. Today's only pairing mechanism
 * (`pairctl`) is a BLE GATT write the PHONE performs directly against a
 * bridge; there is no MQTT-triggered pairing command on a bridge today
 * (flagged, not assumed, in this endpoint's own XF-144 thread — building
 * that is a separate, not-yet-agreed piece). This endpoint answers "which
 * bridge should you pair with," ranked by the bridge's own measured
 * reachability instead of the phone's BLE radio — the app still performs
 * the actual `pairctl` write itself, same mechanism as its already-shipped
 * RSSI-ranked flow, just fed a better signal.
 */
api.post('/locks/:id/scan-bridges', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT id FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

    const { requested, results } = await scanForLockAcrossBridges(deviceId);
    const candidates = results
      .filter((r) => r.found)
      .sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
    logEvent(
      'info',
      `Scan-bridges for ${deviceId}: asked ${requested} online bridge(s), ` +
        `${results.length} responded, ${candidates.length} report reachability`
    );
    res.json({ ok: true, device_id: deviceId, bridges_asked: requested, candidates });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.patch('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { label, power_profile, heartbeat_s, bridge_id } = req.body || {};
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
    // XF-138 P5, S6/S7: `bridge_id` used to be write-once (POST /pairings'
    // `COALESCE(VALUES(bridge_id), bridge_id)` — first value wins, never
    // clearable). That left no way to re-pair an already-registered lock to a
    // different bridge or unpair it, which the P5 allowlist flow needs.
    // `undefined` = field omitted, leave untouched; explicit `null` = unpair;
    // a non-empty string = (re)pair. No COALESCE here — this route is the one
    // place allowed to overwrite, including back to NULL.
    let bridgeIdChanged = false;
    let newBridgeId;
    if (bridge_id !== undefined) {
      if (bridge_id !== null && (typeof bridge_id !== 'string' || !bridge_id.trim()))
        return res.status(400).json({
          ok: false,
          code: 'invalid_bridge_id',
          error: 'bridge_id must be a non-empty string (to pair) or null (to unpair)',
        });
      newBridgeId = bridge_id === null ? null : bridge_id.trim().slice(0, 64);
      sets.push('bridge_id = ?');
      params.push(newBridgeId);
      bridgeIdChanged = true;
      // XF-138 (2026-08-31): `mcu_link_up`/`mcu_last_frame_s` are only ever
      // meaningful relative to the specific bridge relationship that
      // produced them — clear both on ANY bridge_id change (pair or
      // unpair), so stale telemetry from a previous bridge relationship can
      // never be misread as current under a new one.
      //
      // XF-146 (2026-09-01): `direct_presence`/`presence` reset to
      // 'unknown' for the same reason — a bridge_id change is exactly the
      // moment any prior reachability signal stops being trustworthy
      // (found live: LockA got permanently stuck `lock_unreachable` after
      // an unrelated bridge MQTT blip, with no way back short of a brand-
      // new physical reconnect; the underlying derivation bug is fixed,
      // but this endpoint is also a legitimate, reviewed way to force a
      // fresh signal for a row already stuck from before the fix existed).
      // Safe direction to err in either way: under-claiming reachability
      // until a real signal arrives beats trusting a stale one.
      sets.push(
        "mcu_link_up = NULL, mcu_last_frame_s = NULL, direct_presence = 'unknown', presence = 'unknown', presence_reason = NULL"
      );
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
    // XF-125 P0/G4: app-confirmed read progress on the lock's event log.
    // `query_events`/`events_response` is BLE-only (ozdoorlock_core.h:6103-
    // 6161, `ozNotifySealedTo()`) — invisible to this server by construction,
    // same as the door events it carries — so the app must say how far it's
    // gotten, there is no wire signal to observe instead. PATCH here rather
    // than a dedicated route: app (2026-08-21) already shipped this shape
    // best-effort against `PATCH /locks/:id`, and firmware endorsed it
    // ("PATCH on the existing endpoint is sensible, I would not invent a new
    // one") — converging on their existing contract beats introducing a
    // second, competing one for the same fact.
    const lastPulledSeq = req.body ? req.body.last_pulled_seq : undefined;
    if (lastPulledSeq !== undefined) {
      if (!Number.isFinite(Number(lastPulledSeq)) || Number(lastPulledSeq) < 0)
        return res
          .status(400)
          .json({ ok: false, code: 'invalid_last_pulled_seq', error: 'last_pulled_seq must be an integer >= 0' });
      // GREATEST, not a bare overwrite: idempotent and monotonic, so a
      // stale/out-of-order retry can never un-confirm progress a fresher
      // report already made.
      sets.push('last_pulled_seq = GREATEST(COALESCE(last_pulled_seq, 0), ?)');
      params.push(Number(lastPulledSeq));
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
    if (bridgeIdChanged) {
      logEvent(
        'pair',
        newBridgeId
          ? `Lock ${req.params.id} paired to bridge ${newBridgeId} via PATCH (XF-138 P5)`
          : `Lock ${req.params.id} unpaired (bridge_id cleared) via PATCH (XF-138 P5)`
      );
      await recordAudit(
        null,
        req.params.id,
        newBridgeId ? 'pair' : 'unpair',
        newBridgeId ? `bridge_id set to ${newBridgeId}` : 'bridge_id cleared'
      );
      // XF-144 (2026-09-01): keep `lock_bridges` in sync with every real
      // pair/re-pair, not just at migration time — otherwise the candidate
      // list is a frozen snapshot from whenever the table was created,
      // useless for any lock paired after that moment. A newly-active
      // bridge always becomes priority 0; every other known candidate for
      // this lock shifts down one, so the most recently proven-working
      // bridge is always tried first on failover, and older ones remain
      // available as fallback rather than being discarded. Only runs when
      // actually PAIRING (newBridgeId truthy) — unpairing removes the
      // active pointer but deliberately leaves history in this table, so a
      // later re-pair to a previously-known bridge doesn't lose its place
      // in the ranking.
      if (newBridgeId) {
        await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
          req.params.id,
          newBridgeId,
        ]);
        await pool.query(
          `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
             VALUES (?, ?, 0, NOW())
           ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
          [req.params.id, newBridgeId]
        );
      }
    }
    if (lastPulledSeq !== undefined) {
      const [[lock]] = await pool.query(
        'SELECT dropped_before_seq, last_pulled_seq FROM locks WHERE id = ?',
        [req.params.id]
      );
      if (!eventsAtRisk(lock) && eventsAtRiskLogged.has(req.params.id)) {
        eventsAtRiskLogged.delete(req.params.id);
        logEvent('info', `XF-125: ${req.params.id} events risk cleared — PATCH last_pulled_seq=${lock.last_pulled_seq}`);
      }
    }
    // Return the updated row (with recomputed caps) so a bridge_id change is
    // immediately visible without a second GET — the whole point of S6/S7 is
    // that remote_unlock flips the instant bridge_id does (effectiveCaps()).
    const [[updatedLock]] = await pool.query(
      `SELECT id, app_id, site_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
         FROM locks WHERE id = ?`,
      [req.params.id]
    );
    const { caps, source } = effectiveCaps(updatedLock);
    res.json({
      ok: true,
      id: req.params.id,
      lock: {
        ...updatedLock,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(updatedLock),
        events_pending: eventsPending(updatedLock),
      },
    });
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

/**
 * nexus-14 ask #3: tombstone this lock's MAC in Nexus's lock_registry on
 * removal. Best-effort and non-blocking — DELETE /locks/:id must succeed
 * from the app's point of view even if Nexus is unreachable; a missed
 * tombstone is a stale-but-safe row (nexus-14 §1: staleness only turns
 * dangerous on a REKEY going unreported, which this is not), not a reason
 * to fail the lock removal the operator is standing at the door for.
 */
async function nexusDecommission(mac) {
  if (!mac) return;
  if (!CONFIG.NEXUS_SERVER_KEY) {
    logEvent('warn', `Nexus decommission SKIPPED for mac=${mac} — OZLOCK_SERVER_API_KEY not set`);
    return;
  }
  const normalizedMac = String(mac).toLowerCase().replace(/[^0-9a-f]/g, '');
  try {
    const resp = await fetch(`${CONFIG.NEXUS_URL}/api/v1/lock-lifecycle/${normalizedMac}/decommission`, {
      method: 'POST',
      headers: { 'X-Ozlock-Server-Key': CONFIG.NEXUS_SERVER_KEY },
    });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) logEvent('info', `Nexus decommission OK for mac=${normalizedMac}`);
    else logEvent('warn', `Nexus decommission REJECTED for mac=${normalizedMac} — ${resp.status} ${body.message || ''}`);
  } catch (err) {
    logEvent('warn', `Nexus decommission unreachable for mac=${normalizedMac} — ${err.message}`);
  }
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

// ozkey-23 §5(c)'s scrubExpiredGrantNames() lived here — removed 2026-08-13
// (ozkey-29, operator instruction). It nulled `grants.user_name` post hoc,
// on revoke/expiry. There is no longer a `user_name` column to null (see
// the ozkey-29 cutover near the `grants` CREATE TABLE above) and
// `recordAudit()` no longer writes identity into `audit_log.detail` in the
// first place (§5.2/§12 below) — nothing left to redact after the fact.
// A scrub-on-schedule was already the wrong shape per ozkey-29 §1 ("policy
// sitting on top of retained access"); not writing it is the actual fix.

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

// XF-66 (BH): "delivered" is not derivable from MQTT (QoS 1 only confirms the
// broker got it, never the lock), so this is a best-effort heuristic, not a
// guarantee — labeled honestly as `likely_delivered`, matching UnlockResult's
// own "we know it left, not that the door moved" property. 2.5x heartbeat_s
// grace: a lock naps between heartbeats by design, so "no heartbeat since
// last interval" alone would call a perfectly healthy lock offline.
const RESET_DELIVERY_GRACE = 2.5;
function likelyOnline(lastSeenAt, heartbeatS) {
  if (!lastSeenAt) return false;
  const graceMs = (heartbeatS || CONFIG.DEFAULT_HEARTBEAT_S) * 1000 * RESET_DELIVERY_GRACE;
  return Date.now() - new Date(lastSeenAt).getTime() < graceMs;
}

api.delete('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const id = req.params.id;
    const { envelope_hex } = req.body || {};
    // Unpair the physical lock too (BANOI "Gỡ khoá" must reset the device,
    // not just the record): tell it to wipe NVS and return to ADVERTISING.
    // Best-effort — an offline lock misses it and needs the on-device reset.
    const [[lock]] = await conn.query(
      'SELECT site_id, bridge_id, last_seen_at, heartbeat_s, presence, presence_reason, mac, last_reset_reason FROM locks WHERE id = ?',
      [id]
    );
    let attempted = false;
    let transportOk = false;
    let likelyDelivered = false;
    let resetMsgId = null; // ozkey-41 §5.3: only the sealed branch below gets one
    // XF-119 §6/§9.3 ask 2: the lock may have ALREADY told us it reset —
    // BOOT hold, the DL-MCU's own button, or a prior request this server
    // never issued or already forgot about. That message carries no
    // msg_id to wait on and never will (ozpresence.h, deliberately — a
    // reset nobody requested has no request to name), so opening a wait
    // here would time out on a lock that has already reached the goal
    // state. Skip publishing anything and resolve immediately instead.
    const alreadyReset = !!lock && lock.last_reset_reason === 'factory_reset';
    if (lock) {
      if (alreadyReset) {
        logEvent(
          'key',
          `Factory reset SKIPPED for lock ${id} — already reset per retained presence (XF-119)`
        );
      } else if (envelope_hex) {
        // XF-91 §5 — sealed unpair, required for a Thread lock: bridge32 has
        // no `locks/+/command` subscription at all, so the plain path below
        // reaches nothing on that transport (confirmed on hardware, XF-91
        // §2/§7). Same shape as bond-revoke — queue + flushQueueForDevice
        // already route a sealed_envelope job through the bridge with
        // `target` when bridge_id is set (ozkey-11 §3).
        const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
        const [queueResult] = await pool.query(
          `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
           VALUES (?, ?, NULL, 'factory-reset', ?, 'sealed_envelope', 'queued', ?)`,
          [id, lock.site_id, envelope_hex, expiresAt]
        );
        attempted = true;
        const sent = await flushQueueForDevice(lock.site_id, id, (job, msgId) => {
          if (job.id === queueResult.insertId) resetMsgId = msgId;
        });
        transportOk = sent > 0;
        logEvent(
          'key',
          `Factory reset (sealed) queued for lock ${id} (queue #${queueResult.insertId})`
        );
      } else {
        // Legacy plain path — XF-91 §5: "can stay for now" for a Wi-Fi-direct
        // lock, reachable only because it holds its own MQTT session. Same
        // dead end as above on Thread; kept for back-compat with app builds
        // that haven't added sealing for this verb yet.
        attempted = mqttPublish(CONFIG.topicCommand(lock.site_id || CONFIG.SITE_ID, id), {
          op: 'factory_reset',
          ts: new Date().toISOString(),
        });
        transportOk = attempted;
      }
      // XF-91 (AW) / XF-92 §7 / ozkey-25 §5.5 — CORRECTED 2026-08-12.
      // The `lock.presence === 'online'` boolean collapsed presence's own
      // 'unknown' state into `false`, which this comment used to concede as
      // a known limitation ("this field alone still has to"). Firmware then
      // caught it producing a real FALSE NEGATIVE on hardware: two locks
      // that had genuinely wiped (confirmed by direct BLE INFO read —
      // name:"", transport reverted to the wifi NVS default) both reported
      // `likely_delivered:false`, because Thread liveness happened to read
      // "0 reported, 0 updated" — i.e. "we don't know" — at request time,
      // and `false` said "we know it didn't." Now three states instead of
      // two: `null` for genuinely unknown, distinct from a real negative.
      likelyDelivered = lock.bridge_id
        ? lock.presence === 'unknown'
          ? null
          : lock.presence === 'online'
        : lock.last_seen_at == null
          ? null // never contacted the server at all — no data, not a negative
          : likelyOnline(lock.last_seen_at, lock.heartbeat_s);
    }
    await conn.beginTransaction();
    await purgeLockRows(conn, 'device_id = ?', [id]);
    const [d] = await conn.query('DELETE FROM locks WHERE id = ?', [id]);
    await conn.commit();
    if (d.affectedRows === 0)
      return res.status(404).json({ ok: false, code: 'lock_not_found', error: `Lock ${id} not found` });
    if (lock && lock.mac) nexusDecommission(lock.mac); // fire-and-forget, see nexusDecommission()

    // XF-147: tell the currently-assigned bridge to drop this lock from its
    // own BLE allowlist too — without this, a decommissioned lock silently
    // reconnects on the bridge's next scan and keeps occupying a connection
    // slot as if still owned. Fire-and-forget, same as nexusDecommission()
    // above: the row is already gone server-side regardless of whether the
    // bridge is currently reachable to receive this. Deliberately
    // `unpair_lock`, NOT `unpair` — firmware confirmed bare `op:"unpair"` on
    // this topic already means "wipe the whole bridge" (bridgeOwnershipCheck
    // + factoryReset()), a real naming collision caught before this shipped.
    // Only known limitation, not solved here: if this lock was ever paired
    // to an earlier bridge that `bridge_id` no longer points to, that older
    // bridge never hears about this and keeps a stale allowlist entry — the
    // same single-column limitation XF-144's `lock_bridges` table exists
    // for elsewhere, not re-solved as a one-off here (see XF-147 Q3).
    if (lock && lock.bridge_id) {
      mqttPublish(CONFIG.topicBridgeCommand(lock.site_id || CONFIG.SITE_ID, lock.bridge_id), {
        op: 'unpair_lock',
        id,
      });
      logEvent('key', `XF-147: sent unpair_lock for ${id} to bridge ${lock.bridge_id}`);
    }

    // ozkey-41 §5/§6: wait for the real wire-level outcome — done after the
    // commit above (this server's own bookkeeping is unconditional, same as
    // before) rather than gating the DB delete on it, since how the app
    // itself should react to a denial/no_bond/timeout is still open (XF-114
    // §7.6 ask 3). `no_bond` resolves as a CONFIRMED reset (§5.2 — the lock
    // couldn't even decrypt the request because it's already unowned, which
    // for a removal is the desired end state, not a refusal).
    let verdict = null;
    let cause = null;
    if (alreadyReset) {
      verdict = 'reset_confirmed';
      cause = 'already_reset';
    } else if (transportOk && resetMsgId) {
      ({ verdict, cause } = await waitForLockResetVerdict(id, resetMsgId, CONFIG.LOCK_RESET_TIMEOUT_MS));
    }

    logEvent(
      'info',
      `Doorlock ${id} removed + factory_reset sent (attempted=${attempted} likely_delivered=${likelyDelivered} transport_ok=${transportOk}` +
        (verdict ? ` verdict=${verdict}${cause ? `/${cause}` : ''}` : '') +
        ')'
    );
    res.json({
      ok: true,
      id,
      reset: {
        attempted,
        likely_delivered: likelyDelivered,
        transport_ok: transportOk,
        presence: lock ? lock.presence : 'unknown',
        presence_reason: lock ? lock.presence_reason : null,
        verdict,
        cause,
      },
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

/* -- Grants: issue / list / revoke user keys --------------------------------- */
api.post('/locks/:id/grants', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const deviceId = req.params.id;
    // ozkey-29 cutover, 2026-08-13: `user_name`/`date_from`/`date_to` are no
    // longer accepted into server storage — see the `grants` schema note
    // above. A caller may still send them (older app builds do); they are
    // silently ignored rather than rejected, same accept-both posture S3/S4
    // used during their own cutover window. `date_from`/`date_to` were
    // structurally redundant regardless (XF-95 §2) — the lock already
    // receives and enforces its own copy inside the sealed credential
    // envelope.
    const { type = 'pin', envelope_hex, slot_number = 1 } = req.body || {};

    // S4 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08: the app
    // seals the DP frame client-side (A1-A5, ftpos shipped) — `raw_value` is
    // no longer accepted at all, not just deprioritized. The server relays
    // envelope_hex opaque, never builds or sees the PIN/RFID.
    if (!envelope_hex) {
      return res.status(400).json({
        ok: false,
        code: 'missing_fields',
        error: 'envelope_hex is required',
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

    await conn.beginTransaction();
    // S3 cutover: grants has no raw_value column anymore — the server
    // genuinely cannot know the PIN/RFID for a row it stores, structurally.
    const [grantResult] = await conn.query(
      `INSERT INTO grants (device_id, site_id, type, slot_number, sync_status)
       VALUES (?, ?, ?, ?, 'pending')`,
      [deviceId, lock.site_id, type, slot_number]
    );
    const grantId = grantResult.insertId;
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status)
       VALUES (?, ?, ?, 'grant-key', ?, 'sealed_envelope', 'queued')`,
      [deviceId, lock.site_id, grantId, envelope_hex]
    );
    await conn.commit();

    logEvent(
      'key',
      `Granted ${type.toUpperCase()} slot ${slot_number} on ${deviceId} ` +
        `(grant #${grantId}, queue #${queueResult.insertId}, sealed) — awaiting wake`
    );
    await recordAudit(lock.app_id, deviceId, 'grant', `grant ${type.toUpperCase()} slot ${slot_number}`);

    flushQueueForDevice(lock.site_id, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
      envelope_hex,
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
// XF-138 P5 (2026-08-31): CH584M locks are BLE-native and self-report
// transport:"ble" (firmware bridgeBLE-0.20). Thread support is fully
// retired — no lock uses it any more — so `'thread'` is no longer a known
// value here; a stray legacy report of it is treated the same as any other
// unrecognised value (dropped to NULL, not stored) rather than accepted.
const KNOWN_TRANSPORTS = ['wifi', 'ble'];
// XF-138/139 P5 (2026-08-31): `cred_pin` deliberately NOT here — firmware
// confirmed it never sends that literal string, only `pin_sync`
// (`if (canPin) caps.add("pin_sync")`, the actual source). `cred_pin` is
// the app's own name for the same fact (`directory_client.dart:268/273`),
// translated at the `effectiveCaps()` response layer, not accepted here as
// if the device might say it — this list is meant to reflect what firmware
// can actually report on the wire. `rfid_sync` alongside `pin_sync` for the
// same reason — `if (canRfid) caps.add("rfid_sync")`, same source. Missing
// it caused a real live bug: `deviceIdentity()`'s filter requires EVERY
// reported value to be recognised or the whole array is dropped as "nothing
// said" (its own documented all-or-nothing behavior) — a `pin_sync`-only
// whitelist silently rejected a real, otherwise-valid report the moment
// firmware's actual payload included `rfid_sync` too. Caught live before
// firmware's own flash-and-verify pass, not after.
const KNOWN_CAPS = ['remote_unlock', 'assisted_unlock', 'pin_sync', 'rfid_sync', 'audit'];

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
  //
  // XF-128 Ask 4 (firmware, 2026-08-22): `pin_sync` used to be asserted here
  // too, for ANY enrolled lock, device caps or not. Firmware found the real
  // failure mode this enables: on `tuya-luona-ds013-t3` the credential DP
  // (16, `bulk_password_add`) is `reserved` — the supplier never gave us its
  // payload layout — so PIN provisioning is not implementable on this
  // product AT ALL today, by any layer. Asserting `pin_sync` here is exactly
  // "with no word from the device," which is what let the app offer PIN
  // entry, seal a legacy DP-21 frame, and silently corrupt
  // `navigation_volume` while reporting success (XF-128 §1-4). Dropped —
  // now the same rule §2990 already states for `assisted_unlock`: a
  // capability whose enforcement/support lives in firmware is only ever
  // device-reported, never inferred. No shipped profile currently
  // advertises `pin_sync` (ozkey session-state, 2026-08-21: "every
  // credential DP is reserved"), so this now correctly reads `false`
  // everywhere until a real DP layout exists to back it.
  const inferred = bridged ? ['remote_unlock', 'audit'] : ['audit'];

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

  // XF-138/139 P5 (2026-08-31, firmware's correction): `cred_pin` is NOT a
  // name firmware has ever sent or ever will — the device only ever reports
  // `pin_sync` (`if (canPin) caps.add("pin_sync")`, confirmed against the
  // actual source, not assumed). `cred_pin` exists ONLY in the app's own
  // gate (`directory_client.dart:268/273`, `banoi_doorlock.dart:5700`) —
  // the two names were never the same field, just two different teams'
  // words for the same fact, and nothing before this relay ever put them
  // in the same room to notice. Translating here, not in storage: `caps`
  // above stays the raw, unmolested truth of what the device said (`caps`
  // storage is meant to be auditable device fact, not a translation), and
  // this is the one place — building the API response — where the two
  // vocabularies actually need to agree. No RFID-side translation yet:
  // firmware confirmed `cred_pin`'s existence but explicitly could not
  // confirm an app-facing name for RFID's equivalent — guessing one here
  // would just be a third instance of this exact bug, not a fix.
  if (device.includes('pin_sync')) caps.push('cred_pin');

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

    // XF-113 §5.1 — do not report success for an undeliverable command.
    // `lock.presence`/`presence_reason` are already kept live by LWT +
    // recomputeAndStorePresence() (bridge offline, lock off-mesh behind a
    // healthy bridge, or a Wi-Fi lock's own LWT) — this was previously never
    // read here, so a publish to an absent subscriber (fire-and-forget MQTT,
    // no error) still returned ok:true/delivered. Checked BEFORE anything is
    // queued, same reasoning as the capability gate below: a refused unlock
    // leaves no row to expire later and no audit entry implying it was
    // attempted at the door.
    if (lock.presence === 'offline') {
      logEvent(
        'warn',
        `Remote UNLOCK refused for "${lock.label}" — unreachable (${lock.presence_reason || 'offline'})`
      );
      return res.status(409).json({
        ok: false,
        code: lock.presence_reason || 'lock_unreachable',
        error: 'lock_unreachable',
        detail:
          'This doorlock is not currently reachable, so the command would never be ' +
          'delivered. ' +
          (lock.bridge_id
            ? 'Its bridge (or the lock behind it) is offline — nothing is subscribed ' +
              'to receive the command.'
            : 'It has no live network connection.') +
          ' Falling back to BLE-at-the-door is the only path that can work right now.',
        device_id: deviceId,
        presence_reason: lock.presence_reason,
        ref: 'XF-113 §5.1',
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
    //
    // XF-120 §2 step 2, 2026-08-21: `envelope_hex` is now REQUIRED. The
    // accept-both rollout shim (S10) is gone — both REST-unlock callers
    // (`unlock()`, `assistedUnlock()`) ship sealing as of `ftpos 34bc1e6`,
    // so a request with no envelope is no longer a legacy client to shim
    // for, it's exactly the unauthenticated-DP1 hole XF-120 §1.2b describes.
    // Reject it outright rather than silently building a frame for it.
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res.status(400).json({
        ok: false,
        code: 'envelope_hex_required',
        error: 'envelope_hex is required',
        detail:
          'Unsealed unlock is no longer accepted — seal {"kind":"unlock"} client-side. ' +
          'This closes the unauthenticated raw-DP path XF-120 §1.2b described.',
        ref: 'XF-120 §2',
      });
    }
    const actionType = assisted ? 'assisted-unlock' : 'unlock';
    const windowMs = assisted ? ASSISTED_UNLOCK_MS : 60_000;
    const expiresAt = new Date(Date.now() + windowMs);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, NULL, ?, ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, lock.site_id, actionType, envelope_hex, expiresAt]
    );

    logEvent(
      'key',
      `${assisted ? 'ASSISTED' : 'Remote'} UNLOCK queued for "${lock.label}" ` +
        `(queue #${queueResult.insertId}, expires ${windowMs / 1000}s` +
        `${assisted ? ', needs a keypad touch' : ''}, sealed)`
    );
    await recordAudit(lock.app_id, deviceId, 'unlock', 'remote unlock');
    const sent = await flushQueueForDevice(lock.site_id, deviceId);

    res.json({
      ok: true,
      device_id: deviceId,
      queue_id: queueResult.insertId,
      envelope_hex,
      // In the lab LockSim keeps its MQTT link open, so delivery is immediate;
      // a real eco lock would report 'queued' until its next wake.
      // KEPT for compat — the app parses this today, still live-testing.
      delivery: sent > 0 ? 'delivered' : 'queued',
      // ozkey-20 R5: "delivered must stop meaning likelyOnline... report
      // accepted/transport_ok, and let the lock's own ACK be the only thing
      // that produces executed." `sent` only means "handed to the MQTT
      // broker" — it says nothing about the lock. Added alongside `delivery`
      // rather than replacing it (same alias discipline as S12/S16); this is
      // the field to actually trust, `delivery` is the deprecated one.
      transport_ok: sent > 0,
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

/* -- Bond-verb routes (S11, ozkey-14.md 2026-08-10): bond-revoke / invite-cancel --
 * Firmware has dispatched sealed DP 101 (bond-revoke)/102 (invite-cancel) over
 * MQTT identically to BLE since ozkey-13's F1 refactor — this was always
 * deliverable, just unrouted. ftpos found the gap reading server.js before
 * building: grants is grant_id-scoped, unlock is unlock-shaped, and bond verbs
 * fit neither.
 *
 * Three deliberate differences from /unlock — do not copy its shape blindly:
 *   1. Sealed only, no accept-both, no frame builder, now or ever. A bond verb
 *      was never legal in plaintext — the lock's own plaintext path already
 *      refuses DP 101/102 (broker is anon-open). This endpoint is born at the
 *      S3/S4 end-state, not migrating to it.
 *   2. Expiry is LONG (7 days), not unlock's 60s. Access removed late is still
 *      correct; access never removed because the queue row expired first is a
 *      security failure.
 *   3. No remote_unlock capability gate. A revoke is key management — even an
 *      eco/WiFi-direct lock with no bridge still syncs it on its own wake
 *      cycle. Gate on reachability only, same check /unlock uses before its
 *      (skipped-here) capability gate.
 */
const BOND_VERB_EXPIRY_MS = 7 * 24 * 3600 * 1000;

async function handleBondVerb(req, res, actionType, label) {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'envelope_hex is required' });
    }

    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

    const reachable = lock.status === 'enrolled' || (lock.bridge_id && lock.status === 'registered');
    if (!reachable) {
      return res.status(409).json({
        ok: false,
        code: 'lock_not_enrolled',
        error: `Lock ${deviceId} is not enrolled yet (status: ${lock.status})`,
        status_now: lock.status,
      });
    }

    const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, NULL, ?, ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, lock.site_id, actionType, envelope_hex, expiresAt]
    );

    logEvent(
      'key',
      `${label} queued for "${lock.label}" (queue #${queueResult.insertId}, expires 7d)`
    );
    await recordAudit(lock.app_id, deviceId, actionType, label);
    const sent = await flushQueueForDevice(lock.site_id, deviceId);

    res.json({
      ok: true,
      device_id: deviceId,
      queue_id: queueResult.insertId,
      envelope_hex,
      delivery: sent > 0 ? 'delivered' : 'queued', // KEPT for compat, see /unlock
      transport_ok: sent > 0, // ozkey-20 R5 — the field to trust
      expires_at: expiresAt.toISOString(),
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
}

api.post('/locks/:id/bond-revoke', (req, res) => handleBondVerb(req, res, 'bond-revoke', 'Bond revoke'));
api.post('/locks/:id/invite-cancel', (req, res) =>
  handleBondVerb(req, res, 'invite-cancel', 'Invite cancel')
);
// ozkey-32 §4: a generic sealed-settings route (set_name is the first verb
// to need it, more of ozkey-28's settings verbs will follow) — server
// relays envelope_hex opaquely and never needs to know which settings verb
// is inside, so this is handleBondVerb with a new action_type, not a new
// mechanism.
api.post('/locks/:id/settings', (req, res) => handleBondVerb(req, res, 'settings', 'Settings'));

/** XF-93 (AZ) — remote bridge factory reset. `bridges` (ozkey-23 §10.2a)
 * exists now, but only for broker credentials — this route still reads and
 * writes nothing there; the audit trail is the only DB write. Deliberately
 * UNSEALED, matching firmware's own design (`bridge32.ino`, XF-93 §5): the
 * bridge holds no per-bond keys (a relay, not a crypto authority), so
 * `app_id` is checked bridge-side by `bridgeOwnershipCheck()`, not
 * cryptographically authenticated here. Built server-side rather than
 * letting the app publish directly, per XF-93 §7.3's reasoning: the handset
 * must never hold broker publish credentials, full stop — that outweighs
 * the convenience of a direct publish for an unsealed command.
 *
 * ozkey-P2, CORRECTED 2026-08-12 (courier rule, XF-84 — "the server knows
 * it sent the message; it does not know the bridge executed it"): this used
 * to return `ok:true` the moment the MQTT publish succeeded, which reports
 * the SERVER's success (the publish left) as if it were the BRIDGE's
 * (the reset happened) — exactly the class of bug XF-84 and this session's
 * `likely_delivered` fix (AW) both exist to stop. Now waits on the one real
 * signal that exists (`waitForBridgeResetVerdict()`, wired through
 * `handleBridgePresence()` above) instead of trusting the publish alone.
 *
 * `BRIDGE_DENIED` cannot be surfaced here — not a shortcut, a firmware gap,
 * confirmed by reading `bridge32.ino` (~718-730): a denied reset only
 * `Serial.printf()`s and `return`s; nothing is published over MQTT. A
 * denied reset and a reset the bridge never received are, on the wire,
 * IDENTICAL — both silence. Both correctly resolve to `unknown` here; they
 * cannot currently be told apart without a firmware change (bridge32
 * publishing something on refusal). Flagging rather than faking a
 * `BRIDGE_DENIED` verdict this server cannot actually detect.
 */
api.post('/bridges/:id/reset', async (req, res) => {
  try {
    const bridgeId = req.params.id;
    const { app_id } = req.body || {};
    if (!app_id) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'app_id is required' });
    }

    const transportOk = mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), {
      op: 'factory_reset',
      app_id,
    });
    await recordAudit(app_id, bridgeId, 'bridge-reset', 'factory reset requested');

    if (!transportOk) {
      // Never reached the broker — genuinely nothing happened, not "unknown
      // whether something happened". Distinct from the timeout case below.
      logEvent('warn', `Bridge reset for "${bridgeId}" — MQTT publish failed (broker offline)`);
      return res.json({
        ok: true,
        bridge_id: bridgeId,
        verdict: 'unknown',
        cause: 'mqtt_publish_failed',
        transport_ok: false,
      });
    }

    const { verdict, cause } = await waitForBridgeResetVerdict(bridgeId, CONFIG.BRIDGE_RESET_TIMEOUT_MS);
    const causeNote =
      cause === 'timeout'
        ? ` (no factory_reset presence signal within ${CONFIG.BRIDGE_RESET_TIMEOUT_MS}ms)`
        : cause === 'offline_unrelated_reason'
          ? ' (bridge went offline for an unrelated/ambiguous reason before confirming)'
          : cause === 'presence_denied'
            ? ' (bridge32-1.34: bridgeOwnershipCheck() refused the app_id)'
            : '';
    logEvent('key', `Bridge reset for "${bridgeId}" -> ${verdict}${causeNote}`);
    res.json({ ok: true, bridge_id: bridgeId, verdict, cause, transport_ok: true });
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
    // S4 cutover, executed 2026-08-08: envelope_hex is required — ftpos
    // shipped sealed DPID 22/24 delete frames (revokeGrant() ->
    // sealDeletePin/sealDeleteRfid), buildDeleteFrame() is gone.
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'envelope_hex is required' });
    }

    const [[grant]] = await conn.query('SELECT * FROM grants WHERE id = ? AND device_id = ?', [
      grantId,
      deviceId,
    ]);
    // XF-155 (2026-09-03): a `grants` row can legitimately be gone (purged
    // on a lock delete/re-pair cycle, or never confirmed server-side to
    // begin with — the app's own local-history fallback assigns those a
    // synthetic id) while the LOCK still holds a live credential in that
    // slot. The server's bookkeeping row isn't what protects anything here
    // — the sealed envelope is, same trust relationship the S4 cutover
    // already established for grant creation (server relays opaque, never
    // gates on its own copy). Relay through regardless of whether `grants`
    // still has a matching row; only refuse when the LOCK itself doesn't
    // exist — there's genuinely nowhere to route the frame.
    let siteId;
    if (grant) {
      if (grant.sync_status === 'revoked')
        return res.status(409).json({
          ok: false,
          code: 'grant_already_revoked',
          error: `Grant #${grantId} is already revoked`,
        });
      siteId = grant.site_id;
    } else {
      const [[lock]] = await conn.query('SELECT site_id FROM locks WHERE id = ?', [deviceId]);
      if (!lock)
        return res.status(404).json({
          ok: false,
          code: 'device_not_found',
          error: `Lock ${deviceId} not found`,
        });
      siteId = lock.site_id;
    }
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

    // XF-155 (2026-09-03): revoke-key jobs previously had no expires_at at
    // all — a job that could never be delivered (server's MQTT connection
    // down at request time, or the lock permanently gone) sat 'queued'
    // forever with nothing surfacing it as a failure. Give it the same
    // expiry shape the bond verbs already use.
    const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
    await conn.beginTransaction();
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, ?, 'revoke-key', ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, siteId, grantId, envelope_hex, expiresAt]
    );
    if (grant) {
      await conn.query("UPDATE grants SET sync_status = 'revoking' WHERE id = ?", [grantId]);
    }
    await conn.commit();

    logEvent(
      'key',
      grant
        ? `Revoking ${grant.type.toUpperCase()} on ${deviceId} slot ${grant.slot_number} ` +
            `(grant #${grantId}, queue #${queueResult.insertId}, sealed) — awaiting wake`
        : `Revoking grant #${grantId} on ${deviceId} (no matching grants row — envelope ` +
            `authoritative, queue #${queueResult.insertId}, sealed) — awaiting wake`
    );
    await recordAudit(
      null,
      deviceId,
      'revoke',
      grant
        ? `revoke ${grant.type.toUpperCase()} slot ${grant.slot_number}`
        : `revoke grant #${grantId} (no server record)`
    );

    flushQueueForDevice(siteId, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
      envelope_hex,
      sync_status: 'revoking',
      expires_at: expiresAt.toISOString(),
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

// XF-125 P1: audit_log ("security events", per the file header) was
// documented as retained per the Sovereign Edge whitepaper's 90-day target
// since 2026-07-31 — but nothing ever enforced it. Grepped clean before
// writing this: no cron, no age-based DELETE, anywhere in the prior code.
// `pending_queue` is deliberately NOT touched here — its rows already have
// their own per-command `expires_at` governing whether they still FIRE, and
// an `expired`/`sent` row is cheap, small, and useful for `GET /queue`
// debugging; only `audit_log` is named in the whitepaper's retained-data
// inventory, so only it gets a purge.
async function purgeOldAuditLog() {
  try {
    const [result] = await pool.query(
      'DELETE FROM audit_log WHERE created_at < (NOW() - INTERVAL ? SECOND)',
      [CONFIG.AUDIT_LOG_RETENTION_MS / 1000]
    );
    if (result.affectedRows > 0)
      logEvent('info', `XF-125 P1: purged ${result.affectedRows} audit_log row(s) past 90d retention`);
  } catch (err) {
    logEvent('error', `audit_log purge failed: ${err.message}`);
  }
}

/* ---------------------------------------------------------------------------
 * Boot sequence
 * ------------------------------------------------------------------------- */
async function boot() {
  logEvent('info', 'OZLOCKSERV booting — personal-cloud rendezvous directory (lab)');

  serverEcdhKeyPair = loadOrCreateServerEcdhKeyPair();
  logEvent('info', `ozkey-24: server ECDH identity ${serverEcdhKeyPair.publicKeyHex.slice(0, 12)}…`);

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

  // Run once at boot (same "never leave it stale" reasoning as
  // publishRetainedTime on connect), then on a daily cadence — retention
  // enforcement, not a liveness signal, so it doesn't need MQTT-reconnect
  // re-arming the way the time-push timer does.
  await purgeOldAuditLog();
  setInterval(purgeOldAuditLog, CONFIG.AUDIT_LOG_PURGE_INTERVAL_MS);

  app.listen(CONFIG.HTTP_PORT, () => {
    logEvent('info', `HTTP directory listening on http://localhost:${CONFIG.HTTP_PORT}/ozlockserv/api`);
  });
}

process.on('unhandledRejection', (err) => {
  logEvent('error', `Unhandled rejection: ${err && err.message ? err.message : err}`);
});

// 2026-09-03: two silent crashes tonight left `node --watch` stuck at "Failed
// running 'server.js'. Waiting for file changes before restarting..." with
// NO stack trace anywhere in the captured log — `unhandledRejection` above
// only covers a rejected promise; a synchronous throw with no catch anywhere
// on its call stack was reaching neither this file's own try/catches nor any
// handler at all, so Node's default uncaught-exception path was firing with
// its output apparently not making it into the log capture intact. Logging
// explicitly, then exiting, so the NEXT crash (if this is even the cause —
// could also be an OS-level kill, e.g. OOM, which this can't catch) at least
// leaves a real reason in `logEvent`'s own file, not just silence.
process.on('uncaughtException', (err) => {
  logEvent('error', `Uncaught exception: ${err && err.stack ? err.stack : err}`);
  process.exit(1);
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
    toSpacedHex,
    normalizeMac,
    deviceIdFromMac,
  };
}
