'use strict';
/*
 * S1d (ozkey-50 §7): XF-144 bridge failover runs under ozpms's own config
 * (OZPMS_FAILOVER_* and site pms). The algorithm is pinned in depth by
 * ozlock/server/test/failover.test.js; this proves the ozpms profile wires
 * it up. Its own instance and golden file, because it needs short timers:
 *
 *   OZPMS_FAILOVER_GRACE_MS=1000   (default 2 min)
 *   OZPMS_FAILOVER_SWEEP_MS=12000  (default 30 s)
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const { Fixture, Golden, makeSteps, x25519FromSeed, assertGolden, waitFor } = require('./harness');

const fx = new Fixture({ env: { OZPMS_FAILOVER_GRACE_MS: '1000', OZPMS_FAILOVER_SWEEP_MS: '12000' } });
const golden = new Golden(path.join(__dirname, 'golden', 'failover.json'));
const { step, call, getLock } = makeSteps(fx, golden);

const APP = x25519FromSeed('s1-pms-failover-app');
const LA = 's1-pms-fo-01';
const B1 = 'ozb-s1-pms-fo1';
const B2 = 'ozb-s1-pms-fo2';

const T = (kind, id) => `ozkie/pms/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/pms/bridges/${id}/${kind}`;
const SWEEP_WAIT = { timeoutMs: 40000, everyMs: 200 };

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

test('setup: two bridges online, LA on B1 with B2 as a known fallback', async () => {
  await step('bridges-online', async () => {
    for (const b of [B1, B2]) await fx.pub(TB('presence', b), { state: 'online' });
  });
  let { res } = await step('pair-LA', () => call('POST', '/pairings', { app_id: APP.hex, device_id: LA, bridge_id: B1 }));
  assert.equal(res.status, 200);
  ({ res } = await step('patch-LA-B2', () => call('PATCH', `/locks/${LA}`, { bridge_id: B2 })));
  ({ res } = await step('patch-LA-B1', () => call('PATCH', `/locks/${LA}`, { bridge_id: B1 })));
  assert.equal(res.body.lock.bridge_id, B1);
  await step('presence-LA-online', async () => {
    await fx.pub(T('presence', LA), { state: 'online', via: B1 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(LA);
    return l.presence === 'online' ? l : null;
  }, { what: 'LA online' });
  assert.equal(lock.backup_available, true);
});

test('sustained bridge_offline fails over to B2 with pair_lock on ozkie/pms', async () => {
  const { pubs } = await step('failover-LA', async () => {
    await fx.pub(TB('presence', B1), { state: 'offline', reason: 'lwt' });
    await waitFor(async () => (await getLock(LA)).bridge_id === B2, { ...SWEEP_WAIT, what: 'LA moved to B2' });
  });
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[TB('command', B2), 'pair_lock']]);
  assert.equal(pubs[0].payload.id, LA);
  const { res } = await step('activity-after-LA', () => call('GET', `/apps/${APP.hex}/activity?limit=50`));
  assert.equal(res.body.activity.filter((a) => a.action === 'bridge_failover').length, 1);
});

test('golden transcript matches', () => assertGolden(assert, golden));
