'use strict';
/*
 * Core contract tests that need no DB or broker: the fail-closed rules
 * (required CONFIG, mountLockApi's auth hook) and the read-only lazy deps.
 * Behaviour of the routes themselves is pinned by ozlock/server/test.
 * One core per process, so the order of these tests matters.
 */
const test = require('node:test');
const assert = require('node:assert');
const { createCore } = require('..');
const { lazy } = require('../ctx');

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
