'use strict';
/*
 * S0a.1 (ozkey-50 §7): XF-144 automatic bridge failover, the path a refactor
 * is most likely to break silently. Its own server instance and golden file
 * (test/golden/failover.json), because it needs short timers that would
 * reorder the main suite's steps:
 *
 *   OZLOCK_FAILOVER_GRACE_MS=1000   (default 2 min)
 *   OZLOCK_FAILOVER_SWEEP_MS=12000  (default 30 s)
 *
 * The tick stays longer than a scan-confirm (8 s + 3 s grace) so sweeps never
 * overlap, which the real 30 s tick also guarantees.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const { Fixture, Golden, makeSteps, x25519FromSeed, assertGolden, waitFor } = require('./harness');

const fx = new Fixture({ env: { OZLOCK_FAILOVER_GRACE_MS: '1000', OZLOCK_FAILOVER_SWEEP_MS: '12000' } });
const golden = new Golden(path.join(__dirname, 'golden', 'failover.json'));
const { step, call, getLock } = makeSteps(fx, golden);

const APP = x25519FromSeed('s0-failover-app');
const LA = 's0-fo-history-01'; // has lock_bridges history
const LB = 's0-fo-scan-02'; // never lived anywhere else: scan-confirm path
const B1 = 'ozb-s0-fo1';
const B2 = 'ozb-s0-fo2';
const B3 = 'ozb-s0-fo3';

const T = (kind, id) => `ozkie/lab/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;
const SWEEP_WAIT = { timeoutMs: 40000, everyMs: 200 };

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

test('setup: three bridges online, LA with B2 as a known fallback', async () => {
  await step('bridges-online', async () => {
    for (const b of [B1, B2, B3]) await fx.pub(TB('presence', b), { state: 'online' });
  });
  let { res } = await step('pair-LA', () => call('POST', '/pairings', { app_id: APP.hex, device_id: LA, bridge_id: B1 }));
  assert.equal(res.status, 200);
  // PATCH is what fills lock_bridges: B2 first, then back to B1 leaves
  // B1 at priority 0 and B2 at 1.
  ({ res } = await step('patch-LA-B2', () => call('PATCH', `/locks/${LA}`, { bridge_id: B2 })));
  assert.equal(res.status, 200);
  ({ res } = await step('patch-LA-B1', () => call('PATCH', `/locks/${LA}`, { bridge_id: B1 })));
  assert.equal(res.body.lock.bridge_id, B1);
  await step('presence-LA-online', async () => {
    await fx.pub(T('presence', LA), { state: 'online', via: B1 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(LA);
    return l.presence === 'online' ? l : null;
  }, { what: 'LA online' });
  assert.equal(lock.backup_available, true, 'B2 is a known, online fallback');
});

test('sustained bridge_offline fails over to the next lock_bridges candidate', async () => {
  const { pubs } = await step('failover-LA', async () => {
    await fx.pub(TB('presence', B1), { state: 'offline', reason: 'lwt' });
    await waitFor(async () => (await getLock(LA)).bridge_id === B2, { ...SWEEP_WAIT, what: 'LA moved to B2' });
  });
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[TB('command', B2), 'pair_lock']]);
  assert.equal(pubs[0].payload.id, LA);
  const lock = await getLock(LA);
  assert.equal(lock.presence, 'unknown', 'reachability is unproven until B2 reports the lock');
  assert.equal(lock.presence_reason, null);
  const { res } = await step('activity-after-LA', () => call('GET', `/apps/${APP.hex}/activity?limit=50`));
  const fo = res.body.activity.filter((a) => a.action === 'bridge_failover');
  assert.equal(fo.length, 1);
  assert.match(fo[0].detail, new RegExp(`from ${B1} to ${B2}`));
});

test('the moved lock comes back online through its new bridge', async () => {
  await step('presence-LA-via-B2', async () => {
    await fx.pub(T('presence', LA), { state: 'online', via: B2 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(LA);
    return l.presence === 'online' ? l : null;
  }, { what: 'LA online via B2' });
  assert.equal(lock.presence_reason, 'ok');
});

test('no lock_bridges history: scan-confirm finds a bridge that can see the lock', async () => {
  let { res } = await step('pair-LB', () => call('POST', '/pairings', { app_id: APP.hex, device_id: LB, bridge_id: B3 }));
  assert.equal(res.status, 200);
  await step('presence-LB-online', async () => {
    await fx.pub(T('presence', LB), { state: 'online', via: B3 });
  });
  await waitFor(async () => (await getLock(LB)).presence === 'online', { what: 'LB online' });

  // Only B2 is online now (B1 failed above, B3 is about to).
  const responder = (topic, obj) => {
    if (topic === TB('command', B2) && obj.op === 'scan' && obj.target === LB) {
      fx.pub(TB('scan_result', B2), { target: LB, found: true, rssi: -64 });
    }
  };
  fx.responders.push(responder);
  let pubs;
  try {
    ({ pubs } = await step('failover-LB-scan-confirm', async () => {
      await fx.pub(TB('presence', B3), { state: 'offline', reason: 'lwt' });
      await waitFor(async () => (await getLock(LB)).bridge_id === B2, { ...SWEEP_WAIT, what: 'LB moved to B2' });
    }));
  } finally {
    fx.responders.pop();
  }
  assert.deepEqual(
    pubs.map((m) => [m.topic, m.payload.op]).sort(),
    [
      [TB('command', B2), 'pair_lock'],
      [TB('command', B2), 'scan'],
    ]
  );
  ({ res } = await step('activity-after-LB', () => call('GET', `/apps/${APP.hex}/activity?limit=50`)));
  const fo = res.body.activity.filter((a) => a.action === 'bridge_failover' && a.device_id === LB);
  assert.equal(fo.length, 1);
  assert.match(fo[0].detail, /scan-confirm/);
});

test('golden transcript matches', () => assertGolden(assert, golden));
