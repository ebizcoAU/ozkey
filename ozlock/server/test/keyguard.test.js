'use strict';
/*
 * S1e-KEYGUARD (PM nexus-9e, 2026-11-05): a missing server ECDH key file must
 * never silently become a new server identity on a DB that is already in use.
 * The S1e-4 lab boot ran from a worktree without the key file and came up as
 * a different server; this suite pins the rule that stops that.
 *
 * The fixture points OZLOCK_SERVER_ECDH_KEY_PATH at its own temp dir (see
 * harness.js), so moving the key away here never touches a real one.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const harness = require('./harness');

const fx = new harness.Fixture();
let keyPath;
const keyBytes = () => fs.readFileSync(keyPath, 'utf8');
const refusedWithoutServing = (out) => {
  assert.match(out, /S1e-KEYGUARD: refusing to start\./);
  assert.doesNotMatch(out, /MQTT online|HTTP directory listening/);
};

before(async () => {
  await fx.start();
  keyPath = path.join(fx.tmp, 'server_ecdh_key.json');
  fx.siteId = (await fx.query('SELECT id FROM sites LIMIT 1'))[0].id;
});
after(() => fx.stop());

test('fresh DB: the first boot creates the identity, 0600', () => {
  // Fresh = no owners/locks BEFORE schema init; init then seeds the lab owner.
  // Counting after init refused this very boot (caught writing this suite).
  assert.match(fx.log(), /fresh DB, generated the server ECDH identity/);
  assert.equal(fs.statSync(keyPath).mode & 0o777, 0o600);
});

test('key present: a restart keeps the same identity', async () => {
  const before = keyBytes();
  await fx.restartServer();
  assert.equal(keyBytes(), before);
});

test('key missing, DB has an owner: refuses to start and creates nothing', async () => {
  // The first boot seeded the lab owner, so this DB is in use from here on.
  const saved = keyBytes();
  fs.rmSync(keyPath);
  const { code, out } = await fx.bootExpectingExit();
  assert.equal(code, 1, out);
  refusedWithoutServing(out);
  assert.match(out, /server ECDH identity missing: .* already has 1 owner\(s\) and 0 lock\(s\)/);
  assert.match(out, /--new-identity/);
  assert.equal(fs.existsSync(keyPath), false, 'a refused boot must not write a key');
  fs.writeFileSync(keyPath, saved, { mode: 0o600 });
  await fx.restartServer();
});

test('key missing, DB has owners and locks: refused, both counted', async () => {
  // The seeded owner can't be removed (sites FK), so locks are counted alongside it.
  await fx.query("INSERT INTO locks (id, site_id) VALUES ('ozk-keyguard', ?)", [fx.siteId]);
  const saved = keyBytes();
  fs.rmSync(keyPath);
  const { code, out } = await fx.bootExpectingExit();
  assert.equal(code, 1, out);
  refusedWithoutServing(out);
  assert.match(out, /already has 1 owner\(s\) and 1 lock\(s\)/);
  assert.equal(fs.existsSync(keyPath), false);
  fs.writeFileSync(keyPath, saved, { mode: 0o600 });
  await fx.restartServer();
});

test('--new-identity with the key present is refused and the key is kept', async () => {
  const saved = keyBytes();
  const { code, out } = await fx.bootExpectingExit(['--new-identity']);
  assert.equal(code, 1, out);
  refusedWithoutServing(out);
  assert.match(out, /only allows creating a missing key; it never replaces one/);
  assert.equal(keyBytes(), saved);
  await fx.restartServer();
});

test('--new-identity with the key missing creates a new one, loudly', async () => {
  const old = keyBytes();
  fs.rmSync(keyPath);
  fx.extraArgs = ['--new-identity'];
  try {
    await fx.restartServer();
  } finally {
    fx.extraArgs = [];
  }
  assert.notEqual(keyBytes(), old);
  assert.equal(fs.statSync(keyPath).mode & 0o777, 0o600);
  assert.match(fx.log(), /WARN .*--new-identity: created a NEW server ECDH identity for a DB with 1 owner\(s\) and 1 lock\(s\)/);
  // And the next plain boot uses it without complaint.
  const now = keyBytes();
  await fx.restartServer();
  assert.equal(keyBytes(), now);
});

// The guard and C0a's console:create-admin CLI together (PM nexus-9e). As
// designed: the CLI never needs the key, but it touches the DB like a boot
// (initDatabase seeds the lab owner), so it settles the identity by the same
// rule. In-use DB + no key: the admin is created, no key is written, a warning
// says the server will refuse, and the server does refuse. Fresh DB + no key:
// the CLI creates the identity, so "create the admin, then start the server"
// works on a new deployment instead of locking its first boot out.
test('CLI on an in-use DB with no key: admin created, no key written, the server still refuses', async () => {
  const saved = keyBytes();
  fs.rmSync(keyPath);
  await fx.stopServer();
  const r = await fx.runCli(['console:create-admin', 'keyguard-cli@example.com', 'Keyguard']);
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /CONSOLE ADMIN INVITE /);
  assert.match(r.out, /WARN .*S1e-KEYGUARD: the server will refuse to start until this is fixed\./);
  assert.equal(fs.existsSync(keyPath), false, 'the CLI must not create a key for an in-use DB');
  const boot = await fx.bootExpectingExit();
  assert.equal(boot.code, 1, boot.out);
  refusedWithoutServing(boot.out);
  fs.writeFileSync(keyPath, saved, { mode: 0o600 });
  await fx.restartServer();
});

test('CLI first on a fresh DB creates the identity; the first boot then uses it', async () => {
  const mysql = require('mysql2/promise');
  const usedDb = fx.dbName;
  const usedKey = keyBytes();
  const freshDb = `${usedDb}_fresh`;
  await fx.stopServer();
  fs.rmSync(keyPath);
  fx.dbName = freshDb; // serverEnv() reads it at each spawn
  try {
    const r = await fx.runCli(['console:create-admin', 'keyguard-fresh@example.com']);
    assert.equal(r.code, 0, r.out);
    assert.match(r.out, /fresh DB, generated the server ECDH identity/);
    assert.doesNotMatch(r.out, /S1e-KEYGUARD/);
    const minted = keyBytes();
    await fx.restartServer(); // must come up: the DB now has an owner, and it has its key
    assert.equal(keyBytes(), minted);
  } finally {
    await fx.stopServer();
    fx.dbName = usedDb;
    const admin = await mysql.createConnection({ host: 'localhost', user: 'root', password: fx.dbPassword });
    await admin.query(`DROP DATABASE IF EXISTS \`${freshDb}\``);
    await admin.end();
    fs.writeFileSync(keyPath, usedKey, { mode: 0o600 });
    await fx.restartServer();
  }
});
