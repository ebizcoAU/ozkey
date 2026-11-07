'use strict';
/*
 * Regression harness for the ozserver-core profiles (S0a for ozlock, moved
 * here and parameterized by profile in S1d, ozkey-50 §7).
 *
 * A profile's test/harness.js calls configure(profile) once and re-exports
 * this module; see ozlock/server/test/harness.js for the shape. Fields:
 *   name, serverDir, envPrefix ('OZLOCK'), basePath ('/ozlockserv/api'),
 *   site ('lab'), dbPrefix ('s0_ozlock'), serverEnv(fx) -> extra env.
 *
 * Runs the REAL server.js as a child process against fixtures it owns:
 * a private mosquitto on a free localhost port, a throwaway database
 * (<dbPrefix>_<pid>, dropped on teardown) and a stub Nexus HTTP server.
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

// The profile, and its own mqtt + mysql2 (the core has no node_modules).
let P = null;
let mqtt = null;
let mysql = null;

function configure(profile) {
  for (const k of ['name', 'serverDir', 'envPrefix', 'basePath', 'site', 'dbPrefix']) {
    if (!profile[k]) throw new Error(`testkit: profile.${k} is required`);
  }
  P = { serverEnv: () => ({}), ...profile };
  const fromProfile = (m) => require(require.resolve(m, { paths: [P.serverDir] }));
  mqtt = fromProfile('mqtt');
  mysql = fromProfile('mysql2/promise');
}

const envName = (suffix) => `${P.envPrefix}_${suffix}`;

const MOSQUITTO_CANDIDATES = [
  process.env.OZ_TEST_MOSQUITTO,
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

// The private broker's port is ephemeral. A profile that provisions locks
// with its own MQTT_URL port (ozpms) would pin it, so it becomes a token.
// A fixed port (ozlock's 1883) never equals it and stays literal.
let harnessBrokerPort = null;
const BROKER_PORT_TOKEN = '<harness-broker-port>';

function mask(value, key) {
  if (value === null || value === undefined) return value;
  if (VOLATILE_KEYS.has(key)) return MASK;
  if (key === 'broker_tcp_port' && value === harnessBrokerPort) return BROKER_PORT_TOKEN;
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
    const update = process.env[envName('GOLDEN_UPDATE')] === '1' || !fs.existsSync(this.file);
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
const OWED_MS = 5000; // longest wait for an owed reply (expectReply)

class Fixture {
  /** `env`: extra server env for this file's instance (e.g. short timers). */
  constructor({ env = {} } = {}) {
    this.extraEnv = env;
    this.extraArgs = []; // S1e-KEYGUARD: argv after server.js for the next (re)start
    if (!P) throw new Error('testkit: configure(profile) first');
    this.tmp = fs.mkdtempSync(path.join(os.tmpdir(), `s0-${P.name}-`));
    this.dbName = `${P.dbPrefix}_${process.pid}`;
    this.published = []; // server publishes, in arrival order
    this.nexusCalls = [];
    this.responders = []; // (topic, obj) => void, auto-replies as bridge/lock
    // Replies the server owes us (see expectReply); since() waits for them.
    this.owed = new Set();
    this.credentialed = new Set(); // bridges that have had broker_credentials
    this.procs = [];
  }

  async start() {
    const env = readDotEnv(path.join(P.serverDir, '.env'));
    this.dbPassword = process.env[envName('DB_PASSWORD')] || env[envName('DB_PASSWORD')];
    if (!this.dbPassword) throw new Error(`${envName('DB_PASSWORD')} not set (env or ${P.serverDir}/.env)`);

    // Broker
    this.mqttPort = await freePort();
    harnessBrokerPort = this.mqttPort;
    const conf = path.join(this.tmp, 'mosquitto.conf');
    fs.writeFileSync(conf, `listener ${this.mqttPort} 127.0.0.1\nallow_anonymous true\npersistence false\n`);
    const bin = MOSQUITTO_CANDIDATES.find((p) => fs.existsSync(p));
    if (!bin) throw new Error('mosquitto not found; set OZ_TEST_MOSQUITTO');
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

    assertLocal(envName('MQTT_URL'), this.mqttUrl);
    assertLocal('nexus stub', this.nexusUrl);

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
      this.settleOwed(topic, obj);
      for (const r of this.responders) r(topic, obj);
    });

    // S1e-4: the server pins its DB sessions to UTC, so its own TZ must not
    // matter. Run it hours EAST of MySQL, so every suite proves that (before
    // S1e-4 the harness had to match the two to pass; with the pool pin
    // reverted, the goldens and ozlock's direct-lock DELETE fail).
    this.serverTz = await zoneEastOfMysql(this.dbPassword);

    // Server
    this.httpPort = await freePort();
    this.base = `http://127.0.0.1:${this.httpPort}${P.basePath}`;
    this.serverLog = path.join(this.tmp, 'server.log');
    await this.startServer();
    await waitFor(() => this.published.some((m) => m.topic === `ozkie/${P.site}/time`), { what: 'retained time' });
  }

  /** The env every spawn of the profile's server.js gets (server and CLI). */
  serverEnv() {
    const profileEnv = P.serverEnv(this);
    for (const [k, v] of Object.entries(profileEnv)) if (/_URL$/.test(k)) assertLocal(k, v);
    return {
      PATH: process.env.PATH,
      HOME: process.env.HOME,
      TZ: this.serverTz,
      [envName('DB_PASSWORD')]: this.dbPassword,
      [envName('DB_NAME')]: this.dbName,
      [envName('MQTT_URL')]: this.mqttUrl,
      [envName('HTTP_PORT')]: String(this.httpPort),
      [envName('SERVER_IP')]: '127.0.0.1',
      // S1d: the reset-verdict waits get room on a loaded box (firmware saw
      // 'unknown' at the 5 s default with a parallel run). The responders
      // answer at once, so the lock wait only costs time when something is
      // broken. The bridge wait stays shorter: ozlock's bridge-reset-silent
      // step waits it out on purpose.
      [envName('LOCK_RESET_TIMEOUT_MS')]: '20000',
      [envName('BRIDGE_RESET_TIMEOUT_MS')]: '10000',
      // S1c: the provision payload's Wi-Fi now comes only from env. These
      // are the values the source used to carry, so the golden stays
      // byte-identical and proves the env path is what feeds it.
      PROVISION_WIFI_SSID: 'OZKEY-LAB',
      PROVISION_WIFI_PASSWORD: 'labwifi-secret',
      ...profileEnv,
      ...this.extraEnv,
    };
  }

  /** C0a: run `node server.js <args>` (a CLI subcommand) against this
   *  fixture's DB and broker; resolves { code, out }. */
  runCli(args, { timeoutMs = 60000 } = {}) {
    return new Promise((resolve) => {
      const child = spawn(process.execPath, ['server.js', ...args], { cwd: P.serverDir, env: this.serverEnv() });
      let out = '';
      child.stdout.on('data', (d) => (out += d));
      child.stderr.on('data', (d) => (out += d));
      const t = setTimeout(() => child.kill('SIGKILL'), timeoutMs);
      child.on('exit', (code) => {
        clearTimeout(t);
        resolve({ code, out });
      });
    });
  }

  /** Spawn the profile's server.js against this fixture's broker and DB. */
  async startServer() {
    const logFrom = this.log().length;
    const out = fs.openSync(this.serverLog, 'a');
    this.server = spawn(process.execPath, ['server.js', ...this.extraArgs], {
      cwd: P.serverDir,
      stdio: ['ignore', out, out],
      env: this.serverEnv(),
    });
    this.procs.push(this.server);
    const t0 = Date.now();
    try {
      await waitFor(() => /HTTP directory listening/.test(this.log().slice(logFrom)), {
        timeoutMs: 60000, // matches refuse.test.js; slow spawns under load (S1e-3 repro #3)
        what: 'server listening',
      });
    } catch (err) {
      // A start timeout used to say only "timed out" (firmware's S1e-3 repro):
      // say whether the child is alive, how loaded the box is, and what it
      // printed, so a slow spawn and a broken boot read differently.
      const s = this.server;
      const tail = this.log().slice(logFrom).split('\n').slice(-40).join('\n');
      err.message +=
        ` after ${Date.now() - t0} ms; server pid ${s.pid} exitCode=${s.exitCode} signal=${s.signalCode};` +
        ` loadavg ${os.loadavg().map((l) => l.toFixed(1)).join('/')}\n--- server log tail ---\n${tail || '(empty)'}`;
      throw err;
    }
  }

  /** S1e-KEYGUARD: stop the running server, then boot one that is expected to
   *  EXIT (a refused start) against the same DB and broker. Resolves with its
   *  exit code and what it printed; rejects if it is still up after timeoutMs.
   *  The fixture has no running server afterwards: call restartServer(). */
  async bootExpectingExit(args = [], { timeoutMs = 60000 } = {}) {
    await this.stopServer();
    const logFrom = this.log().length;
    const out = fs.openSync(this.serverLog, 'a');
    const child = spawn(process.execPath, ['server.js', ...args], {
      cwd: P.serverDir,
      stdio: ['ignore', out, out],
      env: this.serverEnv(),
    });
    this.procs.push(child);
    const code = await Promise.race([
      new Promise((r) => child.once('exit', (c) => r(c))),
      sleep(timeoutMs).then(() => 'timeout'),
    ]);
    fs.closeSync(out);
    const printed = this.log().slice(logFrom);
    if (code === 'timeout') {
      child.kill('SIGKILL');
      throw new Error(
        `server did not exit within ${timeoutMs} ms (loadavg ${os.loadavg().map((l) => l.toFixed(1)).join('/')})\n${printed}`
      );
    }
    this.procs = this.procs.filter((p) => p !== child);
    return { code, out: printed };
  }

  /** Stop only the server (no-op if it already exited or none is running). */
  async stopServer() {
    const old = this.server;
    if (!old) return;
    if (old.exitCode === null && old.signalCode === null) {
      old.kill('SIGTERM');
      await Promise.race([new Promise((r) => old.once('exit', r)), sleep(3000)]);
      if (old.exitCode === null && old.signalCode === null) old.kill('SIGKILL');
    }
    this.procs = this.procs.filter((p) => p !== old);
    this.server = null;
  }

  /** S1e-1: stop and re-spawn only the server; broker and DB stay. This is
   *  how a boot migration is tested against rows seeded by query(). */
  async restartServer() {
    await this.stopServer();
    await this.startServer();
  }

  /** Run SQL against this fixture's throwaway DB, in a UTC session like the
   *  server's pool (S1e-4). */
  async query(sql, args = []) {
    const conn = await mysql.createConnection({
      host: 'localhost',
      user: 'root',
      password: this.dbPassword,
      database: this.dbName,
      timezone: 'Z',
    });
    try {
      await conn.query("SET time_zone = '+00:00'");
      const [rows] = await conn.query(sql, args);
      return rows;
    } finally {
      await conn.end();
    }
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
    if (process.env[envName('TEST_KEEP_LOG')] !== '1') fs.rmSync(this.tmp, { recursive: true, force: true });
  }

  /** S1e-1: the server hashes a broker secret (PBKDF2, ~100 ms, more under
   *  load) before it sends the enrollment_ack or a bridge's first
   *  broker_credentials, so either reply can outlast since()'s quiet window and
   *  land in the next step. Note what a publish obliges the server to send;
   *  since() waits for it first. A refused enroll sends nothing, so the wait is
   *  bounded (OWED_MS) and an owed reply that never comes is left to the
   *  step's own asserts and the golden. */
  expectReply(topic, payload) {
    let m = /\/locks\/([^/]+)\/enroll$/.exec(topic);
    if (m) this.owed.add(`enrollment_ack:${m[1]}`);
    m = /\/bridges\/([^/]+)\/presence$/.exec(topic);
    if (m && payload && payload.state === 'online' && !this.credentialed.has(m[1])) {
      this.owed.add(`broker_credentials:${m[1]}`);
    }
  }

  settleOwed(topic, obj) {
    const m = /\/(locks|bridges)\/([^/]+)\/command$/.exec(topic);
    if (!m || !obj || typeof obj !== 'object') return;
    this.owed.delete(`${obj.op}:${m[2]}`);
    if (obj.op === 'broker_credentials') this.credentialed.add(m[2]);
  }

  /** Publish as a bridge/lock. */
  async pub(topic, payload, opts = {}) {
    if (typeof payload === 'object') this.expectReply(topic, payload);
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
    const deadline = Date.now() + OWED_MS;
    while (this.owed.size && Date.now() < deadline) await sleep(25);
    this.owed.clear();
    let last = this.published.length;
    for (;;) {
      await sleep(quietMs);
      if (this.published.length === last) break;
      last = this.published.length;
    }
    return this.published.slice(mark).filter((m) => m.topic !== `ozkie/${P.site}/time`);
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

/** An IANA zone well east of MySQL's current offset (see the comment inside). */
async function zoneEastOfMysql(password) {
  const conn = await mysql.createConnection({ host: 'localhost', user: 'root', password });
  const [[{ m }]] = await conn.query('SELECT TIMESTAMPDIFF(MINUTE, UTC_TIMESTAMP(), NOW()) AS m');
  await conn.end();
  // Put Node AHEAD of MySQL. Then a NOW()-written last_seen_at, read back as
  // Node-local by an un-pinned pool, looks hours old and likelyOnline() says
  // offline, so the suites fail without S1e-4. With Node behind (the first cut
  // of this), the skew made locks look MORE online and nothing failed.
  // UTC+14 is as far east as any zone; only MySQL at +13/+14 forces the west.
  return Number(m) < 13 * 60 ? 'Pacific/Kiritimati' : 'Etc/GMT+12';
}

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
  configure,
  Fixture,
  Golden,
  publishSet,
  makeSteps,
  x25519FromSeed,
  assertGolden,
  waitFor,
  sleep,
  assertLocal,
};
