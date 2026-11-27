'use strict';
/*
 * OZKIE-RELAY-TIMING-1 (PM nexus-1b GO 2026-11-27; proposal
 * ozkey_notes/2026-11-27_OZKIE-RELAY-TIMING-1_proposal.md, ozkey-fw's view §4).
 *   S1  after a message_1 timeout (504), the lock's relay EDHOC "settles" for
 *       OZKIE_EDHOC_SETTLE_MS: a new message_1 from anyone is 409
 *       edhoc_settling (retry_after_ms), and every type 0 uplink in that time
 *       is stale: dropped and counted, never handed to anyone.
 *   S2  message_3 + silence is NOT success: 200 {frame_b64: null,
 *       confirmed: false}, logged "delivered, unconfirmed", no audit row. The
 *       app's first OSCORE answer is the confirmation (ozkey-48 §3); the lock
 *       may have refused with STORAGE_FAILED or never got message_3.
 *   S3  the message_3 window default is 5 s (was 2 s).
 * Unit level: transport and audit stubbed, no DB, no broker.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const { ctx } = require('../ctx');

const SETTLE = 400;
const logs = [];
const audits = [];
Object.assign(ctx, {
  CONFIG: { SITE_ID: 'lab', OZKIE_WAIT_MS: 200, OZKIE_EDHOC_LEASE_MS: 2000, OZKIE_M3_WAIT_MS: 150, OZKIE_EDHOC_SETTLE_MS: SETTLE },
  logEvent: (lvl, msg) => logs.push(`${lvl} ${msg}`),
});
require('../transport').mqttPublishRaw = () => true;
require('../audit').recordAudit = async (...a) => audits.push(a);

const oz = require('../ozkie');
const B = 'ozb-t-1';
const relay = (lockId, frame, appId = 'app-a') => oz.relayFrame({ siteId: 'lab', lockId, bridgeId: B, appId, frame });
const nextTick = () => new Promise((r) => setImmediate(r));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const M1 = Buffer.from([0, 0x03, 0x02, 0x58]);
const M2 = Buffer.from([0, 0x58, 0x20, 1, 2]);
const M3 = Buffer.from([0, 0x52, 0xe5, 0x62]);

test('S1: after a message_1 timeout, a new message_1 (any sender) is refused while the lock settles', async () => {
  const L = 'ozk-t-settle';
  const r = await relay(L, M1);
  assert.equal(r.status, 504);
  for (const app of ['app-a', 'app-b']) {
    const again = await relay(L, M1, app);
    assert.equal(again.status, 409, app);
    assert.equal(again.body.code, 'edhoc_settling');
    assert.ok(again.body.retry_after_ms > 0 && again.body.retry_after_ms <= SETTLE);
  }
});

test('S1: a late message_2 during the settle is stale: dropped, counted, handed to nobody', async () => {
  const L = 'ozk-t-stale';
  assert.equal((await relay(L, M1)).status, 504);
  const before = oz.ozkieCounters().uplink_settling || 0;
  oz.handleOzkieUplink(L, B, M2);
  assert.equal(oz.ozkieCounters().uplink_settling, before + 1);
  // after the settle a fresh handshake works and gets ITS message_2, not the stale one
  await sleep(SETTLE + 50);
  const p = relay(L, M1);
  await nextTick();
  const fresh = Buffer.from([0, 0x58, 0x20, 9, 9]);
  oz.handleOzkieUplink(L, B, fresh);
  const r = await p;
  assert.equal(r.status, 200);
  assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(fresh));
  await relay(L, M3);
});

test('S1: the settle is per lock; another lock is not affected', async () => {
  assert.equal((await relay('ozk-t-a', M1)).status, 504);
  const p = relay('ozk-t-b', M1);
  await nextTick();
  oz.handleOzkieUplink('ozk-t-b', B, M2);
  assert.equal((await p).status, 200);
  await relay('ozk-t-b', M3);
});

test('S2: message_3 + silence is "delivered, unconfirmed": confirmed:false, no audit row, no "completed" log', async () => {
  const L = 'ozk-t-m3';
  audits.length = 0;
  logs.length = 0;
  const p = relay(L, M1);
  await nextTick();
  oz.handleOzkieUplink(L, B, M2);
  assert.equal((await p).status, 200);
  const r3 = await relay(L, M3);
  assert.equal(r3.status, 200);
  assert.equal(r3.body.frame_b64, null);
  assert.equal(r3.body.confirmed, false);
  assert.equal(audits.length, 0, 'no completion is audited at message_3');
  assert.ok(!logs.some((l) => /handshake completed/.test(l)), 'no "completed" claim');
  assert.ok(logs.some((l) => /message_3 delivered, unconfirmed/.test(l)));
  // the lease is released: the same app can start again at once
  const p2 = relay(L, M1);
  await nextTick();
  oz.handleOzkieUplink(L, B, M2);
  assert.equal((await p2).status, 200);
  await relay(L, M3);
});

test('S2: a lock refusal after message_3 (e.g. STORAGE_FAILED) comes back with confirmed:false', async () => {
  const L = 'ozk-t-refuse';
  const p = relay(L, M1);
  await nextTick();
  oz.handleOzkieUplink(L, B, M2);
  await p;
  const p3 = relay(L, M3);
  await nextTick();
  const err = Buffer.from([0, 0x01, 0x18, 0x25]);
  oz.handleOzkieUplink(L, B, err);
  const r3 = await p3;
  assert.equal(r3.status, 200);
  assert.ok(Buffer.from(r3.body.frame_b64, 'base64').equals(err));
  assert.equal(r3.body.confirmed, false);
});

test('S3: default message_3 window is 5 s; settle default 10 s', () => {
  const saved = { ...ctx.CONFIG };
  try {
    delete ctx.CONFIG.OZKIE_M3_WAIT_MS;
    delete ctx.CONFIG.OZKIE_EDHOC_SETTLE_MS;
    const t = oz.ozkieTimings();
    assert.equal(t.m3WaitMs, 5000);
    assert.equal(t.settleMs, 10000);
  } finally {
    ctx.CONFIG = saved;
  }
});
