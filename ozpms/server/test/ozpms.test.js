'use strict';
/*
 * S1d (ozkey-50 §7): API regression suite for ozpmsserv on ozserver-core.
 *
 * The core's lock behaviour is pinned in depth by ozlock's suite; this one
 * pins what makes ozpms ozpms, plus one walk through the shared lock path so
 * a profile wiring mistake can't hide: site 'pms', the ozkie/ozkey topic
 * move, the sealed-only relay (§8.3: no Tuya frames, no raw_value, no
 * unsealed unlock), scrypt-hashed lock broker secrets, the LEGACY door log
 * and its purge on removal, and no Nexus hook. Steps build on each other and
 * land in test/golden/ozpms.json.
 *
 *   npm test                         run + compare against the golden file
 *   OZPMS_GOLDEN_UPDATE=1 npm test   re-pin the golden file (review the diff!)
 *
 * Presence is published WITHOUT the retain flag, for the same reason as in
 * ozlock's suite (the XF-152 resubscribe would replay it mid-step).
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const mysql = require('mysql2/promise');
const { Fixture, Golden, makeSteps, x25519FromSeed, assertGolden, waitFor } = require('./harness');

const fx = new Fixture();
const golden = new Golden(path.join(__dirname, 'golden', 'ozpms.json'));
const { step, call, getLock } = makeSteps(fx, golden);

const APP = x25519FromSeed('s1-pms-app');
const L1 = 's1-pms-direct-01'; // Wi-Fi-direct lock, enrolls on ozkie/
const L2 = 's1-pms-bridged-02'; // behind a bridge
const L3 = 's1-pms-legacy-03'; // still on the July ozkey/ root
const B1 = 'ozb-s1-pms-b1';
const MAC1 = 'AA:BB:CC:01:00:01';
const MAC3 = 'AA:BB:CC:01:00:03';

const T = (kind, id) => `ozkie/pms/locks/${id}/${kind}`;
const TL = (kind, id) => `ozkey/pms/locks/${id}/${kind}`; // the legacy twin
const TB = (kind, id) => `ozkie/pms/bridges/${id}/${kind}`;

async function lockRow(id) {
  const conn = await mysql.createConnection({
    host: 'localhost',
    user: 'root',
    password: fx.dbPassword,
    database: fx.dbName,
  });
  try {
    const [[row]] = await conn.query('SELECT status, broker_username, broker_secret FROM locks WHERE id = ?', [id]);
    return row;
  } finally {
    await conn.end();
  }
}

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

/* ===========================================================================
 * Profile basics
 * ========================================================================= */
test('health: ozpmsserv on site pms, db + mqtt up', async () => {
  const { res } = await step('health', () => call('GET', '/health'));
  assert.equal(res.status, 200);
  assert.equal(res.body.service, 'ozpmsserv');
  assert.equal(res.body.site, 'pms');
  assert.equal(res.body.db, true);
  assert.equal(res.body.mqtt, true);
  const alias = await fetch(fx.base.replace('/ozpmsserv/api', '/ozpms/api') + '/health');
  assert.equal(alias.status, 200, '/ozpms/api mount answers too');
});

test('the lab-only open API announces itself', () => {
  assert.match(fx.log(), /OZPMS_LAB_OPEN_API=1: the lock API is OPEN/);
});

test('MQTT subscribes to the door-log topic (ozpms keeps door events)', () => {
  assert.match(fx.log(), /ozkie\/pms\/locks\/\+\/log \(door events, ozpmsserv keeps them\)/);
});

/* ===========================================================================
 * Pairing + enrollment, both topic roots
 * ========================================================================= */
test('POST /pairings: provision payload is site pms, Wi-Fi from env, broker from MQTT_URL', async () => {
  const { res } = await step('pairings-L1', () =>
    call('POST', '/pairings', { app_id: APP.hex, device_id: L1, label: 'Room 101' })
  );
  assert.equal(res.status, 200);
  const pp = res.body.provision_payload;
  assert.equal(pp.site_id, 'pms');
  assert.equal(pp.device_id, L1);
  assert.equal(pp.ssid, 'OZKEY-LAB', 'from PROVISION_WIFI_SSID (harness env), no literal in source');
  assert.equal(pp.password, 'labwifi-secret');
  assert.equal(pp.broker_host, '127.0.0.1');
  assert.equal(pp.broker_tcp_port, fx.mqttPort);
});

test('enroll on ozkie/: ack on ozkie/, issued_by OZPMS/, broker secret stored as scrypt', async () => {
  const { pubs } = await step('enroll-L1', async () => {
    await fx.pub(T('enroll', L1), { device_id: L1, mac: MAC1, fw: 's1-1.0', transport: 'wifi', caps: ['audit'] });
  });
  const ack = pubs.find((m) => m.topic === T('command', L1));
  assert.ok(ack, 'enrollment_ack on ozkie/pms/locks/<id>/command');
  assert.equal(ack.payload.op, 'enrollment_ack');
  assert.equal(ack.payload.site_id, 'pms');
  assert.equal(ack.payload.issued_by, 'OZPMS/');
  assert.equal(ack.payload.label, 'Room 101');
  assert.match(ack.payload.broker_secret, /^ozl_[0-9a-f]{32}$/, 'the lock gets the plaintext once');
  const row = await lockRow(L1);
  assert.equal(row.status, 'enrolled');
  assert.match(row.broker_secret, /^scrypt\$[0-9a-f]{32}\$[0-9a-f]{64}$/, 'the DB holds only the hash');
  assert.notEqual(row.broker_secret, ack.payload.broker_secret);
});

test('enroll + heartbeat on the legacy ozkey/ root still work; replies go out on ozkie/', async () => {
  let { res } = await step('pairings-L3', () => call('POST', '/pairings', { app_id: APP.hex, device_id: L3, label: 'Room 103' }));
  assert.equal(res.status, 200);
  const { pubs } = await step('enroll-L3-legacy-root', async () => {
    await fx.pub(TL('enroll', L3), { device_id: L3, mac: MAC3, fw: 's1-0.9' });
  });
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[T('command', L3), 'enrollment_ack']]);

  await step('heartbeat-L3-legacy-root', async () => {
    await fx.pub(TL('heartbeat', L3), { device_id: L3, fw: 's1-0.9', battery_pct: 64 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L3);
    return l.battery_pct === 64 ? l : null;
  }, { what: 'legacy heartbeat applied' });
  assert.equal(lock.direct_presence, 'online');
});

/* ===========================================================================
 * Bridges + the sealed relay
 * ========================================================================= */
test('bridge online -> utc push + one-time broker credentials on ozkie/pms', async () => {
  const { pubs } = await step('bridge-B1-online', async () => {
    await fx.pub(TB('presence', B1), { state: 'online' });
  });
  const ops = pubs.filter((m) => m.topic === TB('command', B1)).map((m) => m.payload.op || 'utc');
  assert.deepEqual(ops.sort(), ['broker_credentials', 'utc']);
});

test('bridged lock pairs and comes online via its bridge', async () => {
  const { res } = await step('pairings-L2', () =>
    call('POST', '/pairings', { app_id: APP.hex, device_id: L2, bridge_id: B1, label: 'Room 102' })
  );
  assert.equal(res.body.bridge_id, B1);
  await step('presence-L2-online', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B1 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.presence === 'online' ? l : null;
  }, { what: 'L2 online' });
  assert.equal(lock.presence_reason, 'ok');
});

test('§8.3: no unsealed unlock — a body without envelope_hex is refused', async () => {
  let { res } = await step('unlock-L2-no-envelope', () => call('POST', `/locks/${L2}/unlock`, {}));
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'envelope_hex_required');
  ({ res } = await step('unlock-L1-no-cap', () => call('POST', `/locks/${L1}/unlock`, { envelope_hex: 'aa' })));
  assert.equal(res.status, 409);
  assert.equal(res.body.code, 'remote_unlock_unsupported');
});

test('sealed unlock of a bridged lock is relayed via its bridge, unchanged', async () => {
  const { res, pubs } = await step('unlock-L2', () => call('POST', `/locks/${L2}/unlock`, { envelope_hex: 'c0ffee11' }));
  assert.equal(res.status, 200);
  assert.equal(res.body.delivery, 'delivered');
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].topic, TB('command', B1));
  assert.deepEqual(Object.keys(pubs[0].payload).sort(), ['envelope_hex', 'msg_id', 'target']);
  assert.equal(pubs[0].payload.envelope_hex, 'c0ffee11');
});

test('§8.3: grants take envelope_hex only — raw_value is refused, not framed', async () => {
  let { res, pubs } = await step('grant-L2-raw-value', () =>
    call('POST', `/locks/${L2}/grants`, { type: 'pin', raw_value: '123456', user_name: 'Guest' })
  );
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'missing_fields');
  assert.equal(pubs.length, 0, 'nothing built, nothing sent');

  ({ res, pubs } = await step('grant-L2-sealed', () =>
    call('POST', `/locks/${L2}/grants`, { type: 'pin', envelope_hex: 'beef0011', slot_number: 2 })
  ));
  assert.equal(res.status, 200);
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].payload.envelope_hex, 'beef0011');

  const grantId = res.body.grant_id;
  ({ res, pubs } = await step('revoke-L2-sealed', () =>
    call('DELETE', `/locks/${L2}/grants/${grantId}`, { envelope_hex: 'dead0011' })
  ));
  assert.equal(res.status, 200);
  assert.equal(res.body.sync_status, 'revoking');
  assert.equal(pubs[0].payload.envelope_hex, 'dead0011');
});

/* ===========================================================================
 * LEGACY door log (v1 Wi-Fi-direct locks only)
 * ========================================================================= */
test('legacy door log: ingested from both roots, paginated, date-filtered', async () => {
  await step('door-log-L1', async () => {
    await fx.pub(T('log', L1), { result: 'granted', detail: 'PIN slot 1', mac: MAC1, ts: '2026-10-19T01:00:00Z' });
    // The log is ordered by arrival (id). Two roots are two topics with no
    // delivery order between them, so the second waits for the first's row.
    await waitFor(async () => (await fx.http('GET', `/locks/${L1}/log`)).body.total === 1, { what: 'first door event' });
    await fx.pub(TL('log', L1), { result: 'denied', detail: 'bad PIN', mac: MAC1, ts: '2026-10-19T01:05:00Z' });
  });
  const log = await waitFor(async () => {
    const r = (await fx.http('GET', `/locks/${L1}/log`)).body;
    return r.total === 2 ? r : null;
  }, { what: 'two door events' });
  assert.deepEqual(log.log.map((e) => e.result), ['denied', 'granted'], 'newest first');

  let { res } = await step('door-log-L1-page', () => call('GET', `/locks/${L1}/log?limit=1&offset=1`));
  assert.equal(res.body.total, 2);
  assert.deepEqual(res.body.log.map((e) => e.result), ['granted']);
  ({ res } = await step('door-log-L1-future-range', () => call('GET', `/locks/${L1}/log?from=2999-01-01`)));
  assert.equal(res.body.total, 0);
  const ev = (await fx.http('GET', '/events')).body.events;
  assert.ok(ev.some((e) => /Door GRANTED — PIN slot 1 @ "Room 101" \(legacy log\)/.test(e.message)));
});

test('registry reads and app activity', async () => {
  let { res } = await step('locks-list', () => call('GET', '/locks'));
  assert.deepEqual(res.body.locks.map((l) => l.id).sort(), [L1, L2, L3].sort());
  ({ res } = await step('apps-activity', () => call('GET', `/apps/${APP.hex}/activity?limit=50`)));
  const actions = res.body.activity.map((a) => a.action);
  for (const a of ['pair', 'unlock', 'grant', 'revoke']) assert.ok(actions.includes(a), a);
});

/* ===========================================================================
 * Removal
 * ========================================================================= */
test('DELETE a direct lock: factory_reset, door log purged, no Nexus call', async () => {
  const { res, pubs } = await step('delete-L1', () => call('DELETE', `/locks/${L1}`));
  assert.equal(res.status, 200);
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[T('command', L1), 'factory_reset']]);
  const { res: logRes } = await step('door-log-L1-after-delete', () => call('GET', `/locks/${L1}/log`));
  assert.equal(logRes.body.total, 0, 'purgeExtraRows cleared lock_logs');
  assert.deepEqual(fx.nexusCalls, [], 'ozpms has no Nexus hook (ozkey-50 §9 Q7)');
});

test('DELETE a bridged lock with a sealed reset waits for the lock verdict', async () => {
  const responder = (topic, obj) => {
    if (topic === TB('command', B1) && obj.target === L2 && obj.envelope_hex === 'reset11') {
      fx.pub(T('presence', L2), { state: 'offline', reason: 'factory_reset', msg_id: obj.msg_id });
    }
  };
  fx.responders.push(responder);
  let res;
  try {
    ({ res } = await step('delete-L2-sealed', () => call('DELETE', `/locks/${L2}`, { envelope_hex: 'reset11' })));
  } finally {
    fx.responders.pop();
  }
  assert.equal(res.status, 200);
  assert.equal(res.body.reset.verdict, 'reset_confirmed');
});

/* ===========================================================================
 * Golden transcript (must run last)
 * ========================================================================= */
test('golden transcript matches', () => assertGolden(assert, golden));
