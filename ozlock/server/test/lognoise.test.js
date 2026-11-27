'use strict';
/*
 * OZLOCK-LOG-NOISE-1 (PM-approved 11-27): bridge and lock presence are logged
 * on CHANGE only, plus one summary line per period (default 1 h).
 *
 * Why: the XF-152 presence resync (unsubscribe + subscribe every 30 s) makes
 * the broker redeliver every retained presence, and each redelivery logged
 * "Presence: bridge X -> state", "re-verdicted N lock(s)" and "Presence: lock
 * Y -> state" again. Lab ozlock 1ea3168, last 3000 lines on 11-27: 1189 +
 * 1188 + 594 of them were these repeats.
 *
 * Unchanged: the DB writes and the re-verdict itself run on every report
 * (only the log line is skipped). The resync timer is pushed out to an hour
 * here, so only this file's publishes arrive.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { Fixture, x25519FromSeed, waitFor, sleep } = require('./harness');

const SUMMARY_MS = 3000;
const fx = new Fixture({
  env: {
    OZLOCK_FAILOVER_SWEEP_MS: String(3600 * 1000),
    OZLOCK_PRESENCE_SUMMARY_MS: String(SUMMARY_MS),
  },
});
const OWNER = x25519FromSeed('lognoise-owner');
const L = 'ozk-lognoise-l1';
const B = 'ozb-lognoise-b1';
const TB = (id) => `ozkie/lab/bridges/${id}/presence`;
const TL = (id) => `ozkie/lab/locks/${id}/presence`;

const events = async () => (await fx.http('GET', '/events')).body.events.map((e) => e.message);
const count = async (re) => (await events()).filter((m) => re.test(m)).length;
const bridgeLine = new RegExp(`^Presence: bridge ${B} -> `);
const reverdict = new RegExp(`re-verdicted \\d+ lock\\(s\\) behind bridge ${B}$`);
const lockLine = new RegExp(`^Presence: lock ${L} -> `);
const bridgeAt = async () => (await fx.query('SELECT presence_at FROM bridges_presence WHERE bridge_id = ?', [B]))[0];

before(async () => {
  await fx.start();
  const { status } = await fx.http('POST', '/pairings', { app_id: OWNER.hex, device_id: L, bridge_id: B, ozkie_version: 2 });
  assert.equal(status, 200);
});
after(() => fx.stop());

test('LN-1: the same bridge presence 3 times → one bridge line and one re-verdict line; the DB still moves', async () => {
  await fx.pub(TB(B), { state: 'online' });
  await waitFor(async () => (await count(bridgeLine)) === 1, { what: 'first bridge line' });
  const first = await bridgeAt();
  await sleep(1100); // presence_at has 1 s resolution
  await fx.pub(TB(B), { state: 'online' });
  await fx.pub(TB(B), { state: 'online' });
  await waitFor(async () => (await bridgeAt()).presence_at > first.presence_at, { what: 'presence_at refreshed by the repeat' });
  await sleep(300);
  assert.equal(await count(bridgeLine), 1, 'repeats not logged');
  assert.equal(await count(reverdict), 1, 're-verdict line only with the change');
});

test('LN-2: a bridge change is logged at once; a new reason counts as a change', async () => {
  await fx.pub(TB(B), { state: 'offline', reason: 'lwt' });
  await waitFor(async () => (await count(bridgeLine)) === 2, { what: 'offline logged' });
  await fx.pub(TB(B), { state: 'offline', reason: 'lwt' });
  await fx.pub(TB(B), { state: 'offline', reason: 'peer_down' });
  await waitFor(async () => (await count(bridgeLine)) === 3, { what: 'reason change logged' });
  await sleep(300);
  assert.equal(await count(bridgeLine), 3);
  assert.equal(await count(reverdict), 3);
  await fx.pub(TB(B), { state: 'online' });
  await waitFor(async () => (await count(bridgeLine)) === 4, { what: 'back online logged' });
});

test('LN-3: the same lock presence 3 times → one lock line; a change is logged', async () => {
  const n0 = await count(lockLine);
  for (let i = 0; i < 3; i++) await fx.pub(TL(L), { state: 'online', via: B, ozkie_version: 2 });
  await waitFor(async () => (await count(lockLine)) === n0 + 1, { what: 'lock online logged once' });
  await sleep(500);
  assert.equal(await count(lockLine), n0 + 1, 'repeats not logged');
  await fx.pub(TL(L), { state: 'offline', reason: 'lwt', via: B });
  await waitFor(async () => (await count(lockLine)) === n0 + 2, { what: 'lock offline logged' });
});

test('LN-4: one summary line per period with the counts of what was not logged', async () => {
  const re = /^Presence summary \(last \d+ s\): (\d+) repeat report\(s\) not logged \(bridges (\d+), locks (\d+)\)/;
  await waitFor(async () => (await events()).some((m) => re.test(m)), { what: 'summary line', timeoutMs: SUMMARY_MS * 3 });
  const sums = (await events()).filter((m) => re.test(m)).map((m) => m.match(re).slice(1).map(Number));
  const total = sums.reduce((a, [t]) => a + t, 0);
  const br = sums.reduce((a, [, b]) => a + b, 0);
  const lk = sums.reduce((a, [, , l]) => a + l, 0);
  assert.equal(br, 3, `bridge repeats (LN-1 2 + LN-2 1): ${JSON.stringify(sums)}`);
  assert.equal(lk, 2, `lock repeats (LN-3): ${JSON.stringify(sums)}`);
  assert.equal(total, br + lk);
  // the counters reset: the next period's summary reports 0 when nothing repeats
  const n = sums.length;
  await waitFor(async () => (await events()).filter((m) => re.test(m)).length > n, { what: 'next summary', timeoutMs: SUMMARY_MS * 3 });
  const last = (await events()).filter((m) => re.test(m)).pop().match(re);
  assert.equal(Number(last[1]), 0, last[0]);
});
