'use strict';
/*
 * ozkie.js unit tests (ozkey-56): the relay's correlation, EDHOC lease,
 * message_3 window, failover abort and drops, with the transport and audit
 * stubbed. No DB, no broker. Route checks and the end-to-end run against a
 * host-built lock are in ozlock/server/test/ozkierelay.test.js.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const { ctx } = require('../ctx');

const sent = []; // { topic, buf }
let brokerUp = true;
const audits = [];
Object.assign(ctx, {
  CONFIG: { SITE_ID: 'lab', OZKIE_WAIT_MS: 300, OZKIE_EDHOC_LEASE_MS: 1000, OZKIE_M3_WAIT_MS: 150 },
  logEvent: () => {},
});
require('../transport').mqttPublishRaw = (topic, buf) => {
  if (!brokerUp) return false;
  sent.push({ topic, buf: Buffer.from(buf) });
  return true;
};
require('../audit').recordAudit = async (...a) => audits.push(a);

const oz = require('../ozkie');
const L = 'ozk-unit-1';
const B = 'ozb-unit-1';
const B2 = 'ozb-unit-2';
const DOWN = `ozkie/lab/bridges/${B}/ozkie/${L}`;

/** An OSCORE-shaped type 1 frame: CoAP ver 1, CON, TKL, code, mid, token, junk. */
function coap(token, code = 0x02, body = Buffer.from([0xff, 1, 2, 3])) {
  const h = Buffer.from([0x40 | token.length, code, 0x12, 0x34]);
  return Buffer.concat([Buffer.from([1]), h, token, body]);
}
const tok = (n) => Buffer.from([0xa0, 0xb1, 0xc2, n]);
const relay = (frame, o = {}) => oz.relayFrame({ siteId: 'lab', lockId: L, bridgeId: B, appId: 'app-a', frame, ...o });
const nextTick = () => new Promise((r) => setImmediate(r));

test('frame checks: empty, oversize, unknown type, sender type 2', async () => {
  for (const [buf, reason] of [
    [Buffer.from([1]), 'empty'],
    [Buffer.alloc(1026, 1), 'too_large'],
    [Buffer.from([3, 1, 2]), 'bad_type'],
    [Buffer.from([2, 1, 2]), 'bad_type'],
  ]) {
    const r = await relay(buf);
    assert.equal(r.status, 400);
    assert.equal(r.body.reason, reason);
  }
  assert.equal(sent.length, 0);
});

test('OSCORE: malformed CoAP and short token refused before anything is sent', async () => {
  let r = await relay(Buffer.from([1, 0x80, 0x02, 0, 0]));
  assert.equal(r.body.reason, 'bad_coap');
  r = await relay(coap(Buffer.from([7])));
  assert.equal(r.status, 400);
  assert.equal(r.body.code, 'token_too_short');
  assert.equal(sent.length, 0);
});

test('OSCORE: the answer with the same token comes back byte-exact; others are dropped', async () => {
  const req = coap(tok(1));
  const p = relay(req);
  await nextTick();
  assert.equal(sent.length, 1);
  assert.equal(sent[0].topic, DOWN);
  assert.ok(sent[0].buf.equals(req));
  oz.handleOzkieUplink(L, B, coap(tok(9), 0x44)); // another token
  oz.handleOzkieUplink(L, B2, coap(tok(1), 0x44)); // right token, wrong bridge
  const ans = coap(tok(1), 0x44, Buffer.from([0xff, 9, 9]));
  oz.handleOzkieUplink(L, B, ans);
  const r = await p;
  assert.equal(r.status, 200);
  assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(ans));
  const c = oz.ozkieCounters();
  assert.equal(c.uplink_unmatched, 1);
  assert.equal(c.uplink_wrong_bridge, 1);
});

test('OSCORE: a token in flight is refused; silence is 504 no_ack, never re-sent; the late answer is dropped', async () => {
  sent.length = 0;
  const p = relay(coap(tok(2)));
  await nextTick();
  const dup = await relay(coap(tok(2)));
  assert.equal(dup.status, 409);
  assert.equal(dup.body.code, 'token_in_flight');
  const r = await p;
  assert.equal(r.status, 504);
  assert.equal(r.body.code, 'no_ack');
  assert.equal(r.body.outcome, 'unknown');
  assert.equal(sent.length, 1, 'published once, never re-sent');
  const before = oz.ozkieCounters().uplink_unmatched;
  oz.handleOzkieUplink(L, B, coap(tok(2), 0x44));
  assert.equal(oz.ozkieCounters().uplink_unmatched, before + 1);
  // the token is free again
  const again = relay(coap(tok(2)));
  await nextTick();
  oz.handleOzkieUplink(L, B, coap(tok(2), 0x44));
  assert.equal((await again).status, 200);
});

test('broker down: 503 mqtt_down, nothing pending afterwards', async () => {
  brokerUp = false;
  const r = await relay(coap(tok(3)));
  brokerUp = true;
  assert.equal(r.status, 503);
  assert.equal(r.body.code, 'mqtt_down');
  const p = relay(coap(tok(3)));
  await nextTick();
  oz.handleOzkieUplink(L, B, coap(tok(3), 0x44));
  assert.equal((await p).status, 200);
});

test('EDHOC: m1 → m2, m3 + silence = done (one audit row), lease released', async () => {
  audits.length = 0;
  const m1 = Buffer.from([0, 0x03, 0x02, 0x58]);
  const p1 = relay(m1);
  await nextTick();
  const busy = await relay(Buffer.from([0, 0x03, 0x02]), { appId: 'app-b' });
  assert.equal(busy.status, 409);
  assert.equal(busy.body.code, 'edhoc_busy');
  const m2 = Buffer.from([0, 0x58, 0x20, 1, 2]);
  oz.handleOzkieUplink(L, B, m2);
  const r1 = await p1;
  assert.equal(r1.status, 200);
  assert.ok(Buffer.from(r1.body.frame_b64, 'base64').equals(m2));
  const r3 = await relay(Buffer.from([0, 0x52, 0xe5, 0x62]));
  assert.equal(r3.status, 200);
  assert.equal(r3.body.frame_b64, null);
  assert.equal(audits.length, 1);
  assert.equal(audits[0][2], 'ozkie_edhoc');
  // released: another sender can start now
  const pB = relay(m1, { appId: 'app-b' });
  await nextTick();
  oz.handleOzkieUplink(L, B, m2);
  assert.equal((await pB).status, 200);
  assert.equal((await relay(Buffer.from([0, 0x52]), { appId: 'app-b' })).status, 200); // done, lease released
});

test('EDHOC: m3 refused by the lock comes back, no audit row; m3 without a lease refused', async () => {
  audits.length = 0;
  const p1 = relay(Buffer.from([0, 0x03]));
  await nextTick();
  oz.handleOzkieUplink(L, B, Buffer.from([0, 0x58, 0x01]));
  await p1;
  const p3 = relay(Buffer.from([0, 0x52, 0x00]));
  await nextTick();
  const err = Buffer.from([0, 0x01, 0x63, 0x61, 0x62, 0x63]);
  oz.handleOzkieUplink(L, B, err);
  const r3 = await p3;
  assert.ok(Buffer.from(r3.body.frame_b64, 'base64').equals(err));
  assert.equal(audits.length, 0);
  const orphan = await relay(Buffer.from([0, 0x52, 0x00]));
  assert.equal(orphan.status, 409);
  assert.equal(orphan.body.code, 'edhoc_no_lease');
  const before = oz.ozkieCounters().uplink_no_lease || 0;
  oz.handleOzkieUplink(L, B, Buffer.from([0, 0x01]));
  assert.equal(oz.ozkieCounters().uplink_no_lease, before + 1);
});

test('EDHOC: no message_2 is 504 and frees the lease', async () => {
  const r = await relay(Buffer.from([0, 0x03]));
  assert.equal(r.status, 504);
  const p = relay(Buffer.from([0, 0x03]), { appId: 'app-b' });
  await nextTick();
  oz.handleOzkieUplink(L, B, Buffer.from([0, 0x58]));
  assert.equal((await p).status, 200);
  await relay(Buffer.from([0, 0x52])); // finish app-b's handshake
});

test('failover: moving bridge_id aborts pinned exchanges at once (bridge_moved, outcome unknown)', async () => {
  const pReq = relay(coap(tok(4)));
  const pM1 = oz.relayFrame({ siteId: 'lab', lockId: 'ozk-unit-2', bridgeId: B, appId: 'app-a', frame: Buffer.from([0, 0x03]) });
  const keep = relay(coap(tok(5)), { bridgeId: B2 });
  await nextTick();
  const t0 = Date.now();
  oz.abortPinned(L, B2);
  oz.abortPinned('ozk-unit-2', B2);
  for (const r of [await pReq, await pM1]) {
    assert.equal(r.status, 503);
    assert.equal(r.body.code, 'bridge_moved');
    assert.equal(r.body.outcome, 'unknown');
  }
  assert.ok(Date.now() - t0 < 100, 'aborted without waiting for the timeout');
  oz.handleOzkieUplink(L, B2, coap(tok(5), 0x44));
  assert.equal((await keep).status, 200, 'an exchange already on the new bridge is kept');
});

test('topic over 200 B refused', async () => {
  const r = await oz.relayFrame({ siteId: 's'.repeat(80), lockId: 'l'.repeat(64), bridgeId: 'b'.repeat(64), appId: 'a', frame: coap(tok(6)) });
  assert.equal(r.status, 500);
  assert.equal(r.body.code, 'topic_too_long');
});
