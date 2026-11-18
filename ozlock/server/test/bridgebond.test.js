'use strict';
/*
 * Bridge bond, server half (ozkey-56 §5; F1-BRIDGE-BOND-1 agreed 2026-11-06,
 * XF draft 2026-11-06_XF-draft_bridge-bond-enrolment.md steps 5 + revoke).
 *   - the bridge's OZKIE key is pinned from presence on first sight (TOFU);
 *     a different key later is a conflict, served null, never re-pinned;
 *   - GET /bridges/:id is owner-scoped (404 otherwise, no probing);
 *   - PUT|DELETE /locks/:l/bridges/:b/bond is the app's advisory record:
 *     fingerprint = first 16 bytes of SHA-256(raw pub), max 2 per lock,
 *     server-set bonded_at, idempotent.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const { Fixture, x25519FromSeed, waitFor } = require('./harness');

const fx = new Fixture();
const OWNER = x25519FromSeed('bridgebond-owner');
const OTHER = x25519FromSeed('bridgebond-other');
const L = 'ozk-bond-lock01';
const [B1, B2, B3] = ['ozb-bond-one', 'ozb-bond-two', 'ozb-bond-three'];
const PUB1 = 'a1'.repeat(32);
const PUB2 = 'b2'.repeat(32);
const PUB1_EVIL = 'ee'.repeat(32);
const fp = (hex) => crypto.createHash('sha256').update(Buffer.from(hex, 'hex')).digest('hex').slice(0, 32);

const TB = (kind, id) => `ozkie/lab/bridges/${id}/${kind}`;
const bond = (b, body, method = 'PUT') => fx.http(method, `/locks/${L}/bridges/${b}/bond`, body);
const getBridge = (b, app) => fx.http('GET', `/bridges/${b}${app ? `?app_id=${app}` : ''}`);

before(async () => {
  await fx.start();
});
after(async () => {
  await fx.stop();
});

test('setup: B1 presents its key; L paired to B1, B2, then B3 (all in lock_bridges)', async () => {
  await fx.pub(TB('presence', B1), { state: 'online', ozkie_pub: PUB1 });
  await fx.pub(TB('presence', B2), { state: 'online', ozkie_pub: PUB2 });
  await fx.pub(TB('presence', B3), { state: 'online' });
  let r = await fx.http('POST', '/pairings', { app_id: OWNER.hex, device_id: L, bridge_id: B1, ozkie_version: 2 });
  assert.equal(r.status, 200);
  for (const b of [B2, B3]) {
    r = await fx.http('PATCH', `/locks/${L}`, { bridge_id: b });
    assert.equal(r.status, 200);
  }
  await waitFor(async () => (await fx.query('SELECT ozkie_pub FROM bridges WHERE id = ?', [B2]))[0]?.ozkie_pub === PUB2, {
    what: 'B2 pinned',
  });
});

test('TOFU pin: the owner reads it; a stranger or no app_id gets 404; a keyless bridge is not_seen', async () => {
  let r = await getBridge(B1, OWNER.hex);
  assert.equal(r.status, 200);
  assert.deepEqual(r.body, { ok: true, id: B1, ozkie_pub: PUB1, reason: null });
  assert.equal((await getBridge(B1, OTHER.hex)).status, 404);
  assert.equal((await getBridge(B1)).status, 404);
  assert.equal((await getBridge('ozb-nobody', OWNER.hex)).status, 404);
  r = await getBridge(B3, OWNER.hex);
  assert.deepEqual(r.body, { ok: true, id: B3, ozkie_pub: null, reason: 'not_seen' });
});

test('a pinned bridge still gets broker credentials (the pin-only row does not block minting)', async () => {
  await waitFor(() => fx.credentialed.has(B1) && fx.credentialed.has(B2), { what: 'credentials minted' });
  const [row] = await fx.query('SELECT broker_secret FROM bridges WHERE id = ?', [B1]);
  assert.ok(row.broker_secret, 'secret stored');
});

test('a different key later is a conflict: kept pinned, served null, audited once', async () => {
  await fx.pub(TB('presence', B1), { state: 'online', ozkie_pub: PUB1_EVIL });
  await fx.pub(TB('presence', B1), { state: 'online', ozkie_pub: PUB1_EVIL });
  await waitFor(async () => (await getBridge(B1, OWNER.hex)).body.reason === 'conflict', { what: 'conflict' });
  const r = await getBridge(B1, OWNER.hex);
  assert.equal(r.body.ozkie_pub, null);
  const [row] = await fx.query('SELECT ozkie_pub, ozkie_pub_conflict FROM bridges WHERE id = ?', [B1]);
  assert.equal(row.ozkie_pub, PUB1, 'never re-pinned');
  assert.equal(row.ozkie_pub_conflict, PUB1_EVIL);
  const audits = await fx.query("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'bridge_key_conflict'");
  assert.equal(Number(audits[0].n), 1);
});

test('PUT bond: input, owner and pairing checks', async () => {
  const ok = { app_id: OWNER.hex, bond_pub_fp: fp(PUB2), slot: 2 };
  for (const [name, b, body, status, code] of [
    ['fp not hex32', B2, { ...ok, bond_pub_fp: 'XYZ' }, 400, 'invalid_bond_pub_fp'],
    ['fp uppercase', B2, { ...ok, bond_pub_fp: fp(PUB2).toUpperCase() }, 400, 'invalid_bond_pub_fp'],
    ['slot', B2, { ...ok, slot: 'two' }, 400, 'invalid_slot'],
    ['not owner', B2, { ...ok, app_id: OTHER.hex }, 403, 'owner_only'],
    ['not paired', 'ozb-elsewhere', ok, 404, 'bridge_not_paired'],
  ]) {
    const r = await bond(b, body);
    assert.equal(r.status, status, name);
    assert.equal(r.body.code, code, name);
  }
});

test('PUT bond: recorded with a server time, idempotent, pub verified', async () => {
  const body = { app_id: OWNER.hex, bond_pub_fp: fp(PUB2), slot: 2 };
  for (let i = 0; i < 2; i++) {
    const r = await bond(B2, body);
    assert.equal(r.status, 200);
    assert.deepEqual(r.body, { ok: true, recorded: true, pub_verified: true });
  }
  const [row] = await fx.query('SELECT bond_pub_fp, bond_slot, bonded_at FROM lock_bridges WHERE lock_id = ? AND bridge_id = ?', [L, B2]);
  assert.equal(row.bond_pub_fp, fp(PUB2));
  assert.equal(row.bond_slot, 2);
  assert.ok(row.bonded_at);
});

test('PUT bond: a fingerprint that does not match the pinned key is recorded with 409 pub_mismatch', async () => {
  const r = await bond(B1, { app_id: OWNER.hex, bond_pub_fp: fp(PUB1_EVIL), slot: 3 });
  assert.equal(r.status, 409);
  assert.equal(r.body.code, 'pub_mismatch');
  assert.equal(r.body.recorded, true);
  const [row] = await fx.query('SELECT bonded_at FROM lock_bridges WHERE lock_id = ? AND bridge_id = ?', [L, B1]);
  assert.ok(row.bonded_at);
});

test('max 2: a third bridge is refused and not recorded; after a DELETE it fits', async () => {
  const third = { app_id: OWNER.hex, bond_pub_fp: 'c3'.repeat(16), slot: 4 };
  let r = await bond(B3, third);
  assert.equal(r.status, 409);
  assert.equal(r.body.code, 'bridge_bonds_full');
  let [row] = await fx.query('SELECT bonded_at FROM lock_bridges WHERE lock_id = ? AND bridge_id = ?', [L, B3]);
  assert.equal(row.bonded_at, null);

  assert.equal((await bond(B1, { app_id: OTHER.hex }, 'DELETE')).status, 403);
  r = await bond(B1, { app_id: OWNER.hex }, 'DELETE');
  assert.deepEqual(r.body, { ok: true, cleared: true });
  r = await bond(B1, { app_id: OWNER.hex }, 'DELETE');
  assert.deepEqual(r.body, { ok: true, cleared: false }, 'idempotent');

  r = await bond(B3, third);
  assert.equal(r.status, 200);
  assert.equal(r.body.pub_verified, false, 'B3 never presented a key');
  [row] = await fx.query('SELECT COUNT(*) AS n FROM lock_bridges WHERE lock_id = ? AND bonded_at IS NOT NULL', [L]);
  assert.equal(Number(row.n), 2);
});
