'use strict';
/*
 * ozkey-57: ozlock is transit-only (operator ruling via PM nexus-46,
 * 2026-11-18). One slice per section; this file grows with them.
 *
 * R3: no per-use audit rows on ozlock. A remote unlock that was sent, a
 * sealed uplink and a caps report write nothing to audit_log; a REFUSED
 * unlock (failed access) and a pairing are security events and are kept.
 * The security log is kept 30 days, purged at boot and daily.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { Fixture, x25519FromSeed, waitFor } = require('./harness');

const WATCH_MS = 800;
const fx = new Fixture({ env: { OZLOCK_UNLOCK_WATCH_MS: String(WATCH_MS) } });

const APP = x25519FromSeed('s0-transitonly-app');
const L = 's0-to-bridged-01';
const B1 = 'ozb-s0-to1';
const T = (kind, id) => `ozkie/lab/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;
const getLock = async (id) => (await fx.http('GET', `/locks/${id}`)).body.lock;
const auditRows = async (action) =>
  fx.query('SELECT action, detail FROM audit_log WHERE device_id = ? AND action = ? ORDER BY id', [L, action]);

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

test('setup: B1 online, L paired behind it and online; the pairing IS in the security log', async () => {
  await fx.pub(TB('presence', B1), { state: 'online' });
  const { status } = await fx.http('POST', '/pairings', { app_id: APP.hex, device_id: L, bridge_id: B1 });
  assert.equal(status, 200);
  await fx.pub(T('presence', L), { state: 'online', via: B1 });
  await waitFor(async () => (await getLock(L)).presence === 'online', { what: 'L online' });
  assert.equal((await auditRows('pair')).length, 1);
});

test('R3: a remote unlock that was sent writes NO audit row', async () => {
  const res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 't0silent' });
  assert.equal(res.status, 200);
  assert.equal(res.body.result, 'sent_unconfirmed');
  assert.deepEqual(await auditRows('unlock'), []);
});

test('R3: a REFUSED unlock is failed access: exactly one audit row', async () => {
  const r = (topic, obj) => {
    if (topic === TB('command', B1) && obj && obj.target === L && obj.envelope_hex === 't1refuse') {
      fx.pub(T('uplink', L), { from: L, to: APP.hex, code: 'COUNTER_REPLAY' });
    }
  };
  fx.responders.push(r);
  let res;
  try {
    res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 't1refuse' });
  } finally {
    fx.responders.splice(fx.responders.indexOf(r), 1);
  }
  assert.equal(res.status, 502);
  const rows = await auditRows('unlock');
  assert.equal(rows.length, 1);
  assert.match(rows[0].detail, /refused COUNTER_REPLAY/);
});

test('R3: a sealed uplink and a caps report write NO audit row', async () => {
  await fx.pub(T('uplink', L), { from: L, to: APP.hex, envelope_hex: 'aabbccdd' });
  await fx.pub(T('uplink', L), { caps: ['remote_unlock', 'audit'], transport: 'ble' });
  await waitFor(async () => (await getLock(L)).caps_source === 'device', { what: 'caps applied' });
  await new Promise((r) => setTimeout(r, 300));
  assert.deepEqual(await auditRows('uplink'), []);
  assert.deepEqual(await auditRows('caps_update'), []);
});

test('R3: the security log is kept 30 days (a 31-day row is purged at boot, a 29-day row is kept)', async () => {
  await fx.query(
    "INSERT INTO audit_log (device_id, site_id, action, detail, created_at) VALUES (?, 'lab', 'revoke', 'age31', UTC_TIMESTAMP() - INTERVAL 31 DAY), (?, 'lab', 'revoke', 'age29', UTC_TIMESTAMP() - INTERVAL 29 DAY)",
    [L, L]
  );
  await fx.restartServer();
  const kept = (await auditRows('revoke')).map((r) => r.detail);
  assert.deepEqual(kept, ['age29']);
});

test('R3: per-use rows written before the change are swept at boot; a refused unlock is kept', async () => {
  await fx.query(
    `INSERT INTO audit_log (device_id, site_id, action, detail) VALUES
       (?, 'lab', 'unlock', 'remote unlock: sent_unconfirmed'),
       (?, 'lab', 'unlock', 'assisted unlock: queued'),
       (?, 'lab', 'uplink', 'size=12B'),
       (?, 'lab', 'caps_update', 'device-reported caps=x'),
       (?, 'lab', 'unlock', 'remote unlock: refused OLD_CODE')`,
    [L, L, L, L, L]
  );
  await fx.restartServer();
  assert.deepEqual((await auditRows('uplink')).length + (await auditRows('caps_update')).length, 0);
  const unlocks = (await auditRows('unlock')).map((r) => r.detail);
  assert.deepEqual(unlocks, ['remote unlock: refused COUNTER_REPLAY', 'remote unlock: refused OLD_CODE']);
});
