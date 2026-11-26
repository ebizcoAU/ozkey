'use strict';
/*
 * OZLOCK-AUTH-1 (CN-LAB-1 R1 + R2; PM nexus-1b GO 2026-11-26).
 *   R1  every /ozlockserv/api and /ozlock/api route needs
 *       `Authorization: Bearer <key>`, one key per client in OZLOCK_API_KEYS
 *       ("name:key,name:key"), checked before any route or DB access.
 *       GET /health is the one exemption (liveness probe for the proxy).
 *       Boot is fail-closed: no keys = refuse, unless OZLOCK_API_OPEN=1
 *       outside production. A key never appears in a log line.
 *   R2  OZLOCK_DB_USER (default root, unchanged) and OZLOCK_BIND (default
 *       all interfaces, unchanged).
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const net = require('net');
const { spawn } = require('child_process');
const { Fixture, x25519FromSeed, waitFor, S0_API_KEY, S0_API_KEY_2 } = require('./harness');

const fx = new Fixture();
const OWNER = x25519FromSeed('apiauth-owner');
const L = 'ozk-auth-lock01';
const B = 'ozb-auth-bridge';
const WRONG = 'w'.repeat(48);
const SERVER_DIR = require('path').join(__dirname, '..');

const noKey = (m, p, b) => fx.http(m, p, b, { apiKey: null });
const raw = async (m, p, headers = {}) => {
  const res = await fetch(fx.base + p, { method: m, headers });
  let body = null;
  try {
    body = await res.json();
  } catch (_) {
    /* non-JSON */
  }
  return { status: res.status, body, www: res.headers.get('www-authenticate') };
};
const count = async (t) => Number((await fx.query(`SELECT COUNT(*) AS n FROM ${t}`))[0].n);
const keysIn = (text) => [S0_API_KEY, S0_API_KEY_2, WRONG].filter((k) => text.includes(k));

// One of every route family: core lock API, relay, bridge verbs, the
// server-local auth routes, the event feed, and a route that does not exist
// (401, not 404: the check runs before routing).
const ROUTES = [
  ['GET', '/locks'],
  ['DELETE', '/locks'],
  ['GET', `/locks/${L}`],
  ['PATCH', `/locks/${L}`],
  ['DELETE', `/locks/${L}`],
  ['POST', '/pairings'],
  ['GET', '/apps'],
  ['GET', '/bridges'],
  ['GET', `/bridges/${B}`],
  ['POST', `/bridges/${B}/reset`],
  ['POST', `/locks/${L}/ozkie`],
  ['POST', `/locks/${L}/unlock`],
  ['PUT', `/locks/${L}/bridges/${B}/bond`],
  ['GET', '/queue'],
  ['GET', '/events'],
  ['POST', '/auth/challenge'],
  ['POST', '/auth/token'],
  ['GET', '/no-such-route'],
];

before(async () => {
  await fx.start();
});
after(async () => {
  await fx.stop();
});

test('setup: one paired lock (with the key)', async () => {
  await fx.pub(`ozkie/lab/bridges/${B}/presence`, { state: 'online' });
  const r = await fx.http('POST', '/pairings', { app_id: OWNER.hex, device_id: L, bridge_id: B, ozkie_version: 2 });
  assert.equal(r.status, 200, JSON.stringify(r.body));
});

test('A1: no Authorization header → 401 api_key_required on every route family, both mounts', async () => {
  for (const [m, p] of ROUTES) {
    for (const mount of ['/ozlockserv/api', '/ozlock/api']) {
      const res = await fetch(fx.base.replace('/ozlockserv/api', mount) + p, {
        method: m,
        headers: { 'content-type': 'application/json' },
        body: m === 'GET' ? undefined : JSON.stringify({ app_id: OWNER.hex, device_id: L }),
      });
      assert.equal(res.status, 401, `${m} ${mount}${p}`);
      const body = await res.json();
      assert.equal(body.code, 'api_key_required', `${m} ${mount}${p}`);
      assert.match(res.headers.get('www-authenticate') || '', /^Bearer realm="ozlock"/);
    }
  }
});

test('A2: a wrong key, a wrong scheme, or an empty bearer → 401; nothing served', async () => {
  for (const [m, p] of ROUTES) {
    const r = await raw(m, p, { authorization: `Bearer ${WRONG}` });
    assert.equal(r.status, 401, `${m} ${p}`);
    assert.equal(r.body.code, 'api_key_invalid', `${m} ${p}`);
  }
  assert.equal((await raw('GET', '/locks', { authorization: `Basic ${S0_API_KEY}` })).body.code, 'api_key_required');
  assert.equal((await raw('GET', '/locks', { authorization: S0_API_KEY })).body.code, 'api_key_required');
  assert.equal((await raw('GET', '/locks', { authorization: 'Bearer ' })).body.code, 'api_key_required');
  // A key with a prefix/suffix of a real one is not that key.
  assert.equal((await raw('GET', '/locks', { authorization: `Bearer ${S0_API_KEY}x` })).status, 401);
  assert.equal((await raw('GET', '/locks', { authorization: `Bearer ${S0_API_KEY.slice(0, -1)}` })).status, 401);
});

test('A3: each configured client key is served; the scheme is case-insensitive', async () => {
  for (const key of [S0_API_KEY, S0_API_KEY_2]) {
    const r = await fx.http('GET', `/locks/${L}`, undefined, { apiKey: key });
    assert.equal(r.status, 200, JSON.stringify(r.body));
  }
  assert.equal((await raw('GET', `/locks/${L}`, { authorization: `bearer ${S0_API_KEY}` })).status, 200);
});

test('A4: DELETE /locks and DELETE /locks/:id without a key change nothing', async () => {
  const before = { locks: await count('locks'), lb: await count('lock_bridges'), audit: await count('audit_log') };
  assert.ok(before.locks >= 1);
  assert.equal((await noKey('DELETE', '/locks')).status, 401);
  assert.equal((await noKey('DELETE', `/locks/${L}`)).status, 401);
  assert.equal((await raw('DELETE', '/locks', { authorization: `Bearer ${WRONG}` })).status, 401);
  const now = { locks: await count('locks'), lb: await count('lock_bridges'), audit: await count('audit_log') };
  assert.deepEqual(now, before);
});

test('A5: GET /health stays open (proxy liveness), on both mounts; other verbs on /health are not', async () => {
  assert.equal((await noKey('GET', '/health')).status, 200);
  const r = await fetch(fx.base.replace('/ozlockserv/api', '/ozlock/api') + '/health');
  assert.equal(r.status, 200);
  assert.equal((await noKey('POST', '/health')).status, 401);
});

test('A6: boot names the clients, refusals are logged, no key is ever printed', async () => {
  const log = fx.log();
  assert.deepEqual(keysIn(log), [], 'a key value reached the server log');
  assert.match(log, /OZLOCK-AUTH-1: API keys loaded for 2 client\(s\): s0-tests, s0-second/);
  assert.match(log, /OZLOCK-AUTH-1: refused .* api_key_invalid/);
});

/* -- Boot is fail-closed --------------------------------------------------- */

async function refusedBoot(env, want) {
  const saved = fx.extraEnv;
  fx.extraEnv = { ...saved, ...env };
  try {
    const { code, out } = await fx.bootExpectingExit();
    assert.equal(code, 1, out);
    assert.doesNotMatch(out, /HTTP directory listening/);
    assert.match(out, want);
    assert.deepEqual(keysIn(out), [], 'a refused boot printed a key');
    return out;
  } finally {
    fx.extraEnv = saved;
  }
}

test('A7: no keys and no lab opt-in → boot refused, naming the variable', async () => {
  await refusedBoot({ OZLOCK_API_KEYS: '' }, /OZLOCK_API_KEYS is missing.*OZLOCK_API_OPEN=1/);
  await fx.restartServer();
});

test('A8: malformed key lists are refused (short key, no name, bad name, duplicate name, duplicate key)', async () => {
  await refusedBoot({ OZLOCK_API_KEYS: 'short:abc' }, /OZLOCK_API_KEYS: client "short" key is shorter than 32/);
  await refusedBoot({ OZLOCK_API_KEYS: S0_API_KEY }, /OZLOCK_API_KEYS: entry 1 is not name:key/);
  await refusedBoot({ OZLOCK_API_KEYS: `Bad Name:${S0_API_KEY}` }, /OZLOCK_API_KEYS: entry 1 has an invalid client name/);
  await refusedBoot(
    { OZLOCK_API_KEYS: `a:${S0_API_KEY},a:${S0_API_KEY_2}` },
    /OZLOCK_API_KEYS: client name "a" appears twice/
  );
  await refusedBoot(
    { OZLOCK_API_KEYS: `a:${S0_API_KEY},b:${S0_API_KEY}` },
    /OZLOCK_API_KEYS: clients "a" and "b" share a key/
  );
  await refusedBoot({ OZLOCK_API_KEYS: `a:lab-${'x'.repeat(40)}` }, /OZLOCK_API_KEYS: client "a" key is a placeholder/);
  await fx.restartServer();
});

test('A9: OZLOCK_API_OPEN=1 is refused in production and when keys are also set', async () => {
  await refusedBoot({ OZLOCK_API_OPEN: '1' }, /OZLOCK_API_OPEN=1 and OZLOCK_API_KEYS are both set/);
  await refusedBoot(
    { OZLOCK_API_KEYS: '', OZLOCK_API_OPEN: '1', NODE_ENV: 'production' },
    /OZLOCK_API_OPEN=1 is not allowed in production/
  );
  await fx.restartServer();
});

test('A10: lab opt-in OZLOCK_API_OPEN=1 serves without a key and says so at boot', async () => {
  const saved = fx.extraEnv;
  fx.extraEnv = { ...saved, OZLOCK_API_KEYS: '', OZLOCK_API_OPEN: '1' };
  try {
    await fx.restartServer();
    assert.equal((await noKey('GET', `/locks/${L}`)).status, 200);
    assert.match(fx.log(), /OZLOCK_API_OPEN=1: the HTTP API is OPEN, no caller key \(lab only/);
  } finally {
    fx.extraEnv = saved;
    await fx.restartServer();
  }
  assert.equal((await noKey('GET', `/locks/${L}`)).status, 401);
});

/* -- R2: bind address and DB user ------------------------------------------ */

function lanIp() {
  for (const ifs of Object.values(require('os').networkInterfaces())) {
    for (const i of ifs || []) if (i.family === 'IPv4' && !i.internal) return i.address;
  }
  return null;
}
const canConnect = (host, port) =>
  new Promise((resolve) => {
    const s = net.connect({ host, port, timeout: 1500 });
    s.once('connect', () => (s.destroy(), resolve(true)));
    s.once('error', () => resolve(false));
    s.once('timeout', () => (s.destroy(), resolve(false)));
  });

test('A11: OZLOCK_BIND=127.0.0.1 listens on loopback only', async (t) => {
  const ip = lanIp();
  const saved = fx.extraEnv;
  fx.extraEnv = { ...saved, OZLOCK_BIND: '127.0.0.1' };
  try {
    await fx.restartServer();
    assert.match(fx.log(), new RegExp(`HTTP directory listening on http://127\\.0\\.0\\.1:${fx.httpPort}/`));
    assert.equal(await canConnect('127.0.0.1', fx.httpPort), true);
    if (!ip) t.diagnostic('no non-loopback IPv4 on this box: LAN half skipped');
    else assert.equal(await canConnect(ip, fx.httpPort), false, `reachable on ${ip}`);
  } finally {
    fx.extraEnv = saved;
    await fx.restartServer();
  }
  // Default (unset): all interfaces, as before.
  if (ip) assert.equal(await canConnect(ip, fx.httpPort), true, 'default bind no longer all interfaces');
});

test('A12: OZLOCK_DB_USER is the user ozlock connects as (default root)', async () => {
  await fx.stopServer();
  const child = spawn(process.execPath, ['server.js'], {
    cwd: SERVER_DIR,
    env: { ...fx.serverEnv(), OZLOCK_DB_USER: 'ozlock_auth1_nouser' },
  });
  let out = '';
  child.stdout.on('data', (d) => (out += d));
  child.stderr.on('data', (d) => (out += d));
  try {
    await waitFor(() => /MySQL init failed/.test(out), { timeoutMs: 30000, what: 'DB connect attempt' });
    assert.match(out, /Access denied for user 'ozlock_auth1_nouser'/);
  } finally {
    child.kill('SIGKILL');
    await fx.restartServer();
  }
  assert.equal((await fx.http('GET', `/locks/${L}`)).status, 200);
});
