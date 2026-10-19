'use strict';
/*
 * S0a (ozkey-50 §7): API regression suite for today's ozlockserv.
 *
 * Steps run in order and build on each other (one server, one DB), the way a
 * lock's life does: pair, enroll, heartbeat, bridge up, unlock, grants,
 * failover, v2 gates, removal. Each step asserts the contract it covers and
 * records its HTTP exchange + MQTT publishes into test/golden/ozlock.json;
 * the last test fails if the transcript drifts from the pinned golden file.
 *
 *   npm test                       run + compare against the golden file
 *   OZLOCK_GOLDEN_UPDATE=1 npm test   re-pin the golden file (review the diff!)
 *
 * Not covered here (timer-driven, would need a shorter interval than the
 * defaults): the XF-144 failover sweep (30 s tick, 2 min grace), the 10 min
 * utc refresh, and the 90-day audit purge.
 *
 * Presence is published WITHOUT the retain flag on purpose: the server's
 * XF-152 30 s unsubscribe/resubscribe forces the broker to redeliver every
 * retained presence message, which would replay old steps into whichever
 * step happens to be running at that moment.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const path = require('path');
const jwt = require('jsonwebtoken');
const {
  Fixture,
  Golden,
  makeSteps,
  x25519FromSeed,
  assertGolden,
  waitFor,
  S0_JWT_SECRET,
} = require('./harness');

const fx = new Fixture();
const golden = new Golden(path.join(__dirname, 'golden', 'ozlock.json'));

/* -- Deterministic identities ------------------------------------------------ */
const APP_A = x25519FromSeed('s0-app-A'); // owner of L1, L2
const APP_B = x25519FromSeed('s0-app-B'); // owner of L3, squatter attempts
const L1 = 's0-direct-01'; // Wi-Fi-direct lock
const L2 = 's0-bridged-02'; // lock behind a bridge
const L3 = 's0-bridged-03'; // reports its own factory reset
const L4 = 's0-registered-04'; // direct, never enrolled
const L5 = 's0-v2-05'; // OZKIE v2 per the app
const B1 = 'ozb-s0-b1';
const B2 = 'ozb-s0-b2';
const MAC1 = 'AA:BB:CC:00:00:01';

const T = (kind, id) => `ozkie/lab/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;

const { step, call, getLock } = makeSteps(fx, golden);

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

/* ===========================================================================
 * Basics
 * ========================================================================= */
test('health reports db + mqtt up', async () => {
  const { res } = await step('health', () => call('GET', '/health'));
  assert.equal(res.status, 200);
  assert.equal(res.body.service, 'ozlockserv');
  assert.equal(res.body.site, 'lab');
  assert.equal(res.body.db, true);
  assert.equal(res.body.mqtt, true);
});

test('door log is 410 Gone', async () => {
  const { res } = await step('log-gone', () => call('GET', `/locks/${L1}/log`));
  assert.equal(res.status, 410);
  assert.equal(res.body.code, 'gone');
});

test('legacy /ozlock/api mount answers too', async () => {
  const r = await fetch(fx.base.replace('/ozlockserv/api', '/ozlock/api') + '/health');
  assert.equal(r.status, 200);
});

/* ===========================================================================
 * Pairing + enrollment
 * ========================================================================= */
test('POST /pairings validates input', async () => {
  let { res } = await step('pairings-missing', () => call('POST', '/pairings', { app_id: APP_A.hex }));
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'missing_fields');
  ({ res } = await step('pairings-bad-version', () =>
    call('POST', '/pairings', { app_id: APP_A.hex, device_id: L1, ozkie_version: 3 })
  ));
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'invalid_ozkie_version');
});

test('POST /pairings registers a direct lock and is idempotent for the same app', async () => {
  let { res } = await step('pairings-L1', () =>
    call('POST', '/pairings', { app_id: APP_A.hex, device_id: L1, label: 'Front door' })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.device_id, L1);
  assert.equal(res.body.bridge_id, null);
  assert.equal(res.body.provision_payload.site_id, 'lab');
  assert.equal(res.body.provision_payload.device_id, L1);
  ({ res } = await step('pairings-L1-again', () =>
    call('POST', '/pairings', { app_id: APP_A.hex, device_id: L1 })
  ));
  assert.equal(res.status, 200);
});

test('POST /pairings refuses a second app on a live device_id', async () => {
  const { res } = await step('pairings-squat', () =>
    call('POST', '/pairings', { app_id: APP_B.hex, device_id: L1 })
  );
  assert.equal(res.status, 409);
  assert.equal(res.body.code, 'device_paired_elsewhere');
});

test('GET /pairings/status', async () => {
  let { res } = await step('pairing-status-L1', () => call('GET', `/pairings/status?device_id=${L1}`));
  assert.equal(res.status, 200);
  assert.equal(res.body.status, 'registered');
  ({ res } = await step('pairing-status-unknown', () => call('GET', '/pairings/status?device_id=nope')));
  assert.equal(res.status, 404);
  assert.equal(res.body.code, 'pairing_not_found');
});

test('MQTT enroll of a paired lock -> enrollment_ack; unknown -> nack', async () => {
  let { pubs } = await step('enroll-L1', async () => {
    await fx.pub(T('enroll', L1), {
      device_id: L1,
      mac: MAC1,
      fw: 's0-1.0',
      transport: 'wifi',
      caps: ['audit'],
    });
  });
  const ack = pubs.find((m) => m.topic === T('command', L1));
  assert.ok(ack, 'enrollment_ack published on the lock command topic');
  assert.equal(ack.payload.op, 'enrollment_ack');
  assert.equal(ack.payload.app_id, APP_A.hex);
  // Recorded, not endorsed: the label-less re-register above wrote the
  // 'New Doorlock' default over 'Front door' (registerPairing's
  // `label || 'New Doorlock'` defeats its own COALESCE), so enroll then
  // names the lock from its MAC. Today's behaviour; flagged for S1.
  assert.equal(ack.payload.label, 'Doorlock 00:01');
  assert.match(ack.payload.broker_secret, /^ozl_[0-9a-f]{32}$/);

  ({ pubs } = await step('enroll-unknown', async () => {
    await fx.pub(T('enroll', 's0-ghost'), { device_id: 's0-ghost', mac: 'AA:BB:CC:00:00:99' });
  }));
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].payload.op, 'enrollment_nack');

  const { res } = await step('pairing-status-L1-enrolled', () => call('GET', `/pairings/status?device_id=${L1}`));
  assert.equal(res.body.status, 'enrolled');
  assert.equal(res.body.mac, MAC1);
});

test('heartbeat (legacy ozkey/ root) updates fw/battery/name and direct presence', async () => {
  await step('heartbeat-L1-legacy-root', async () => {
    await fx.pub(`ozkey/lab/locks/${L1}/heartbeat`, {
      device_id: L1,
      fw: 's0-1.1',
      battery_pct: 80,
      name: 'Front door (lock says)',
      roster_epoch: 4,
    });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L1);
    return l.fw === 's0-1.1' ? l : null;
  }, { what: 'heartbeat applied' });
  assert.equal(lock.battery_pct, 80);
  assert.equal(lock.label, 'Front door (lock says)');
  assert.equal(lock.direct_presence, 'online');
  assert.equal(lock.presence, 'online');
  assert.equal(lock.presence_reason, 'ok');
  assert.deepEqual(lock.caps, ['audit']);
  assert.equal(lock.caps_source, 'device');
});

test('deprecated /enroll/begin still registers', async () => {
  const { res } = await step('enroll-begin', () =>
    call('POST', '/enroll/begin', { app_id: APP_B.hex, device_id: L4, label: 'Shed' })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.token, L4);
});

test('POST /pairings for bridged locks', async () => {
  let { res } = await step('pairings-L2', () =>
    call('POST', '/pairings', { app_id: APP_A.hex, device_id: L2, bridge_id: B1, label: 'Back door' })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.bridge_id, B1);
  ({ res } = await step('pairings-L3', () =>
    call('POST', '/pairings', { app_id: APP_B.hex, device_id: L3, bridge_id: B1 })
  ));
  assert.equal(res.status, 200);
});

/* ===========================================================================
 * Bridges + presence
 * ========================================================================= */
test('bridge online -> utc push + one-time broker credentials', async () => {
  let { pubs } = await step('bridge-B1-online', async () => {
    await fx.pub(TB('presence', B1), { state: 'online' });
  });
  const ops = pubs.filter((m) => m.topic === TB('command', B1)).map((m) => m.payload.op || 'utc');
  assert.deepEqual(ops.sort(), ['broker_credentials', 'utc']);

  ({ pubs } = await step('bridge-B1-online-again', async () => {
    await fx.pub(TB('presence', B1), { state: 'online' });
  }));
  assert.deepEqual(pubs.map((m) => m.payload.op || 'utc'), ['utc'], 'credentials are minted once');

  const { res } = await step('bridges-list', () => call('GET', '/bridges'));
  assert.deepEqual(res.body.bridges.map((b) => [b.bridge_id, b.presence]), [[B1, 'online']]);
});

test('lock presence via its bridge -> online/ok', async () => {
  await step('presence-L2-online', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B1 });
    await fx.pub(T('presence', L3), { state: 'online', via: B1 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.presence === 'online' ? l : null;
  }, { what: 'L2 online' });
  assert.equal(lock.presence_reason, 'ok');
  assert.deepEqual(lock.caps, ['remote_unlock', 'audit']);
  assert.equal(lock.caps_source, 'inferred');
});

/* ===========================================================================
 * App auth (ozkey-24)
 * ========================================================================= */
test('auth challenge/token: X25519 proof -> JWT', async () => {
  let { res } = await step('auth-challenge-invalid', () => call('POST', '/auth/challenge', { app_id: 'not-hex' }));
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'invalid_app_id');

  const stranger = x25519FromSeed('s0-stranger');
  ({ res } = await step('auth-challenge-unknown', () => call('POST', '/auth/challenge', { app_id: stranger.hex })));
  assert.equal(res.status, 404);
  assert.equal(res.body.code, 'app_unknown');

  ({ res } = await step('auth-challenge-A', () => call('POST', '/auth/challenge', { app_id: APP_A.hex })));
  assert.equal(res.status, 200);
  const { nonce, server_pubkey } = res.body;
  const serverPub = crypto.createPublicKey({
    key: { kty: 'OKP', crv: 'X25519', x: Buffer.from(server_pubkey, 'hex').toString('base64url') },
    format: 'jwk',
  });
  const shared = crypto.diffieHellman({ privateKey: APP_A.privateKey, publicKey: serverPub });
  const proof = crypto.createHmac('sha256', shared).update(nonce).digest('hex');

  ({ res } = await step('auth-token-A', () => call('POST', '/auth/token', { app_id: APP_A.hex, nonce, proof })));
  assert.equal(res.status, 200);
  const claims = jwt.verify(res.body.jwt, S0_JWT_SECRET);
  assert.equal(claims.sub, APP_A.hex);
  assert.equal(claims.scope, 'mqtt');

  ({ res } = await step('auth-token-reused-nonce', () =>
    call('POST', '/auth/token', { app_id: APP_A.hex, nonce, proof })
  ));
  assert.equal(res.status, 401);
  assert.equal(res.body.code, 'invalid_or_expired_nonce');

  ({ res } = await step('auth-challenge-A-2', () => call('POST', '/auth/challenge', { app_id: APP_A.hex })));
  const n2 = res.body.nonce;
  ({ res } = await step('auth-token-bad-proof', () =>
    call('POST', '/auth/token', { app_id: APP_A.hex, nonce: n2, proof: '00'.repeat(32) })
  ));
  assert.equal(res.status, 401);
  assert.equal(res.body.code, 'invalid_proof');
});

/* ===========================================================================
 * Registry reads + PATCH
 * ========================================================================= */
test('registry reads', async () => {
  let { res } = await step('locks-list', () => call('GET', '/locks'));
  assert.deepEqual(res.body.locks.map((l) => l.id).sort(), [L1, L2, L3, L4].sort());
  ({ res } = await step('apps', () => call('GET', '/apps')));
  assert.equal(res.body.apps.find((a) => a.app_id === APP_A.hex).lock_count, 2);
  ({ res } = await step('apps-A-locks', () => call('GET', `/apps/${APP_A.hex}/locks`)));
  assert.deepEqual(res.body.locks.map((l) => l.id).sort(), [L1, L2].sort());
  ({ res } = await step('apps-A-activity', () => call('GET', `/apps/${APP_A.hex}/activity`)));
  assert.ok(res.body.activity.every((a) => a.app_id === APP_A.hex));
  ({ res } = await step('lock-unknown', () => call('GET', '/locks/nope')));
  assert.equal(res.status, 404);
});

test('PATCH /locks/:id', async () => {
  let { res } = await step('patch-L1', () =>
    call('PATCH', `/locks/${L1}`, { label: 'Front', power_profile: 'responsive', heartbeat_s: 5 })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.lock.label, 'Front');
  assert.equal(res.body.lock.heartbeat_s, 60, 'clamped to the firmware range');
  ({ res } = await step('patch-bad-profile', () => call('PATCH', `/locks/${L1}`, { power_profile: 'turbo' })));
  assert.equal(res.status, 400);
  ({ res } = await step('patch-nothing', () => call('PATCH', `/locks/${L1}`, {})));
  assert.equal(res.body.code, 'nothing_to_update');
  ({ res } = await step('patch-unknown', () => call('PATCH', '/locks/nope', { label: 'x' })));
  assert.equal(res.status, 404);
  ({ res } = await step('patch-L1-last-pulled', () => call('PATCH', `/locks/${L1}`, { last_pulled_seq: 7 })));
  assert.equal(res.body.lock.last_pulled_seq, 7);
});

/* ===========================================================================
 * Relay: unlock, grants, bond verbs
 * ========================================================================= */
test('unlock gates: unknown, no capability, no envelope', async () => {
  let { res } = await step('unlock-unknown', () => call('POST', '/locks/nope/unlock', { envelope_hex: 'aa' }));
  assert.equal(res.status, 404);
  ({ res } = await step('unlock-L1-no-cap', () => call('POST', `/locks/${L1}/unlock`, { envelope_hex: 'aa' })));
  assert.equal(res.status, 409);
  assert.equal(res.body.code, 'remote_unlock_unsupported');
  ({ res } = await step('unlock-L2-no-envelope', () => call('POST', `/locks/${L2}/unlock`, {})));
  assert.equal(res.status, 400);
  assert.equal(res.body.code, 'envelope_hex_required');
});

test('unlock of a bridged lock relays the sealed envelope via its bridge, unchanged', async () => {
  const { res, pubs } = await step('unlock-L2', () =>
    call('POST', `/locks/${L2}/unlock`, { envelope_hex: 'c0ffee01' })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.delivery, 'delivered');
  assert.equal(res.body.transport_ok, true);
  assert.equal(res.body.mode, 'remote');
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].topic, TB('command', B1));
  assert.deepEqual(Object.keys(pubs[0].payload).sort(), ['envelope_hex', 'msg_id', 'target']);
  assert.equal(pubs[0].payload.envelope_hex, 'c0ffee01');
  assert.equal(pubs[0].payload.target, L2);
});

test('grants: validation, issue via bridge, list', async () => {
  let { res } = await step('grant-no-envelope', () => call('POST', `/locks/${L2}/grants`, { type: 'pin' }));
  assert.equal(res.status, 400);
  ({ res } = await step('grant-fingerprint', () =>
    call('POST', `/locks/${L2}/grants`, { type: 'fingerprint', envelope_hex: 'aa' })
  ));
  assert.equal(res.status, 422);
  ({ res } = await step('grant-bad-type', () =>
    call('POST', `/locks/${L2}/grants`, { type: 'retina', envelope_hex: 'aa' })
  ));
  assert.equal(res.status, 400);
  ({ res } = await step('grant-unknown-lock', () =>
    call('POST', '/locks/nope/grants', { type: 'pin', envelope_hex: 'aa' })
  ));
  assert.equal(res.status, 404);

  let pubs;
  ({ res, pubs } = await step('grant-L2-pin', () =>
    call('POST', `/locks/${L2}/grants`, { type: 'pin', envelope_hex: 'beef0001', slot_number: 3 })
  ));
  assert.equal(res.status, 200);
  assert.equal(res.body.sync_status, 'pending');
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].topic, TB('command', B1));
  assert.equal(pubs[0].payload.envelope_hex, 'beef0001');

  ({ res } = await step('grants-L2-list', () => call('GET', `/locks/${L2}/grants`)));
  assert.equal(res.body.grants.length, 1);
  assert.equal(res.body.grants[0].slot_number, 3);
  assert.equal(res.body.grants[0].sync_status, 'pending');
});

test('uplink: plaintext failure code marks the last sent grant failed', async () => {
  await step('uplink-L2-code', async () => {
    await fx.pub(T('uplink', L2), { from: L2, to: APP_A.hex, code: 'MCU_TIMEOUT' });
  });
  const grants = await waitFor(async () => {
    const g = (await fx.http('GET', `/locks/${L2}/grants`)).body.grants;
    return g[0].sync_status === 'failed' ? g : null;
  }, { what: 'grant failed' });
  assert.equal(grants[0].sync_status, 'failed');
});

test('uplink: sealed envelope is audited, not read; overheard chatter is not', async () => {
  const sealed = { from: L2, to: APP_A.hex, envelope_hex: 'abcdef' };
  await step('uplink-L2-sealed', async () => {
    await fx.pub(T('uplink', L2), sealed);
    await fx.pub(T('uplink', L2), { kind: 'list_bonds_reply' });
  });
  const { res } = await step('apps-A-activity-uplink', () => call('GET', `/apps/${APP_A.hex}/activity?limit=50`));
  const uplinks = res.body.activity.filter((a) => a.action === 'uplink');
  assert.equal(uplinks.length, 1);
  assert.equal(uplinks[0].detail, `size=${Buffer.byteLength(JSON.stringify(sealed))}B`);
});

test('uplink: caps report from a bonded lock is stored as device caps', async () => {
  await step('uplink-L2-caps', async () => {
    await fx.pub(T('uplink', L2), { caps: ['remote_unlock', 'pin_sync', 'audit'], transport: 'ble' });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.caps_source === 'device' ? l : null;
  }, { what: 'device caps' });
  assert.deepEqual(lock.caps, ['remote_unlock', 'pin_sync', 'audit', 'cred_pin']);
  assert.equal(lock.transport, 'ble');
});

test('grant revoke: with and without a server-side grant row', async () => {
  const grantId = (await fx.http('GET', `/locks/${L2}/grants`)).body.grants[0].id;
  let { res, pubs } = await step('revoke-L2-grant', () =>
    call('DELETE', `/locks/${L2}/grants/${grantId}`, { envelope_hex: 'dead0001' })
  );
  assert.equal(res.status, 200);
  assert.equal(res.body.sync_status, 'revoking');
  assert.equal(pubs[0].payload.envelope_hex, 'dead0001');

  ({ res, pubs } = await step('revoke-L2-no-row', () =>
    call('DELETE', `/locks/${L2}/grants/9999`, { envelope_hex: 'dead0002' })
  ));
  assert.equal(res.status, 200, 'relayed: the envelope is authoritative, not our row');
  assert.equal(pubs.length, 1);

  ({ res } = await step('revoke-no-envelope', () => call('DELETE', `/locks/${L2}/grants/${grantId}`, {})));
  assert.equal(res.status, 400);
  ({ res } = await step('revoke-unknown-lock', () =>
    call('DELETE', '/locks/nope/grants/1', { envelope_hex: 'aa' })
  ));
  assert.equal(res.status, 404);
  assert.equal(res.body.code, 'device_not_found');

  await step('uplink-L2-revoke-code', async () => {
    await fx.pub(T('uplink', L2), { from: L2, to: APP_A.hex, code: 'COUNTER_REPLAY' });
  });
});

test('bond verbs: bond-revoke, invite-cancel, settings', async () => {
  for (const verb of ['bond-revoke', 'invite-cancel', 'settings']) {
    const { res, pubs } = await step(`${verb}-L2`, () =>
      call('POST', `/locks/${L2}/${verb}`, { envelope_hex: `${verb.length}0aa` })
    );
    assert.equal(res.status, 200, verb);
    assert.equal(res.body.delivery, 'delivered');
    assert.equal(pubs.length, 1);
    assert.equal(pubs[0].topic, TB('command', B1));
  }
  let { res } = await step('bond-revoke-no-envelope', () => call('POST', `/locks/${L2}/bond-revoke`, {}));
  assert.equal(res.status, 400);
  ({ res } = await step('bond-revoke-not-enrolled', () =>
    call('POST', `/locks/${L4}/bond-revoke`, { envelope_hex: 'aa' })
  ));
  assert.equal(res.status, 409);
  assert.equal(res.body.code, 'lock_not_enrolled');
  ({ res } = await step('bond-revoke-direct-L1', () =>
    call('POST', `/locks/${L1}/bond-revoke`, { envelope_hex: '11aa' })
  ));
  assert.equal(res.status, 200);
});

test('sim heartbeat + queue introspection', async () => {
  let { res } = await step('sim-heartbeat-L2', () => call('POST', '/sim/heartbeat', { device_id: L2 }));
  assert.equal(res.body.flushed, 0, 'nothing left queued');
  ({ res } = await step('sim-heartbeat-unknown', () => call('POST', '/sim/heartbeat', { device_id: 'nope' })));
  assert.equal(res.status, 404);
  ({ res } = await step('queue', () => call('GET', '/queue')));
  assert.ok(res.body.queue.length >= 8);
  assert.ok(res.body.queue.every((q) => q.status === 'sent'));
});

/* ===========================================================================
 * Faults, reconciliation, scan, peer_status
 * ========================================================================= */
test('bridge offline -> lock bridge_offline, unlock refused; back online heals', async () => {
  await step('bridge-B1-offline', async () => {
    await fx.pub(TB('presence', B1), { state: 'offline', reason: 'lwt' });
  });
  let lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.presence_reason === 'bridge_offline' ? l : null;
  }, { what: 'bridge_offline' });
  assert.equal(lock.presence, 'offline');
  const { res } = await step('unlock-L2-bridge-offline', () =>
    call('POST', `/locks/${L2}/unlock`, { envelope_hex: 'aa' })
  );
  assert.equal(res.status, 409);
  assert.equal(res.body.code, 'bridge_offline');

  await step('bridge-B1-online-heal', async () => {
    await fx.pub(TB('presence', B1), { state: 'online' });
  });
  lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.presence === 'online' ? l : null;
  }, { what: 'healed' });
  assert.equal(lock.presence_reason, 'ok');
});

test('reconciliation: a claim against a healthy owner is refused with release_lock', async () => {
  await step('bridge-B2-online', async () => {
    await fx.pub(TB('presence', B2), { state: 'online' });
  });
  const { pubs } = await step('claim-L2-from-B2-rejected', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B2 });
  });
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[TB('command', B2), 'release_lock']]);
  assert.equal((await getLock(L2)).bridge_id, B1);
});

test('reconciliation: a claim against a dead owner moves the lock', async () => {
  await step('bridge-B1-offline-2', async () => {
    await fx.pub(TB('presence', B1), { state: 'offline', reason: 'lwt' });
  });
  await waitFor(async () => (await getLock(L2)).presence_reason === 'bridge_offline', { what: 'bridge_offline' });
  await step('claim-L2-from-B2-accepted', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B2 });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L2);
    return l.bridge_id === B2 && l.presence === 'online' ? l : null;
  }, { what: 'moved to B2' });
  assert.equal(lock.presence_reason, 'ok');
  assert.equal(lock.backup_available, false, 'B1 is the only other candidate and it is offline');
});

test('scan-bridges asks every online bridge and ranks by RSSI', async () => {
  await step('bridge-B1-online-3', async () => {
    await fx.pub(TB('presence', B1), { state: 'online' });
  });
  const rssi = { [B1]: -71, [B2]: -58 };
  const responder = (topic, obj) => {
    const m = topic.match(/^ozkie\/lab\/bridges\/([^/]+)\/command$/);
    if (m && obj.op === 'scan') {
      fx.pub(TB('scan_result', m[1]), { target: obj.target, found: true, rssi: rssi[m[1]] });
    }
  };
  fx.responders.push(responder);
  try {
    const { res, pubs } = await step('scan-bridges-L2', () => call('POST', `/locks/${L2}/scan-bridges`));
    assert.equal(res.status, 200);
    assert.equal(res.body.bridges_asked, 2);
    assert.deepEqual(res.body.candidates.map((c) => c.bridge_id), [B2, B1]);
    assert.equal(pubs.filter((m) => m.payload.op === 'scan').length, 2);
  } finally {
    fx.responders.pop();
  }
  const { res } = await step('scan-bridges-unknown', () => call('POST', '/locks/nope/scan-bridges'));
  assert.equal(res.status, 404);
});

test('peer_status peer_down marks the peer bridge offline', async () => {
  await step('peer-down-B1', async () => {
    await fx.pub(TB('peer_status', B2), { event: 'peer_down', peer: B1 });
  });
  await waitFor(async () => {
    const b = (await fx.http('GET', '/bridges')).body.bridges.find((x) => x.bridge_id === B1);
    return b && b.presence === 'offline';
  }, { what: 'B1 offline via peer_down' });
});

test('member removal relay is observed, never republished', async () => {
  const { pubs } = await step('members-request-remove', async () => {
    await fx.pub('ozkie/lab/members/s0-admin/request_remove', {
      request_id: 'r1',
      target_lock_id: L2,
      target_member_app_id: APP_B.hex,
    });
  });
  assert.equal(pubs.length, 0);
  const ev = (await fx.http('GET', '/events')).body.events;
  assert.ok(ev.some((e) => /Removal request r1/.test(e.message)));
});

/* ===========================================================================
 * OZKIE v2 gates (ozkey-48 groundwork)
 * ========================================================================= */
test('bridge-reported ozkie_version 2 switches the v2 gates on', async () => {
  await step('presence-L2-v2', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B2, ozkie_version: 2 });
  });
  await waitFor(async () => (await getLock(L2)).ozkie_version === 2, { what: 'v2' });

  let { res } = await step('unlock-L2-v2-not-owner', () => call('POST', `/locks/${L2}/unlock`, { envelope_hex: 'aa' }));
  assert.equal(res.status, 403);
  assert.equal(res.body.code, 'remote_unlock_owner_only');
  ({ res } = await step('unlock-L2-v2-owner', () =>
    call('POST', `/locks/${L2}/unlock`, { envelope_hex: 'aa', app_id: APP_A.hex })
  ));
  assert.equal(res.status, 501);
  assert.equal(res.body.code, 'ozkie_v2_relay_not_built');
  for (const [name, method, p] of [
    ['grant-L2-v2', 'POST', `/locks/${L2}/grants`],
    ['settings-L2-v2', 'POST', `/locks/${L2}/settings`],
    ['revoke-L2-v2', 'DELETE', `/locks/${L2}/grants/1`],
  ]) {
    ({ res } = await step(name, () => call(method, p, { type: 'pin', envelope_hex: 'aa' })));
    assert.equal(res.status, 501, name);
  }

  await step('presence-L2-v1', async () => {
    await fx.pub(T('presence', L2), { state: 'online', via: B2, ozkie_version: 1 });
  });
  await waitFor(async () => (await getLock(L2)).ozkie_version === 1, { what: 'back to v1' });
});

test('app-declared v2 pairing; DELETE sends no reset to a v2 lock', async () => {
  let { res } = await step('pairings-L5-v2', () =>
    call('POST', '/pairings', { app_id: APP_A.hex, device_id: L5, ozkie_version: 2 })
  );
  assert.equal(res.body.ozkie_version, 2);
  let pubs;
  ({ res, pubs } = await step('delete-L5-v2', () => call('DELETE', `/locks/${L5}`)));
  assert.equal(res.status, 200);
  assert.equal(res.body.reset.attempted, false);
  assert.equal(pubs.length, 0);
});

/* ===========================================================================
 * Resets + removal
 * ========================================================================= */
test('bridge reset: confirmed by presence, or unknown on timeout', async () => {
  let { res } = await step('bridge-reset-no-app', () => call('POST', '/bridges/ozb-s0-r1/reset', {}));
  assert.equal(res.status, 400);

  const responder = (topic, obj) => {
    if (topic === TB('command', 'ozb-s0-r1') && obj.op === 'factory_reset') {
      fx.pub(TB('presence', 'ozb-s0-r1'), { state: 'offline', reason: 'factory_reset' });
    }
  };
  fx.responders.push(responder);
  try {
    ({ res } = await step('bridge-reset-confirmed', () =>
      call('POST', '/bridges/ozb-s0-r1/reset', { app_id: APP_A.hex })
    ));
  } finally {
    fx.responders.pop();
  }
  assert.equal(res.body.verdict, 'reset_confirmed');
  assert.equal(res.body.cause, 'presence_confirmed');

  ({ res } = await step('bridge-reset-silent', () =>
    call('POST', '/bridges/ozb-s0-r2/reset', { app_id: APP_A.hex })
  ));
  assert.equal(res.body.verdict, 'unknown');
  assert.equal(res.body.cause, 'timeout');
});

test('uplink reset report clears ownership but keeps the row', async () => {
  await step('uplink-L3-reset', async () => {
    await fx.pub(T('uplink', L3), { type: 'reset' });
  });
  const lock = await waitFor(async () => {
    const l = await getLock(L3);
    return l.app_id === null ? l : null;
  }, { what: 'L3 ownership cleared' });
  assert.equal(lock.bridge_id, null);
  assert.equal(lock.status, 'registered');
});

test('DELETE a direct lock: plain factory_reset + Nexus decommission', async () => {
  const { res, pubs } = await step('delete-L1', () => call('DELETE', `/locks/${L1}`));
  assert.equal(res.status, 200);
  assert.equal(res.body.reset.attempted, true);
  assert.equal(res.body.reset.likely_delivered, true);
  assert.deepEqual(pubs.map((m) => [m.topic, m.payload.op]), [[T('command', L1), 'factory_reset']]);
  await waitFor(() => fx.nexusCalls.length === 1, { what: 'Nexus decommission' });
  assert.deepEqual(fx.nexusCalls[0], {
    method: 'POST',
    url: '/api/v1/lock-lifecycle/aabbcc000001/decommission',
    key: 's0-test-nexus-key',
  });
});

test('DELETE a bridged lock with a sealed reset waits for the lock verdict', async () => {
  const responder = (topic, obj) => {
    if (topic === TB('command', B2) && obj.target === L2 && obj.envelope_hex === 'reset01') {
      fx.pub(T('presence', L2), { state: 'offline', reason: 'factory_reset', msg_id: obj.msg_id });
    }
  };
  fx.responders.push(responder);
  let res;
  let pubs;
  try {
    ({ res, pubs } = await step('delete-L2-sealed', () => call('DELETE', `/locks/${L2}`, { envelope_hex: 'reset01' })));
  } finally {
    fx.responders.pop();
  }
  assert.equal(res.status, 200);
  assert.equal(res.body.reset.verdict, 'reset_confirmed');
  assert.equal(res.body.reset.transport_ok, true);
  assert.deepEqual(
    pubs.map((m) => m.payload.op || 'sealed').sort(),
    ['sealed', 'unpair_lock'],
    'sealed reset to the bridge + unpair_lock'
  );
});

test('DELETE unknown lock is 404; DELETE /locks clears the fleet', async () => {
  let { res } = await step('delete-unknown', () => call('DELETE', '/locks/nope'));
  assert.equal(res.status, 404);
  ({ res } = await step('delete-fleet', () => call('DELETE', '/locks')));
  assert.equal(res.body.removed, 2); // L3, L4
  ({ res } = await step('locks-empty', () => call('GET', '/locks')));
  assert.deepEqual(res.body.locks, []);
});

/* ===========================================================================
 * Golden transcript (must run last)
 * ========================================================================= */
test('golden transcript matches', () => assertGolden(assert, golden));
