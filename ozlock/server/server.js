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
} = coreUtil;


loadDotEnv(path.join(__dirname, '.env'));

const SECRETS = {
  DB_PASSWORD: resolveSecret(['OZLOCK_DB_PASSWORD'], { required: 'always' }),
  JWT_SIGNING_SECRET: resolveSecret(['OZLOCK_JWT_SECRET'], {
    required: 'production',
    minProdLen: SECRET_MIN_PROD_LEN,
  }),
  // S1c: the Wi-Fi the provision payload hands a lock. Required in every mode,
  // no literal in source (secret, and the old SSID carried a trademark).
  PROVISION_WIFI_SSID: resolveSecret(['PROVISION_WIFI_SSID'], { required: 'always' }),
  PROVISION_WIFI_PASSWORD: resolveSecret(['PROVISION_WIFI_PASSWORD'], { required: 'always' }),
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
  // S1 (ozkey-50 §7): profile identity the core stamps on the wire — the
  // MQTT clientId prefix, the queue envelope's `source`, the enrollment
  // ack's `issued_by`.
  SERVICE_NAME: 'ozlockserv',
  ENROLL_ISSUED_BY: 'OZLOCK/',
  // XF-144 failover timing. Overridable (S0a.1) so the regression suite can
  // exercise a real failover in seconds; unset = 2 min grace / 30 s sweep.
  FAILOVER_GRACE_MS: Number(process.env.OZLOCK_FAILOVER_GRACE_MS) || 2 * 60 * 1000,
  FAILOVER_SWEEP_MS: Number(process.env.OZLOCK_FAILOVER_SWEEP_MS) || 30_000,
  // S3 + ozkey-29 cutovers: drop grants.raw_value/user_name/date_from/date_to
  // on boot. Irreversible, so it is the profile's explicit choice, never a
  // core default — the hosted relay holds nothing beyond the pairing.
  DROP_PLAINTEXT_GRANT_COLUMNS: true,
  // XF-48: refuse a remote unlock the lock's caps don't allow. OZLOCK_ENFORCE_CAPS=0
  // turns it off (lab escape hatch); anything else, including unset, enforces.
  ENFORCE_CAPS: process.env.OZLOCK_ENFORCE_CAPS !== '0',
  PROVISION_WIFI_SSID: SECRETS.PROVISION_WIFI_SSID,
  PROVISION_WIFI_PASSWORD: SECRETS.PROVISION_WIFI_PASSWORD,
  // Lock broker_secret stays plaintext here for now (ozpms hashes it). Flipping
  // it is its own step: it changes what the ozlock DB holds (ozkey-50 §7 S1e).
  HASH_LOCK_BROKER_SECRETS: false,
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
  purgeOldAuditLog,
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


/* -- The lock API (ozserver-core/api.js, S1b) --------------------------------
 * Pairing, registry reads, settings, sealed grants/unlock/bond verbs, bridge
 * reset, removal, queue, sim. OZLOCK's REST is unauthenticated today (file
 * header, "Lab simplifications"), so the auth hook is an explicit open pass;
 * the core refuses to mount without one.
 */
const ozlockOpenAccess = (_req, _res, next) => next();
core.mountLockApi(api, {
  auth: ozlockOpenAccess,
  onLockRemoved: (lock) => {
    if (lock.mac) nexusDecommission(lock.mac);
  },
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


/* -- Terminal feed: the event ring is the profile's (logEvent above) -------- */
api.get('/events', (req, res) => {
  const after = Number(req.query.after) || 0;
  res.json({ ok: true, events: eventRing.filter((e) => e.id > after) });
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
