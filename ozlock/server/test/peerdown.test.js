'use strict';
/*
 * OZLOCK-PEER-DOWN-1 (PM nexus-1b, 2026-11-19; found in AK1037 phase B,
 * 2026-11-19 22:49: Bridge 2's Thread-roster peer_down marked a healthy,
 * relaying Bridge 1 offline, and 3 of 10 remote unlocks got a false 503).
 *
 * Rule: a peer's peer_down report alone must not mark a bridge offline while
 * that bridge's OWN presence says online and is fresh (within
 * PEER_DOWN_FRESH_MS; ozlock default 75 s = 2 × the 30 s presence resync +
 * 15 s). The relay needs the bridge's MQTT session and its BLE link, not
 * Thread, so a Thread-level opinion from another bridge doesn't override what
 * the broker says about the bridge itself. When the bridge's own report is
 * stale or missing, peer_down still fast-tracks it offline, as before (XF-144).
 *
 * The presence resync timer is pushed out to an hour here so presence_at only
 * moves when this file publishes.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { Fixture, x25519FromSeed, waitFor, sleep } = require('./harness');

const FRESH_MS = 20000;
const fx = new Fixture({
  env: {
    OZLOCK_FAILOVER_SWEEP_MS: String(3600 * 1000),
    OZLOCK_PEER_DOWN_FRESH_MS: String(FRESH_MS),
    OZLOCK_OZKIE_WAIT_MS: '400',
  },
});
const OWNER = x25519FromSeed('peerdown-owner');
const L = 'ozk-peerdown-l1';
const [B1, B2, B3] = ['ozb-peerdown-one', 'ozb-peerdown-two', 'ozb-peerdown-ghost'];
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;

const bridgeRow = async (b) => (await fx.query('SELECT presence, presence_at FROM bridges_presence WHERE bridge_id = ?', [b]))[0];
const lockRow = async () => (await fx.query('SELECT presence, presence_reason FROM locks WHERE id = ?', [L]))[0];
const ageBridge = (b, s) =>
  fx.query('UPDATE bridges_presence SET presence_at = DATE_SUB(NOW(), INTERVAL ? SECOND) WHERE bridge_id = ?', [s, b]);
const peerDown = (reporter, peer) => fx.pub(TB('peer_status', reporter), { event: 'peer_down', peer });

// One OSCORE-typed frame (type 1, CoAP CON POST, 4-byte token). ozlock only
// reads the outer header; it is never opened.
let tok = 0;
const relay = () => {
  const t = Buffer.alloc(4);
  t.writeUInt32BE(++tok + 0x5000);
  const frame = Buffer.concat([Buffer.from([1, 0x44, 0x02, 0x00, tok & 0xff]), t, Buffer.from([0xff, 0xaa, 0xbb])]);
  return fx.http('POST', `/locks/${L}/ozkie`, { app_id: OWNER.hex, frame_b64: frame.toString('base64') });
};

before(async () => {
  await fx.start();
});
after(async () => {
  await fx.stop();
});

test('setup: B1 and B2 online by their own presence; v2 lock on B1', async () => {
  await fx.pub(TB('presence', B1), { state: 'online' });
  await fx.pub(TB('presence', B2), { state: 'online' });
  const r = await fx.http('POST', '/pairings', { app_id: OWNER.hex, device_id: L, bridge_id: B1, ozkie_version: 2 });
  assert.equal(r.status, 200, JSON.stringify(r.body));
  await fx.pub(`ozkie/lab/locks/${L}/presence`, { state: 'online', via: B1, ozkie_version: 2 });
  await waitFor(async () => (await bridgeRow(B1))?.presence === 'online' && (await lockRow())?.presence === 'online', {
    what: 'B1 + lock online',
  });
});

test('P1: peer_down for a bridge whose own presence is fresh is ignored: bridge, lock and relay unaffected', async () => {
  for (let i = 0; i < 3; i++) await peerDown(B2, B1);
  await sleep(600);
  assert.equal((await bridgeRow(B1)).presence, 'online');
  const lock = await lockRow();
  assert.notEqual(lock.presence_reason, 'bridge_offline');
  const r = await relay();
  assert.notEqual(r.status, 503, `relay refused: ${JSON.stringify(r.body)}`);
  assert.equal(r.status, 504); // published, the stand-in lock never answers
  assert.match(fx.log(), new RegExp(`peer_down: bridge ${B2} reports peer ${B1} down — ignored, ${B1}'s own presence is online`));
});

test('P2: once the bridge\'s own report is older than the fresh window, peer_down fast-tracks it offline (XF-144)', async () => {
  await ageBridge(B1, FRESH_MS / 1000 + 5);
  await peerDown(B2, B1);
  await waitFor(async () => (await bridgeRow(B1)).presence === 'offline', { what: 'B1 offline via peer_down' });
  await waitFor(async () => (await lockRow()).presence_reason === 'bridge_offline', { what: 'lock bridge_offline' });
  const r = await relay();
  assert.equal(r.status, 503);
  assert.equal(r.body.code, 'bridge_offline');
});

test('P3: the bridge\'s own online report heals it; a peer_down right after is ignored again', async () => {
  await fx.pub(TB('presence', B1), { state: 'online' });
  await waitFor(async () => (await lockRow()).presence === 'online', { what: 'healed' });
  await peerDown(B2, B1);
  await sleep(600);
  assert.equal((await bridgeRow(B1)).presence, 'online');
  assert.notEqual((await relay()).status, 503);
});

test('P4: a bridge whose own presence is offline or unknown is still marked offline by peer_down', async () => {
  // never seen: no row at all
  await peerDown(B2, B3);
  await waitFor(async () => (await bridgeRow(B3))?.presence === 'offline', { what: 'ghost offline' });
  // own presence already offline (LWT): stays offline
  await fx.pub(TB('presence', B2), { state: 'offline', reason: 'lwt' });
  await peerDown(B1, B2);
  await sleep(400);
  assert.equal((await bridgeRow(B2)).presence, 'offline');
  // own presence fresh but not 'online' (an unrecognised state reads as
  // unknown): not a confirmation of life, so peer_down still applies
  await fx.pub(TB('presence', B2), { state: 'booting' });
  await waitFor(async () => (await bridgeRow(B2)).presence === 'unknown', { what: 'B2 unknown' });
  await peerDown(B1, B2);
  await waitFor(async () => (await bridgeRow(B2)).presence === 'offline', { what: 'unknown B2 offline via peer_down' });
});

test('P5: a lock behind a falsely reported bridge is never failed over (the sweep sees no bridge_offline)', async () => {
  // B1 fresh again, then a burst of false reports; the lock's verdict never
  // became bridge_offline, so FAILOVER_GRACE can't start counting.
  await fx.pub(TB('presence', B1), { state: 'online' });
  await waitFor(async () => (await bridgeRow(B1)).presence === 'online', { what: 'B1 online' });
  const before = await lockRow();
  for (let i = 0; i < 5; i++) await peerDown(B2, B1);
  await sleep(600);
  const now = await lockRow();
  assert.equal(now.presence_reason, before.presence_reason);
  assert.notEqual(now.presence_reason, 'bridge_offline');
});
