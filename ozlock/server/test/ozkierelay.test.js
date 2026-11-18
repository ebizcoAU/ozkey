'use strict';
/*
 * OZKIE v2 relay through ozlock (ozkey-56; ozkey-48 rev 7 §2.4).
 *
 * Part 1, end to end with real crypto: a host-built ozcore lock
 * (`ozhostlock`, built from blelock/ozcore into the fixture's tmp dir) sits
 * behind a STAND-IN BRIDGE that does exactly what the real bridge does:
 * MQTT downlink ⇄ the lock's relay link, never opening a frame. The owner
 * app is ozcore's own RFC-written client (tools/ozapp.py, driven by
 * ozkierelay_app.py): pair at the door, register with ozlock, then EDHOC +
 * OSCORE (read, unlock with the Echo round trip) through POST /locks/:id/ozkie.
 *
 * Part 2, the route's own rules, against a scripted bridge: refusals in
 * ozkey-56 §3.1 order, byte-exact max-size frames with 64-char ids, 504
 * never re-sent, and the failover abort through a real XF-144 bridge move.
 *
 * Throughout: TRANSIT-ONLY. No relay call writes pending_queue or grants;
 * the only audit rows are the pairing's and one `ozkie_edhoc` per completed
 * relay handshake (ozkey-56 §2, F1).
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const net = require('net');
const fs = require('fs');
const { spawn, spawnSync } = require('child_process');
const mqtt = require('mqtt');
const { Fixture, x25519FromSeed, waitFor, sleep } = require('./harness');

const WAIT_MS = 3000;
const fx = new Fixture({ env: { OZLOCK_OZKIE_WAIT_MS: String(WAIT_MS), OZLOCK_OZKIE_M3_WAIT_MS: '500' } });

const REPO = path.join(__dirname, '..', '..', '..');
const OZCORE = path.join(REPO, 'blelock', 'ozcore');

const L = 'ozk-relay-host01'; // the host lock (part 1)
const B = 'ozb-relay-stand1'; // its stand-in bridge
const APP = x25519FromSeed('ozkierelay-owner'); // part 2 owner
const OTHER = x25519FromSeed('ozkierelay-other');
const L2 = 'l'.repeat(64); // 64-char ids (§2.4.6)
const B2 = 'b'.repeat(64);
const B5 = 'ozb-relay-takeover';
const L3 = 'ozk-relay-v1lock';
const L4 = 'ozk-relay-offline';
const B4 = 'ozb-relay-offline';

const T = (kind, id) => `ozkie/lab/locks/${id}/${kind}`;
const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;
const down = (b, l) => `ozkie/lab/bridges/${b}/ozkie/${l}`;
const up = (l, b) => `ozkie/lab/locks/${l}/ozkie/${b}`;
const getLock = async (id) => (await fx.http('GET', `/locks/${id}`)).body.lock;
const relay = (id, body) => fx.http('POST', `/locks/${id}/ozkie`, body);
const b64 = (buf) => buf.toString('base64');

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once('error', reject);
    s.listen(0, '127.0.0.1', () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
}

/** A type 1 frame shaped like OSCORE: CoAP v1 CON, token, then `fill` bytes. */
function coapFrame(token, totalLen = 0, code = 0x02) {
  const head = Buffer.concat([Buffer.from([1, 0x40 | token.length, code, 0x01, 0x02]), token]);
  const pad = Math.max(0, totalLen - head.length);
  return Buffer.concat([head, Buffer.alloc(pad, 0x5a)]);
}

let hostlock = null;
let hostOut = '';
let bridgeClient = null; // the stand-in bridge (part 1) + scripted bridges (part 2)
let lockSock = null;
const scripted = []; // part 2: { topic, buf } downlinks seen by scripted bridges
let counts0 = null;

async function tableCounts() {
  const out = {};
  for (const t of ['pending_queue', 'grants']) {
    out[t] = Number((await fx.query(`SELECT COUNT(*) AS n FROM ${t}`))[0].n);
  }
  out.audit_max_id = Number((await fx.query('SELECT COALESCE(MAX(id), 0) AS m FROM audit_log'))[0].m);
  return out;
}

before(async () => {
  await fx.start();

  // Build the host lock from this checkout's ozcore.
  const build = path.join(fx.tmp, 'ozcore-build');
  let r = spawnSync('cmake', ['-S', OZCORE, '-B', build, '-DCMAKE_BUILD_TYPE=Release'], { encoding: 'utf8' });
  assert.equal(r.status, 0, `cmake configure failed:\n${r.stdout}\n${r.stderr}`);
  r = spawnSync('cmake', ['--build', build, '--target', 'ozhostlock', '-j', '4'], { encoding: 'utf8' });
  assert.equal(r.status, 0, `ozhostlock build failed:\n${r.stdout}\n${r.stderr}`);
  const exe = path.join(build, 'ozhostlock');
  assert.ok(fs.existsSync(exe));

  const blePort = await freePort();
  const relayPort = await freePort();
  hostlock = spawn(exe, ['--state', path.join(fx.tmp, 'lockstate'), '--device-id', L, '--ble', String(blePort), '--relay', String(relayPort)], {
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  hostlock.stdout.on('data', (d) => (hostOut += d.toString()));
  await waitFor(() => /lock key /.test(hostOut), { what: 'ozhostlock up' });
  fx.blePort = blePort;

  // The stand-in bridge: one relay connection to the lock, MQTT both ways.
  lockSock = net.connect(relayPort, '127.0.0.1');
  await new Promise((res, rej) => lockSock.once('connect', res).once('error', rej));
  bridgeClient = await mqtt.connectAsync(fx.mqttUrl, { clientId: `ozkierelay-bridges-${process.pid}` });
  let rx = Buffer.alloc(0);
  lockSock.on('data', (d) => {
    rx = Buffer.concat([rx, d]);
    while (rx.length >= 3 && rx.length >= 3 + rx.readUInt16BE(1)) {
      const n = rx.readUInt16BE(1);
      const frame = Buffer.concat([rx.subarray(0, 1), rx.subarray(3, 3 + n)]);
      rx = rx.subarray(3 + n);
      bridgeClient.publish(up(L, B), frame, { qos: 0 });
    }
  });
  await bridgeClient.subscribeAsync([`ozkie/lab/bridges/+/ozkie/+`], { qos: 0 });
  bridgeClient.on('message', (topic, buf) => {
    if (topic === down(B, L)) {
      // type ‖ message  →  type ‖ len(2) ‖ message (hostlock's TCP framing)
      const len = Buffer.alloc(2);
      len.writeUInt16BE(buf.length - 1);
      lockSock.write(Buffer.concat([buf.subarray(0, 1), len, buf.subarray(1)]));
    } else {
      scripted.push({ topic, buf: Buffer.from(buf) });
    }
  });

  for (const b of [B, B2]) await fx.pub(TB('presence', b), { state: 'online' });
  await fx.pub(TB('presence', B4), { state: 'offline' });
  await fx.pub(T('presence', L), { state: 'online', via: B, ozkie_version: 2 });
  counts0 = await tableCounts();
});

after(async () => {
  if (lockSock) lockSock.destroy();
  if (bridgeClient) await bridgeClient.endAsync();
  if (hostlock) hostlock.kill();
  await fx.stop();
});

/* ===========================================================================
 * Part 1: end to end, real EDHOC + OSCORE, the app reaches the lock only via ozlock
 * ========================================================================= */
test('owner app: door pairing, then EDHOC, read and Echo unlock through ozlock', async () => {
  // Async on purpose: the stand-in bridge runs in THIS process, so a
  // spawnSync would freeze it while the app waits for ozlock's answer.
  const r = await new Promise((resolve) => {
    const child = spawn(
      'python3',
      ['-I', path.join(__dirname, 'ozkierelay_app.py'), path.join(OZCORE, 'tools'), String(fx.blePort), fx.base, L, B],
      { stdio: ['ignore', 'pipe', 'pipe'] }
    );
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += d));
    child.stderr.on('data', (d) => (stderr += d));
    const kill = setTimeout(() => child.kill(), 90_000);
    child.on('close', (status) => {
      clearTimeout(kill);
      resolve({ status, stdout, stderr });
    });
  });
  const lines = r.stdout.trim().split('\n').filter(Boolean).map((s) => JSON.parse(s));
  for (const c of lines.filter((x) => x.check)) assert.ok(c.ok, `${c.check}: ${JSON.stringify(c)}`);
  assert.equal(r.status, 0, `ozkierelay_app.py failed:\n${r.stdout}\n${r.stderr}`);
  assert.ok(lines.filter((x) => x.check).length >= 8, 'every check ran');
  assert.match(hostOut, /\[DOOR\] UNLOCKED/, 'the host lock actuated');
  const lock = await getLock(L);
  assert.equal(lock.ozkie_version, 2);
  // exactly one content-free audit row for the relay handshake
  const rows = await fx.query("SELECT app_id, detail FROM audit_log WHERE action = 'ozkie_edhoc' AND device_id = ?", [L]);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].app_id, lines.find((x) => x.app_id).app_id);
});

/* ===========================================================================
 * Part 2: the route's rules (scripted bridges)
 * ========================================================================= */
test('setup: v2 locks behind online/offline bridges, one v1 lock', async () => {
  for (const [id, b, v] of [
    [L2, B2, 2],
    [L3, B2, 1],
    [L4, B4, 2],
  ]) {
    const { status } = await fx.http('POST', '/pairings', { app_id: APP.hex, device_id: id, bridge_id: b, ozkie_version: v });
    assert.equal(status, 200);
  }
  await fx.pub(T('presence', L2), { state: 'online', via: B2, ozkie_version: 2 });
  await waitFor(async () => (await getLock(L2)).presence === 'online', { what: 'L2 online' });
});

test('refusals in §3.1 order; nothing published for any of them', async () => {
  const n0 = scripted.length;
  const ok = b64(coapFrame(Buffer.from([1, 2, 3, 4])));
  const cases = [
    ['not base64', L2, { app_id: APP.hex, frame_b64: '***' }, 400, 'bad_frame'],
    ['unknown type', L2, { app_id: APP.hex, frame_b64: b64(Buffer.from([3, 1, 2])) }, 400, 'bad_frame'],
    ['sender type 2', L2, { app_id: APP.hex, frame_b64: b64(Buffer.from([2, 1, 2])) }, 400, 'bad_frame'],
    ['oversize', L2, { app_id: APP.hex, frame_b64: b64(Buffer.alloc(1026, 1)) }, 400, 'bad_frame'],
    ['unknown lock', 'ozk-nope', { app_id: APP.hex, frame_b64: ok }, 404, 'lock_not_found'],
    ['v1 lock', L3, { app_id: APP.hex, frame_b64: ok }, 409, 'not_ozkie_v2'],
    ['not the owner', L2, { app_id: OTHER.hex, frame_b64: ok }, 403, 'relay_owner_only'],
    ['no app_id', L2, { frame_b64: ok }, 403, 'relay_owner_only'],
    ['bridge offline', L4, { app_id: APP.hex, frame_b64: ok }, 503, 'bridge_offline'],
    ['short token', L2, { app_id: APP.hex, frame_b64: b64(coapFrame(Buffer.from([9]))) }, 400, 'token_too_short'],
    ['m3 with no handshake', L2, { app_id: APP.hex, frame_b64: b64(Buffer.from([0, 0x52, 1])) }, 409, 'edhoc_no_lease'],
  ];
  for (const [name, id, body, status, code] of cases) {
    const r = await relay(id, body);
    assert.equal(r.status, status, name);
    assert.equal(r.body.code, code, name);
  }
  await sleep(200);
  assert.equal(scripted.length, n0, 'no refusal published anything');
});

test('max-size frame, 64-char ids: byte-exact both ways, binary on the wire', async () => {
  const tokenA = Buffer.from([0xde, 0xad, 0xbe, 0xef]);
  const req = coapFrame(tokenA, 1025);
  const ans = coapFrame(tokenA, 1025, 0x44);
  ans[ans.length - 1] = 0x77;
  const n0 = scripted.length;
  const p = relay(L2, { app_id: APP.hex, frame_b64: b64(req) });
  await waitFor(() => scripted.length > n0, { what: 'downlink' });
  const d = scripted[n0];
  assert.equal(d.topic, down(B2, L2));
  assert.ok(d.buf.equals(req), 'downlink byte-exact');
  assert.ok(Buffer.byteLength(d.topic) <= 200);
  bridgeClient.publish(up(L2, B2), ans, { qos: 0 });
  const r = await p;
  assert.equal(r.status, 200);
  assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(ans), 'answer byte-exact');
});

test('silence: 504 no_ack after the wait, published once, never re-sent', async () => {
  const n0 = scripted.length;
  const t0 = Date.now();
  const r = await relay(L2, { app_id: APP.hex, frame_b64: b64(coapFrame(Buffer.from([5, 5, 5, 5]))) });
  assert.equal(r.status, 504);
  assert.equal(r.body.code, 'no_ack');
  assert.equal(r.body.outcome, 'unknown');
  assert.ok(Date.now() - t0 >= WAIT_MS - 100);
  await sleep(500);
  assert.equal(scripted.length - n0, 1, 'exactly one publish');
});

test('failover: a real XF-144 bridge move aborts the pinned exchange at once', async () => {
  const p = relay(L2, { app_id: APP.hex, frame_b64: b64(coapFrame(Buffer.from([6, 6, 6, 6]))) });
  await sleep(150);
  const t0 = Date.now();
  await fx.pub(TB('presence', B2), { state: 'offline' });
  await waitFor(async () => (await getLock(L2)).presence !== 'online', { what: 'L2 not online' });
  await fx.pub(TB('presence', B5), { state: 'online' });
  await fx.pub(T('presence', L2), { state: 'online', via: B5, ozkie_version: 2 });
  const r = await p;
  assert.equal(r.status, 503);
  assert.equal(r.body.code, 'bridge_moved');
  assert.equal(r.body.outcome, 'unknown');
  assert.ok(Date.now() - t0 < WAIT_MS, 'aborted before the wait ran out');
  assert.equal((await getLock(L2)).bridge_id, B5);
});

test('old v1 routes refuse a v2 lock and name the relay', async () => {
  const r = await fx.http('POST', `/locks/${L2}/grants`, { type: 'pin', envelope_hex: 'aa', slot_number: 1 });
  assert.equal(r.status, 409);
  assert.equal(r.body.code, 'ozkie_v2_use_relay');
  assert.equal(r.body.relay, `/locks/${L2}/ozkie`);
});

test('transit-only: no relay call wrote pending_queue or grants; audit = pairings, the move, one handshake', async () => {
  const c = await tableCounts();
  assert.equal(c.pending_queue, counts0.pending_queue);
  assert.equal(c.grants, counts0.grants);
  const extra = await fx.query('SELECT action, COUNT(*) AS n FROM audit_log WHERE id > ? GROUP BY action', [counts0.audit_max_id]);
  const byAction = Object.fromEntries(extra.map((r) => [r.action, Number(r.n)]));
  // pair: L (the app's /pairings) + L2, L3, L4; bridge_reconcile: the XF-144
  // move the failover test makes; ozkie_edhoc: the one relay handshake.
  // Nothing else: no relayed request leaves a row.
  assert.deepEqual(byAction, { pair: 4, bridge_reconcile: 1, ozkie_edhoc: 1 });
});
