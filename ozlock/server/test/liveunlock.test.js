'use strict';
/*
 * Live-only remote unlock (operator 2026-11-05: "remote unlock must NEVER be
 * queued"; PM nexus-6e GO 2026-11-07, R1 (A) + R2 (i)).
 *
 * v1 contract:
 *   - no pending_queue row for a remote unlock, on any path;
 *   - no route -> 409 (lock_unreachable / bridge_offline …);
 *   - otherwise publish ONCE, watch the uplink for a refusal code for
 *     UNLOCK_REFUSAL_WATCH_MS: a code -> 502 lock_refused, silence -> 200
 *     result "sent_unconfirmed" (a v1 lock never reports success to the
 *     server, so 200 never means "opened");
 *   - never re-sent, no failover retry; one unlock in flight per lock.
 * The one listed exception: ASSISTED unlock stays queued (60 s, keypad touch)
 * until the operator rules on the doorbell redesign.
 *
 * Side bug fixed under (A): a refusal code that answers a remote unlock used
 * to be attributed to the last GRANT sent to that lock and mark it 'failed'.
 *
 * Own server instance, short watch (OZLOCK_UNLOCK_WATCH_MS) so the silence
 * path doesn't cost 10 s.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { Fixture, x25519FromSeed, waitFor } = require('./harness');

const WATCH_MS = 1500;
const fx = new Fixture({ env: { OZLOCK_UNLOCK_WATCH_MS: String(WATCH_MS) } });

const APP = x25519FromSeed('s0-liveunlock-app');
const L = 's0-lu-bridged-01'; // remote_unlock (inferred from its bridge)
const LA = 's0-lu-assisted-02'; // assisted_unlock only (device caps)
const B1 = 'ozb-s0-lu1';

const T = (kind, id) => `ozkie/lab/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;
const getLock = async (id) => (await fx.http('GET', `/locks/${id}`)).body.lock;

/** As the lock behind B1: answer a command carrying `envelope_hex` with an uplink code. */
function refuseOn(envelopeHex, code, target = L) {
  const r = (topic, obj) => {
    if (topic === TB('command', B1) && obj && obj.target === target && obj.envelope_hex === envelopeHex) {
      fx.pub(T('uplink', target), { from: target, to: APP.hex, code });
    }
  };
  fx.responders.push(r);
  return () => fx.responders.splice(fx.responders.indexOf(r), 1);
}

const unlockPubs = (m, target = L) =>
  fx.published.slice(m).filter((p) => p.topic === TB('command', B1) && p.payload && p.payload.target === target);

before(async () => {
  await fx.start();
});

after(async () => {
  await fx.stop();
});

test('setup: B1 online, L and LA paired behind it and online', async () => {
  await fx.pub(TB('presence', B1), { state: 'online' });
  for (const id of [L, LA]) {
    const { status } = await fx.http('POST', '/pairings', { app_id: APP.hex, device_id: id, bridge_id: B1 });
    assert.equal(status, 200);
    await fx.pub(T('presence', id), { state: 'online', via: B1 });
  }
  await fx.pub(T('uplink', LA), { caps: ['assisted_unlock', 'audit'], transport: 'ble' });
  await waitFor(async () => (await getLock(L)).presence === 'online', { what: 'L online' });
  await waitFor(async () => {
    const l = await getLock(LA);
    return l.presence === 'online' && l.caps_source === 'device';
  }, { what: 'LA online with device caps' });
});

test('side bug: a refusal answering a remote unlock is NOT attributed to an earlier grant', async () => {
  const g = await fx.http('POST', `/locks/${L}/grants`, { type: 'pin', envelope_hex: 'beef0101', slot_number: 2 });
  assert.equal(g.status, 200);
  const grantId = g.body.grant_id || (await fx.http('GET', `/locks/${L}/grants`)).body.grants[0].id;

  const undo = refuseOn('u0refuse', 'COUNTER_REPLAY');
  let res;
  try {
    res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'u0refuse' });
    // Give a fire-and-forget server a moment to mis-attribute, as it used to.
    await new Promise((r) => setTimeout(r, 500));
  } finally {
    undo();
  }
  const grant = (await fx.http('GET', `/locks/${L}/grants`)).body.grants.find((x) => x.id === grantId);
  assert.equal(grant.sync_status, 'pending', 'the grant is untouched; the code answered the unlock');
  assert.equal(res.status, 502);
  assert.equal(res.body.code, 'lock_refused');
  assert.equal(res.body.lock_code, 'COUNTER_REPLAY');
});

test('silence within the watch -> 200 sent_unconfirmed, published exactly once, never queued', async () => {
  const m = fx.mark();
  const t0 = Date.now();
  const res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'u1silent' });
  const took = Date.now() - t0;
  assert.equal(res.status, 200);
  assert.equal(res.body.result, 'sent_unconfirmed');
  assert.equal(res.body.transport_ok, true);
  assert.equal(res.body.delivery, 'delivered', 'compat field kept');
  assert.equal(res.body.mode, 'remote');
  assert.equal(res.body.queue_id, undefined, 'no queue row');
  assert.equal(res.body.expires_at, undefined, 'nothing waits to fire later');
  assert.ok(took >= WATCH_MS - 50, `the server watched for a refusal (${took} ms)`);
  // Wait past one more watch: nothing is ever re-sent.
  await new Promise((r) => setTimeout(r, WATCH_MS + 500));
  const pubs = unlockPubs(m);
  assert.equal(pubs.length, 1);
  assert.equal(pubs[0].payload.envelope_hex, 'u1silent');
  assert.deepEqual(Object.keys(pubs[0].payload).sort(), ['envelope_hex', 'msg_id', 'target']);
});

test('one unlock in flight per lock: a second one during the watch gets 409 unlock_in_progress', async () => {
  const m = fx.mark();
  const first = fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'u2first' });
  await waitFor(() => unlockPubs(m).length === 1, { what: 'first unlock published' });
  const second = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'u2second' });
  assert.equal(second.status, 409);
  assert.equal(second.body.code, 'unlock_in_progress');
  assert.equal((await first).status, 200);
  assert.equal(unlockPubs(m).length, 1, 'the second one was never published');
});

test('no route (bridge offline) -> 409, nothing published, nothing queued', async () => {
  await fx.pub(TB('presence', B1), { state: 'offline', reason: 'lwt' });
  await waitFor(async () => (await getLock(L)).presence === 'offline', { what: 'L offline' });
  const m = fx.mark();
  const res = await fx.http('POST', `/locks/${L}/unlock`, { envelope_hex: 'u3offline' });
  assert.equal(res.status, 409);
  assert.equal(res.body.error, 'lock_unreachable');
  await new Promise((r) => setTimeout(r, 300));
  assert.equal(unlockPubs(m).length, 0);
  await fx.pub(TB('presence', B1), { state: 'online' });
  await fx.pub(T('presence', L), { state: 'online', via: B1 });
  await fx.pub(T('presence', LA), { state: 'online', via: B1 });
  await waitFor(async () => (await getLock(L)).presence === 'online', { what: 'L back online' });
});

test('INVARIANT: the server never wrote a remote-unlock row to pending_queue', async () => {
  const rows = await fx.query("SELECT id, status FROM pending_queue WHERE action_type = 'unlock'");
  assert.deepEqual(rows, []);
});

test('listed exception: ASSISTED unlock is still queued with its 60 s touch window', async () => {
  const res = await fx.http('POST', `/locks/${LA}/unlock`, { envelope_hex: 'a1assist' });
  assert.equal(res.status, 200);
  assert.equal(res.body.mode, 'assisted');
  assert.equal(res.body.requires_touch, true);
  assert.equal(res.body.window_s, 60);
  assert.ok(res.body.queue_id, 'assisted keeps its queue row until the doorbell ruling');
  const rows = await fx.query('SELECT action_type FROM pending_queue WHERE id = ?', [res.body.queue_id]);
  assert.equal(rows[0].action_type, 'assisted-unlock');
});

test('backstop: a queued remote-unlock row (e.g. left by an older server) is never published', async () => {
  const [lock] = await fx.query('SELECT site_id FROM locks WHERE id = ?', [L]);
  await fx.query(
    `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
     VALUES (?, ?, NULL, 'unlock', 'u4stale', 'sealed_envelope', 'queued', UTC_TIMESTAMP() + INTERVAL 1 HOUR)`,
    [L, lock.site_id]
  );
  const m = fx.mark();
  // Any write that flushes L's queue: a grant.
  const g = await fx.http('POST', `/locks/${L}/grants`, { type: 'pin', envelope_hex: 'beef0102', slot_number: 4 });
  assert.equal(g.status, 200);
  await new Promise((r) => setTimeout(r, 500));
  const pubs = unlockPubs(m);
  assert.ok(pubs.some((p) => p.payload.envelope_hex === 'beef0102'), 'the grant went out');
  assert.ok(!pubs.some((p) => p.payload.envelope_hex === 'u4stale'), 'the stale unlock did not');
  const rows = await fx.query("SELECT status FROM pending_queue WHERE envelope_hex = 'u4stale'");
  assert.equal(rows[0].status, 'expired');
});
