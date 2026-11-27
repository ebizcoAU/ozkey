'use strict';
/*
 * ozkey-57 P1: ozpms is NOT transit-only (PM nexus-46, 2026-11-18: per
 * profile; property managers need the history). This row pins today's ozpms
 * retention so the ozlock changes in the shared core cannot reach it:
 * per-use audit rows are still written, and audit_log is kept 90 days.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { Fixture, x25519FromSeed, waitFor } = require('./harness');

const fx = new Fixture();
const APP = x25519FromSeed('s1-pms-retention-app');
const L = 's1-pms-ret-01';
const B1 = 'ozb-s1-pms-ret1';
const T = (kind, id) => `ozkie/pms/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/pms/bridges/${id}/${kind}`;
const getLock = async (id) => (await fx.http('GET', `/locks/${id}`)).body.lock;
const auditRows = async (action) =>
  fx.query('SELECT action, detail FROM audit_log WHERE device_id = ? AND action = ? ORDER BY id', [L, action]);

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

test('P1 setup: L paired behind B1 and online', async () => {
  await fx.pub(TB('presence', B1), { state: 'online' });
  const { status } = await fx.http('POST', '/pairings', { app_id: APP.hex, device_id: L, bridge_id: B1 });
  assert.equal(status, 200);
  await fx.pub(T('presence', L), { state: 'online', via: B1 });
  await waitFor(async () => (await getLock(L)).presence === 'online', { what: 'L online' });
});

test('P1: ozpms still writes per-use audit rows (unlock, uplink, caps_update)', async () => {
  const res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'p1unlock' });
  assert.equal(res.status, 200);
  await fx.pub(T('uplink', L), { from: L, to: APP.hex, envelope_hex: 'aabbccdd' });
  await fx.pub(T('uplink', L), { caps: ['remote_unlock', 'audit'], transport: 'ble' });
  await waitFor(async () => (await auditRows('caps_update')).length === 1, { what: 'caps_update row' });
  await waitFor(async () => (await auditRows('uplink')).length === 1, { what: 'uplink row' });
  assert.equal((await auditRows('unlock')).length, 1);
});

test('P1: ozpms keeps audit_log 90 days (a 31-day row survives a boot, a 91-day row does not)', async () => {
  await fx.query(
    "INSERT INTO audit_log (device_id, site_id, action, detail, created_at) VALUES (?, 'pms', 'revoke', 'age31', UTC_TIMESTAMP() - INTERVAL 31 DAY), (?, 'pms', 'revoke', 'age91', UTC_TIMESTAMP() - INTERVAL 91 DAY)",
    [L, L]
  );
  await fx.restartServer();
  assert.deepEqual((await auditRows('revoke')).map((r) => r.detail), ['age31']);
});
