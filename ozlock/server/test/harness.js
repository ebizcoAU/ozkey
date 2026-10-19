'use strict';
/*
 * S0a (ozkey-50 §7) regression harness for ozlockserv.
 *
 * Runs the REAL server.js as a child process against fixtures it owns:
 * a private mosquitto on a free localhost port, a throwaway database
 * (s0_ozlock_<pid>, dropped on teardown) and a stub Nexus HTTP server.
 * Nothing here may reach the shared lab (10.1.1.42): assertLocal() checks
 * every URL handed to the server before it starts.
 *
 * The suite talks to the server the way the field does: HTTP as the app,
 * MQTT as the bridge and the lock. Every server publish is recorded so a
 * step can assert on it, and every step lands in a golden transcript
 * (see Golden below) that S0b must reproduce unchanged.
 */

const { spawn } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const http = require('http');
const mqtt = require('mqtt');
const mysql = require('mysql2/promise');

const SERVER_DIR = path.join(__dirname, '..');
const MOSQUITTO_CANDIDATES = [
  process.env.OZLOCK_TEST_MOSQUITTO,
  '/usr/local/sbin/mosquitto',
  '/opt/homebrew/sbin/mosquitto',
  '/usr/sbin/mosquitto',
].filter(Boolean);

/** Aborts unless `url` points at this machine (firmware's S0 note (b)). */
function assertLocal(name, url) {
  const host = new URL(url).hostname;
  if (!['127.0.0.1', 'localhost', '::1'].includes(host)) {
    throw new Error(`S0a guard: ${name}=${url} is not localhost; refusing to run against a shared host`);
  }
}

function freePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.once('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const { port } = srv.address();
      srv.close(() => resolve(port));
    });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Same KEY=VALUE format server.js reads; only used for the DB password. */
function readDotEnv(file) {
  const out = {};
  if (!fs.existsSync(file)) return out;
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/);
    if (m) out[m[1]] = m[2].replace(/^(['"])(.*)\1$/, '$2');
  }
  return out;
}

async function waitFor(predicate, { timeoutMs = 5000, everyMs = 50, what = 'condition' } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await predicate();
    if (v) return v;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await sleep(everyMs);
  }
}

/* -- Golden transcript --------------------------------------------------------
 * One entry per step: the HTTP exchange (if any) and the server's MQTT
 * publishes during that step. Volatile values are masked; publishes are
 * compared as a sorted set because one request's publishes can arrive in
 * any order (firmware's S0 note (a)).
 */
const MASK = '<masked>';
const VOLATILE_KEYS = new Set([
  'ts', 'issued_at', 'expires_at', 'created_at', 'last_seen_at', 'enrolled_at', 'presence_at',
  'last_confirmed_reachable_at', 'last_mech_at', 'updated_at', 'nonce', 'server_pubkey', 'jwt',
  'broker_secret', 'proof', 'utc', 'tz', 'uptime_s', 'server_ip', 'server_port',
]);

function mask(value, key) {
  if (value === null || value === undefined) return value;
  if (VOLATILE_KEYS.has(key)) return MASK;
  if (key === 'msg_id' && typeof value === 'string') return value.replace(/^(ozl-\d+-)\d+$/, '$1<ms>');
  if (Array.isArray(value)) return value.map((v) => mask(v, key));
  if (typeof value === 'object') {
    const out = {};
    for (const k of Object.keys(value).sort()) out[k] = mask(value[k], k);
    return out;
  }
  if (typeof value === 'string') {
    // utc=<n> inside log text, and msg ids embedded in strings
    return value.replace(/ozl-(\d+)-\d{10,}/g, 'ozl-$1-<ms>');
  }
  return value;
}

class Golden {
  constructor(file) {
    this.file = file;
    this.steps = [];
  }

  record(step) {
    this.steps.push(mask(step));
  }

  /** Returns null when it matches (or was written), else a diff summary. */
  settle() {
    const update = process.env.OZLOCK_GOLDEN_UPDATE === '1' || !fs.existsSync(this.file);
    if (update) {
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      fs.writeFileSync(this.file, JSON.stringify(this.steps, null, 2) + '\n');
      return { written: true, mismatches: [] };
    }
    const want = JSON.parse(fs.readFileSync(this.file, 'utf8'));
    const mismatches = [];
    const n = Math.max(want.length, this.steps.length);
    for (let i = 0; i < n; i++) {
      const a = JSON.stringify(want[i]);
      const b = JSON.stringify(this.steps[i]);
      if (a !== b) mismatches.push({ index: i, step: (want[i] || this.steps[i]).step, want: want[i], got: this.steps[i] });
    }
    return { written: false, mismatches };
  }
}

/* -- Fixture ----------------------------------------------------------------- */
class Fixture {
  /** `env`: extra server env for this file's instance (e.g. short timers). */
  constructor({ env = {} } = {}) {
    this.extraEnv = env;
    this.tmp = fs.mkdtempSync(path.join(os.tmpdir(), 's0-ozlock-'));
    this.dbName = `s0_ozlock_${process.pid}`;
    this.published = []; // server publishes, in arrival order
    this.nexusCalls = [];
    this.responders = []; // (topic, obj) => void, auto-replies as bridge/lock
    this.procs = [];
  }

  async start() {
    const env = readDotEnv(path.join(SERVER_DIR, '.env'));
    this.dbPassword = process.env.OZLOCK_DB_PASSWORD || env.OZLOCK_DB_PASSWORD;
    if (!this.dbPassword) throw new Error('OZLOCK_DB_PASSWORD not set (env or ozlock/server/.env)');

    // Broker
    this.mqttPort = await freePort();
    const conf = path.join(this.tmp, 'mosquitto.conf');
    fs.writeFileSync(conf, `listener ${this.mqttPort} 127.0.0.1\nallow_anonymous true\npersistence false\n`);
    const bin = MOSQUITTO_CANDIDATES.find((p) => fs.existsSync(p));
    if (!bin) throw new Error('mosquitto not found; set OZLOCK_TEST_MOSQUITTO');
    this.broker = spawn(bin, ['-c', conf], { stdio: 'ignore' });
    this.procs.push(this.broker);
    this.mqttUrl = `mqtt://127.0.0.1:${this.mqttPort}`;
    await waitFor(() => this.canConnect(this.mqttPort), { what: 'mosquitto' });

    // Stub Nexus
    this.nexus = http.createServer((req, res) => {
      this.nexusCalls.push({ method: req.method, url: req.url, key: req.headers['x-ozlock-server-key'] || null });
      res.setHeader('content-type', 'application/json');
      res.end('{"ok":true}');
    });
    await new Promise((r) => this.nexus.listen(0, '127.0.0.1', r));
    this.nexusUrl = `http://127.0.0.1:${this.nexus.address().port}`;

    assertLocal('OZLOCK_MQTT_URL', this.mqttUrl);
    assertLocal('OZLOCK_NEXUS_URL', this.nexusUrl);

    // Observer + actor: one client plays every bridge and lock.
    this.client = await mqtt.connectAsync(this.mqttUrl, { clientId: `s0-actor-${process.pid}` });
    await this.client.subscribeAsync(['ozkie/#', 'ozkey/#'], { qos: 1 });
    this.client.on('message', (topic, buf, packet) => {
      // The server only ever publishes .../command and .../time.
      if (!/\/(command|time)$/.test(topic)) return;
      let obj;
      try {
        obj = JSON.parse(buf.toString('utf8'));
      } catch (_) {
        obj = buf.toString('utf8');
      }
      const msg = { topic, payload: obj, retain: !!packet.retain };
      this.published.push(msg);
      for (const r of this.responders) r(topic, obj);
    });

    // mysql2 reads DATETIME as Node-local time, so the server must run in the
    // DB's zone or every JS-side age check (likelyOnline) is off by the gap.
    // Pinning the child to MySQL's offset keeps the transcript host-independent.
    this.serverTz = await mysqlZone(this.dbPassword);

    // Server
    this.httpPort = await freePort();
    this.base = `http://127.0.0.1:${this.httpPort}/ozlockserv/api`;
    this.serverLog = path.join(this.tmp, 'server.log');
    const out = fs.openSync(this.serverLog, 'a');
    this.server = spawn(process.execPath, ['server.js'], {
      cwd: SERVER_DIR,
      stdio: ['ignore', out, out],
      env: {
        PATH: process.env.PATH,
        HOME: process.env.HOME,
        TZ: this.serverTz,
        OZLOCK_DB_PASSWORD: this.dbPassword,
        OZLOCK_DB_NAME: this.dbName,
        OZLOCK_MQTT_URL: this.mqttUrl,
        OZLOCK_HTTP_PORT: String(this.httpPort),
        OZLOCK_SERVER_IP: '127.0.0.1',
        OZLOCK_NEXUS_URL: this.nexusUrl,
        OZLOCK_SERVER_API_KEY: 's0-test-nexus-key',
        OZLOCK_JWT_SECRET: S0_JWT_SECRET,
        ...this.extraEnv,
      },
    });
    this.procs.push(this.server);
    await waitFor(() => /HTTP directory listening/.test(this.log()), { timeoutMs: 30000, what: 'server listening' });
    await waitFor(() => this.published.some((m) => m.topic === 'ozkie/lab/time'), { what: 'retained time' });
  }

  canConnect(port) {
    return new Promise((resolve) => {
      const s = net.connect(port, '127.0.0.1', () => {
        s.end();
        resolve(true);
      });
      s.on('error', () => resolve(false));
    });
  }

  log() {
    try {
      return fs.readFileSync(this.serverLog, 'utf8');
    } catch (_) {
      return '';
    }
  }

  async stop() {
    try {
      if (this.client) await this.client.endAsync(true);
    } catch (_) {
      /* already closed */
    }
    for (const p of this.procs.reverse()) {
      if (p.exitCode === null) {
        p.kill('SIGTERM');
        await Promise.race([new Promise((r) => p.once('exit', r)), sleep(3000)]);
        if (p.exitCode === null) p.kill('SIGKILL');
      }
    }
    if (this.nexus) await new Promise((r) => this.nexus.close(r));
    if (this.dbPassword) {
      const admin = await mysql.createConnection({ host: 'localhost', user: 'root', password: this.dbPassword });
      await admin.query(`DROP DATABASE IF EXISTS \`${this.dbName}\``);
      await admin.end();
    }
    if (process.env.OZLOCK_TEST_KEEP_LOG !== '1') fs.rmSync(this.tmp, { recursive: true, force: true });
  }

  /** Publish as a bridge/lock. */
  async pub(topic, payload, opts = {}) {
    await this.client.publishAsync(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), {
      qos: 1,
      retain: !!opts.retain,
    });
  }

  mark() {
    return this.published.length;
  }

  /** Server publishes since `mark`, after the stream has been quiet `quietMs`. */
  async since(mark, { quietMs = 250, minMs = 150 } = {}) {
    await sleep(minMs);
    let last = this.published.length;
    for (;;) {
      await sleep(quietMs);
      if (this.published.length === last) break;
      last = this.published.length;
    }
    return this.published.slice(mark).filter((m) => m.topic !== 'ozkie/lab/time');
  }

  async http(method, p, body) {
    const res = await fetch(this.base + p, {
      method,
      headers: body === undefined ? {} : { 'content-type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    let json = null;
    try {
      json = await res.json();
    } catch (_) {
      /* non-JSON */
    }
    return { status: res.status, body: json };
  }
}

/** MySQL's current UTC offset as an IANA zone Node understands. */
async function mysqlZone(password) {
  const conn = await mysql.createConnection({ host: 'localhost', user: 'root', password });
  const [[{ m }]] = await conn.query('SELECT TIMESTAMPDIFF(MINUTE, UTC_TIMESTAMP(), NOW()) AS m');
  await conn.end();
  const minutes = Math.round(Number(m) / 15) * 15;
  if (minutes % 60) throw new Error(`MySQL offset ${minutes} min is not whole hours; set its time_zone`);
  const h = minutes / 60;
  return h === 0 ? 'UTC' : `Etc/GMT${h > 0 ? '-' : '+'}${Math.abs(h)}`; // Etc/ signs are inverted
}

const S0_JWT_SECRET = 's0-regression-suite-jwt-secret-not-a-real-one';

/** Sorted, masked view of a step's publishes for the golden file. */
function publishSet(msgs) {
  return msgs
    .map((m) => mask({ topic: m.topic, payload: m.payload, retain: m.retain }))
    .map((m) => JSON.stringify(m))
    .sort()
    .map((s) => JSON.parse(s));
}

/* -- Step helpers ---------------------------------------------------------------
 * `step(name, act)`: `act` performs the step (HTTP and/or MQTT) and returns
 * the HTTP exchange if any; the helper captures publishes and records the
 * golden entry. `call()` builds that exchange. */

// Lists the server orders by a timestamp (or a count) can tie within a
// second; sort those by their key so the transcript only records content.
const TIE_SORTED_LISTS = { locks: 'id', apps: 'app_id' };

function makeSteps(fx, golden) {
  async function step(name, act) {
    const mark = fx.mark();
    const exchange = await act();
    const pubs = await fx.since(mark);
    const entry = { step: name, mqtt: publishSet(pubs) };
    if (exchange && exchange.request) entry.http = exchange;
    golden.record(entry);
    return { res: exchange && exchange.response, pubs };
  }

  async function call(method, p, body) {
    const r = await fx.http(method, p, body);
    for (const [field, key] of Object.entries(TIE_SORTED_LISTS)) {
      const list = r.body && r.body[field];
      if (Array.isArray(list)) list.sort((x, y) => String(x[key]).localeCompare(String(y[key])));
    }
    return { request: { method, path: p, body: body === undefined ? null : body }, response: r };
  }

  const getLock = async (id) => (await fx.http('GET', `/locks/${id}`)).body.lock;

  return { step, call, getLock };
}

/** Deterministic X25519 identity; `hex` is the app_id (ozkey-24 §9.2). */
function x25519FromSeed(seed) {
  const raw = crypto.createHash('sha256').update(seed).digest();
  const der = Buffer.concat([Buffer.from('302e020100300506032b656e04220420', 'hex'), raw]);
  const privateKey = crypto.createPrivateKey({ key: der, format: 'der', type: 'pkcs8' });
  const x = crypto.createPublicKey(privateKey).export({ format: 'jwk' }).x;
  return { privateKey, hex: Buffer.from(x, 'base64url').toString('hex') };
}

/** Last test in a file: the transcript must match its golden file. */
function assertGolden(assert, golden) {
  const { written, mismatches } = golden.settle();
  if (written) return;
  assert.deepEqual(
    mismatches.map((m) => m.step),
    [],
    'golden drift — first mismatch:\n' + JSON.stringify(mismatches[0], null, 2)
  );
}

module.exports = {
  Fixture,
  Golden,
  publishSet,
  makeSteps,
  x25519FromSeed,
  assertGolden,
  waitFor,
  sleep,
  assertLocal,
  S0_JWT_SECRET,
};
