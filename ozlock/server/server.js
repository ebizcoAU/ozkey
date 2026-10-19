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
// S0b (ozkey-50 §7): the lock side lives in ozserver-core now; this file is
// the ozlock profile (pairing, apps/auth, grants HTTP layer, Nexus, routes).
const { createCore, util: coreUtil } = require('../../ozserver-core');
const {
  detectLanIp,
  SECRET_MIN_PROD_LEN,
  loadDotEnv,
  resolveSecret,
  enforceSecrets,
  normalizeMac,
  deviceIdFromMac,
  makeSecret,
} = coreUtil;


loadDotEnv(path.join(__dirname, '.env'));

const SECRETS = {
  DB_PASSWORD: resolveSecret(['OZLOCK_DB_PASSWORD'], { required: 'always' }),
  JWT_SIGNING_SECRET: resolveSecret(['OZLOCK_JWT_SECRET'], {
    required: 'production',
    minProdLen: SECRET_MIN_PROD_LEN,
  }),
};

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
const CONFIG = {
  // Override to spin a verification instance beside the watcher-run :4400 one.
  HTTP_PORT: Number(process.env.OZLOCK_HTTP_PORT) || 4400,
  SERVER_IP: process.env.OZLOCK_SERVER_IP || detectLanIp(),
  SITE_ID: 'lab', // single-tenant lab deployment (ozkey-05 §1.3)
  // ozkey-24: HMAC secret ozlockserv signs app broker-auth JWTs with; EMQX's
  // JWT auth plugin must hold the SAME secret to verify them. S−1: production
  // refuses to start without OZLOCK_JWT_SECRET (>= 32 chars); the 'lab-only'
  // fallback below is lab-only and logged as a warning at boot.
  JWT_SIGNING_SECRET: SECRETS.JWT_SIGNING_SECRET || 'lab-only-INSECURE-jwt-secret-CHANGE-IN-PRODUCTION',
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
    password: SECRETS.DB_PASSWORD, // S−1: OZLOCK_DB_PASSWORD, required in every mode
    // S0a: the regression suite points these at a throwaway DB and a local
    // broker; unset = the lab values, unchanged.
    database: process.env.OZLOCK_DB_NAME || 'ozlock',
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0,
  },
  MQTT_URL: process.env.OZLOCK_MQTT_URL || 'mqtt://10.1.1.42:1883',
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
 * ozserver-core (S0b): one core per process; everything it needs is here.
 * ------------------------------------------------------------------------- */
const core = createCore({ CONFIG, logEvent, mysql, mqtt });
const {
  recordAudit,
  purgeOldAuditLog,
  OZKIE_V2,
  refuseIfOzkieV2,
  flushQueueForDevice,
  eventsAtRisk,
  eventsPending,
  eventsAtRiskLogged,
  waitForBridgeResetVerdict,
  scanForLockAcrossBridges,
  waitForLockResetVerdict,
  backupAvailableSet,
  likelyOnline,
  effectiveCaps,
  mqttPublish,
  isMqttConnected,
  initMqtt,
  initDatabase,
} = core;

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
 * MySQL — the schema bootstrap is core.initDatabase() (ozserver-core/db.js);
 * routes use the pool it creates, set in boot() once the schema is up.
 * ------------------------------------------------------------------------- */
let pool = null;


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
    mqtt: isMqttConnected(),
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
async function registerPairing(appId, deviceId, label, bridgeId, ozkieVersion) {
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
  // Only written when the app says so — an older app build that never sends
  // it must not flip an existing row back to the default.
  if (ozkieVersion) await pool.query('UPDATE locks SET ozkie_version = ? WHERE id = ?', [ozkieVersion, deviceId]);
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
    const { app_id, device_id, label, bridge_id, ozkie_version } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    // ozkey-48 §7: optional, absent = leave as is (default 1 for a new row).
    if (ozkie_version !== undefined && ozkie_version !== 1 && ozkie_version !== 2) {
      return res.status(400).json({
        ok: false,
        code: 'invalid_ozkie_version',
        error: 'ozkie_version must be 1 or 2 (ozkey-48 §7)',
      });
    }
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
    await registerPairing(appId, deviceId, label, bridgeId, ozkie_version);
    // ozkey-23 §10.2a, XF-96 §4/§6: app broker auth is NOT minted here —
    // it's a separate identity/auth flow (ozkey-24: POST /apps/register +
    // /auth/challenge + /auth/token, below), independent of pairing a
    // particular lock.
    res.json({
      ok: true,
      device_id: deviceId,
      app_id: appId,
      bridge_id: bridgeId,
      ozkie_version: ozkie_version || null,
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
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
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
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
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
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
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
      'SELECT site_id, bridge_id, last_seen_at, heartbeat_s, presence, presence_reason, mac, last_reset_reason, ozkie_version FROM locks WHERE id = ?',
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
      } else if (lock.ozkie_version === OZKIE_V2) {
        // ozkey-48: neither branch below can reach a v2 lock — both send v1
        // bytes — and `dev/reset` over a relay is owner-only actuation with an
        // Echo round trip (§4.2), not built yet. Remove the record only and
        // report the reset as not attempted; the owner resets at the door.
        logEvent('warn', `Factory reset NOT sent for lock ${id} — OZKIE v2 relay not built yet (ozkey-48 §7)`);
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
    if (refuseIfOzkieV2(res, lock, deviceId, 'Grant')) return;

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


// XF-58. The "visitor at the door" unlock: the owner authorises remotely while on
// the phone, and the visitor's keypad touch completes it. 60 s — operator's call,
// and it is safe ONLY because doorlock-1.5 refuses the command without a recent
// touch. Without that lock-side check the window would be a probability rather
// than a requirement: a sleeping lock also wakes on its heartbeat timer, so a
// longer window would make an UNATTENDED open more likely, not less.
const ASSISTED_UNLOCK_MS = 60_000;


api.post('/locks/:id/unlock', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

    // ozkey-48 §6 (operator, 2026-10-18): on a v2 lock, remote unlock is OWNER
    // ONLY — members are on-premises only and never get a relay context, so
    // their relayed unlock could never execute anyway. The lock is what
    // enforces this; the server has no way to see whose context sealed an
    // OSCORE request, and this API takes the caller's word for `app_id`. This
    // check is fail-fast UX, so a member's app gets a clear answer instead of
    // a timeout, not a security boundary.
    if (lock.ozkie_version === OZKIE_V2) {
      const callerAppId = req.body && req.body.app_id ? String(req.body.app_id).trim().toLowerCase() : null;
      if (!callerAppId || callerAppId !== String(lock.app_id || '').toLowerCase()) {
        logEvent('warn', `Remote UNLOCK refused for "${lock.label}" — not the owner (OZKIE v2, ozkey-48 §6)`);
        return res.status(403).json({
          ok: false,
          code: 'remote_unlock_owner_only',
          error: 'remote_unlock_owner_only',
          detail:
            'Only the lock owner can unlock this lock remotely. Members unlock with ' +
            'Bluetooth at the door.',
          device_id: deviceId,
          ref: 'ozkey-48 §6',
        });
      }
      // §4.2: actuation is never store-and-forward — so nothing below (which
      // queues) may run for a v2 lock, even once the relay exists.
      if (refuseIfOzkieV2(res, lock, deviceId, 'Remote UNLOCK')) return;
    }

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
          'wake, not live commands. Add a bridge to enable remote unlock.',
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
    if (refuseIfOzkieV2(res, lock, deviceId, label)) return;

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
    const [[verRow]] = await conn.query('SELECT ozkie_version FROM locks WHERE id = ?', [deviceId]);
    if (refuseIfOzkieV2(res, verRow, deviceId, 'Grant revoke')) return;
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
      'OZLODGESERV instance, or on-device.',
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
  enforceSecrets(
    SECRETS.JWT_SIGNING_SECRET ? [] : ['OZLOCK_JWT_SECRET unset, using the lab-only JWT secret (refused in production)']
  );

  serverEcdhKeyPair = loadOrCreateServerEcdhKeyPair();
  logEvent('info', `ozkey-24: server ECDH identity ${serverEcdhKeyPair.publicKeyHex.slice(0, 12)}…`);

  let attempts = 0;
  for (;;) {
    try {
      await initDatabase();
      pool = core.pool;
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
