'use strict';
/*
 * Core contract tests that need no DB or broker: the fail-closed rules
 * (required CONFIG, mountLockApi's auth hook), the read-only lazy deps, and
 * the broker-secret hash format (S1e-1).
 * Behaviour of the routes themselves is pinned by ozlock/server/test.
 * One core per process, so the order of these tests matters.
 */
const test = require('node:test');
const assert = require('node:assert');
const { createCore } = require('..');
const crypto = require('crypto');
const { lazy } = require('../ctx');
const { hashBrokerSecret, isHashedBrokerSecret, verifyBrokerSecret } = require('../util');

const CONFIG = {
  SITE_ID: 'test',
  DB: { database: 'unused' },
  MQTT_URL: 'mqtt://127.0.0.1:1',
  SERVICE_NAME: 'coretest',
  ENROLL_ISSUED_BY: 'TEST/',
  DEFAULT_HEARTBEAT_S: 60,
  FAILOVER_GRACE_MS: 1000,
  FAILOVER_SWEEP_MS: 1000,
  UTC_PUSH_REFRESH_MS: 1000,
  AUDIT_LOG_RETENTION_MS: 1000,
  ENFORCE_CAPS: true,
  HASH_LOCK_BROKER_SECRETS: true,
  PROVISION_WIFI_SSID: 'test-ssid',
  PROVISION_WIFI_PASSWORD: 'test-pass',
};
const deps = { logEvent: () => {}, mysql: {}, mqtt: {} };

/** Records what mountLockApi puts on a router. */
function fakeRouter() {
  const calls = [];
  const r = { calls };
  for (const m of ['get', 'post', 'patch', 'delete']) r[m] = (path, ...h) => calls.push({ m, path, h });
  return r;
}

test('mountLockApi refuses before createCore()', () => {
  const { mountLockApi } = require('../api');
  assert.throws(() => mountLockApi(fakeRouter(), { auth: (_q, _s, n) => n() }), /before createCore/);
});

test('createCore refuses a CONFIG missing a required key', () => {
  const { ENROLL_ISSUED_BY, ...partial } = CONFIG;
  assert.throws(() => createCore({ CONFIG: partial, ...deps }), /missing ENROLL_ISSUED_BY/);
});

test('mountLockApi refuses to mount without an auth hook', () => {
  const core = createCore({ CONFIG, ...deps });
  for (const opts of [undefined, {}, { auth: null }, { auth: 'open' }]) {
    const r = fakeRouter();
    assert.throws(() => core.mountLockApi(r, opts), /needs an auth middleware/);
    assert.strictEqual(r.calls.length, 0, 'nothing mounted');
  }
});

test('mountLockApi puts the auth hook first on every route', () => {
  const core = require('../api');
  const auth = (_q, _s, n) => n();
  const r = fakeRouter();
  core.mountLockApi(r, { auth });
  assert.ok(r.calls.length > 20, `mounted ${r.calls.length} routes`);
  for (const c of r.calls) assert.strictEqual(c.h[0], auth, `${c.m} ${c.path}`);
  const paths = r.calls.map((c) => `${c.m} ${c.path}`);
  for (const p of ['post /pairings', 'post /locks/:id/unlock', 'delete /locks/:id', 'get /queue']) {
    assert.ok(paths.includes(p), p);
  }
  assert.ok(!paths.includes('get /health') && !paths.includes('get /events'), 'profile routes stay out');
});

test('createCore twice throws', () => {
  assert.throws(() => createCore({ CONFIG, ...deps }), /called twice/);
});

test('lazy deps are read-only', () => {
  const l = lazy(() => ({ a: 1 }));
  assert.strictEqual(l.a, 1);
  assert.throws(() => {
    l.b = 2;
  }, /cannot set 'b'/);
  assert.throws(() => Object.defineProperty(l, 'c', { value: 3 }), /cannot define 'c'/);
  assert.throws(() => {
    delete l.a;
  }, /cannot delete 'a'/);
});

/* S1e-1: PM-ruled format, mosquitto-go-auth's PBKDF2 string with a 64-byte key. */
test('broker secrets hash to PBKDF2$sha512$100000$<16-byte salt>$<64-byte key>', async () => {
  const h = await hashBrokerSecret('ozl_0123456789abcdef0123456789abcdef');
  const f = h.split('$');
  assert.deepStrictEqual(f.slice(0, 3), ['PBKDF2', 'sha512', '100000']);
  assert.strictEqual(Buffer.from(f[3], 'base64').length, 16);
  assert.strictEqual(Buffer.from(f[4], 'base64').length, 64);
  assert.ok(h.length <= 160, 'fits BROKER_SECRET_COLUMN');
  assert.notStrictEqual(await hashBrokerSecret('same'), await hashBrokerSecret('same'), 'salted');
});

test('verifyBrokerSecret: PBKDF2 and legacy scrypt verify; anything else never does', async () => {
  const secret = 'ozl_0123456789abcdef0123456789abcdef';
  const pbkdf2 = await hashBrokerSecret(secret);
  assert.ok(verifyBrokerSecret(secret, pbkdf2));
  assert.ok(!verifyBrokerSecret('ozl_wrong', pbkdf2));

  // The S−1 format already on ozpms rows (the old hashBrokerSecret).
  const salt = crypto.randomBytes(16);
  const scrypt = `scrypt$${salt.toString('hex')}$${crypto.scryptSync(secret, salt, 32).toString('hex')}`;
  assert.ok(verifyBrokerSecret(secret, scrypt));
  assert.ok(!verifyBrokerSecret('ozl_wrong', scrypt));

  // An independently built go-auth string (sha512, any key length) verifies.
  const goSalt = crypto.randomBytes(16);
  const goauth = `PBKDF2$sha512$1000$${goSalt.toString('base64')}$${crypto
    .pbkdf2Sync(secret, goSalt, 1000, 32, 'sha512')
    .toString('base64')}`;
  assert.ok(verifyBrokerSecret(secret, goauth));

  for (const bad of [secret, '', 'PBKDF2$sha512$0$AA==$AA==', 'PBKDF2$md5$1$AA==$AA==', 'scrypt$00', null]) {
    assert.ok(!verifyBrokerSecret(secret, bad), `rejects ${bad}`);
  }
  assert.ok(isHashedBrokerSecret(pbkdf2) && isHashedBrokerSecret(scrypt));
  assert.ok(!isHashedBrokerSecret(secret));
});
