'use strict';
/*
 * F1 server side: message_4 on the relay link (ozkey-48 rev 15 §3, §2.4.3;
 * ozkey-fw's frame note ozkey_notes/2026-11-27_F1_message4_frame_for_ozkey-srv.md).
 * After message_3 the lock sends one type-0 uplink:
 *   00 48 <8-byte tag>  message_4: context saved   → 200 confirmed:true (frame passed on;
 *                                                    the app verifies the tag, ozlock can't)
 *   00 01 ...           EDHOC error (STORAGE_FAILED, BOND_DENIED, ...) → 200 confirmed:false, frame passed on
 *   silence             → 504 outcome "unknown", never success
 * ozlock tells message_4 from an error by the first byte after the type, and the length.
 * Unit level: transport and audit stubbed, no DB, no broker.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const { ctx } = require('../ctx');

const logs = [];
const audits = [];
Object.assign(ctx, {
  CONFIG: { SITE_ID: 'lab', OZKIE_WAIT_MS: 200, OZKIE_EDHOC_LEASE_MS: 2000, OZKIE_M3_WAIT_MS: 150, OZKIE_EDHOC_SETTLE_MS: 400 },
  logEvent: (lvl, msg) => logs.push(`${lvl} ${msg}`),
});
require('../transport').mqttPublishRaw = () => true;
require('../audit').recordAudit = async (...a) => audits.push(a);

const oz = require('../ozkie');
const B = 'ozb-t-1';
const relay = (lockId, frame, appId = 'app-a') => oz.relayFrame({ siteId: 'lab', lockId, bridgeId: B, appId, frame });
const nextTick = () => new Promise((r) => setImmediate(r));
const M1 = Buffer.from([0, 0x03, 0x02, 0x58]);
const M2 = Buffer.from([0, 0x58, 0x20, 1, 2]);
const M3 = Buffer.from([0, 0x52, 0xe5, 0x62]);
// RFC 9529 §3.6 trace message_4, as the whole MQTT payload (type byte first).
const M4_RFC = Buffer.from('AEgoyWa3yjBPgw==', 'base64');
// Captured from the host lock (test_lock M4 rows), ozkey-fw's note.
const M4_HOST = ['0048366f8c44938eafe7', '00484d5bd3469e7848ff', '00488a8a56d5e29c826d'].map((h) => Buffer.from(h, 'hex'));

async function toM3(L) {
  const p = relay(L, M1);
  await nextTick();
  oz.handleOzkieUplink(L, B, M2);
  assert.equal((await p).status, 200);
}
async function m3Answer(L, uplink) {
  await toM3(L);
  const p3 = relay(L, M3);
  await nextTick();
  if (uplink) oz.handleOzkieUplink(L, B, uplink);
  return p3;
}

test('fixture: the RFC 9529 message_4 payload is 00 48 28 c9 66 b7 ca 30 4f 83', () => {
  assert.equal(M4_RFC.toString('hex'), '004828c966b7ca304f83');
});

test('M4-1: message_4 (RFC 9529 trace) after message_3 → 200 confirmed:true, frame passed on unchanged', async () => {
  logs.length = 0;
  audits.length = 0;
  const r = await m3Answer('ozk-m4-rfc', M4_RFC);
  assert.equal(r.status, 200);
  assert.equal(r.body.confirmed, true);
  assert.equal(r.body.frame_b64, 'AEgoyWa3yjBPgw==');
  assert.ok(logs.some((l) => /message_3 confirmed by message_4/.test(l)), logs.join('\n'));
  assert.equal(audits.length, 0, 'transit only: no audit row for a handshake');
});

test('M4-2: the host lock\'s captured message_4 frames are all confirmed', async () => {
  for (const [i, m4] of M4_HOST.entries()) {
    const r = await m3Answer(`ozk-m4-host${i}`, m4);
    assert.equal(r.status, 200);
    assert.equal(r.body.confirmed, true, m4.toString('hex'));
    assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(m4));
  }
});

test('M4-3: an EDHOC error after message_3 (STORAGE_FAILED) → 200 confirmed:false, frame passed on', async () => {
  // (1, "STORAGE_FAILED") as a CBOR sequence: 01 6e "STORAGE_FAILED"
  const err = Buffer.concat([Buffer.from([0, 0x01, 0x6e]), Buffer.from('STORAGE_FAILED')]);
  logs.length = 0;
  const r = await m3Answer('ozk-m4-err', err);
  assert.equal(r.status, 200);
  assert.equal(r.body.confirmed, false);
  assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(err));
  assert.ok(logs.some((l) => /refused by the lock after message_3/.test(l)), logs.join('\n'));
});

test('M4-4: message_3 + silence → 504 outcome unknown, never success; the lease is released', async () => {
  const L = 'ozk-m4-silent';
  logs.length = 0;
  const r = await m3Answer(L, null);
  assert.equal(r.status, 504);
  assert.equal(r.body.code, 'no_message_4');
  assert.equal(r.body.outcome, 'unknown');
  assert.notEqual(r.body.confirmed, true);
  assert.ok(logs.some((l) => /no message_4/.test(l)), logs.join('\n'));
  // released: the same app can start again at once (no settle after message_3)
  await toM3(L);
  await relay(L, M3);
});

test('M4-5: 0x48 with the wrong length is not a message_4: confirmed:false, frame passed on', async () => {
  for (const [i, bad] of [Buffer.from('0048112233445566', 'hex'), Buffer.from('00481122334455667788aa', 'hex')].entries()) {
    const r = await m3Answer(`ozk-m4-bad${i}`, bad);
    assert.equal(r.status, 200);
    assert.equal(r.body.confirmed, false, bad.toString('hex'));
    assert.ok(Buffer.from(r.body.frame_b64, 'base64').equals(bad));
  }
});

test('M4-6: a message_4-shaped reply to message_1 is still just message_1\'s answer (no confirmed field)', async () => {
  const L = 'ozk-m4-m1';
  const p = relay(L, M1);
  await nextTick();
  oz.handleOzkieUplink(L, B, M4_RFC);
  const r = await p;
  assert.equal(r.status, 200);
  assert.equal(r.body.confirmed, undefined);
  await relay(L, M3);
});

test('M4-7: a late message_4 after the 504 is dropped (no lease), not handed to anyone', async () => {
  const L = 'ozk-m4-late';
  assert.equal((await m3Answer(L, null)).status, 504);
  const before = oz.ozkieCounters().uplink_no_lease || 0;
  oz.handleOzkieUplink(L, B, M4_RFC);
  assert.equal(oz.ozkieCounters().uplink_no_lease, before + 1);
});

/* ozkey-57 slice 1b (PM ruled 11-19; "completed" row dropped by the PM 11-27):
 * a lock that refuses after message_3 is a failed access → ONE security row.
 * A confirmed handshake and every 504 leave no row. */
test('1b-1: a refusal after message_3 → exactly one audit row, kind security', async () => {
  audits.length = 0;
  const err = Buffer.concat([Buffer.from([0, 0x01, 0x6e]), Buffer.from('STORAGE_FAILED')]);
  await m3Answer('ozk-1b-refuse', err);
  assert.equal(audits.length, 1, JSON.stringify(audits));
  const [appId, lockId, action, detail, kind] = audits[0];
  assert.deepEqual([appId, lockId, action, kind], ['app-a', 'ozk-1b-refuse', 'ozkie_edhoc', 'security']);
  assert.match(detail, /refused by the lock after message_3/);
});

test('1b-2: a confirmed handshake, a 504 after message_3 and a 504 after message_1 leave no audit row', async () => {
  audits.length = 0;
  await m3Answer('ozk-1b-ok', M4_RFC);
  await m3Answer('ozk-1b-silent', null);
  assert.equal((await relay('ozk-1b-m1', M1)).status, 504);
  assert.equal(audits.length, 0, JSON.stringify(audits));
});
