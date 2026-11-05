'use strict';
/**
 * S1e-5 regression: ozlodge's pool and its one-time last_synced_at clear
 * (dbutc.js). Runs against a scratch DB on the local MySQL, never the live
 * `ozkey`/`ozlodge` DBs, and never spawns the server (that would join the
 * lab broker and advertise over mDNS).
 *
 * Node is put well EAST of MySQL's current offset, and the test checks that
 * the gap is real before asserting anything. Without the gap an unpinned
 * pool reads its own NOW() back correctly and the suite would pass with or
 * without the fix (S1e-4's negative control (b) found exactly that).
 */
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const mysql = require('mysql2/promise');
const { createUtcPool, clearPreUtcSyncStamps, S1E5_FLAG } = require('../dbutc');

function dbPassword() {
  if (process.env.OZLODGE_DB_PASSWORD) return process.env.OZLODGE_DB_PASSWORD;
  const file = path.join(__dirname, '..', '.env');
  if (fs.existsSync(file)) {
    for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
      const m = line.match(/^\s*OZLODGE_DB_PASSWORD\s*=\s*(.*?)\s*$/);
      if (m) return m[1].replace(/^(['"])(.*)\1$/, '$2');
    }
  }
  throw new Error('OZLODGE_DB_PASSWORD not set (env or ozlodge/server/.env)');
}

const password = dbPassword();
const dbName = `zzz_ozlodge_s1e5_${process.pid}`;
const logs = [];
const log = (level, message) => logs.push(`${level} ${message}`);
let admin;
let pool;

test.before(async () => {
  admin = await mysql.createConnection({ host: 'localhost', user: 'root', password });
  const [[{ m }]] = await admin.query('SELECT TIMESTAMPDIFF(MINUTE, UTC_TIMESTAMP(), NOW()) AS m');
  // UTC+14 is as far east as any zone; only MySQL at +13/+14 forces the west.
  process.env.TZ = Number(m) < 13 * 60 ? 'Pacific/Kiritimati' : 'Etc/GMT+12';
  const nodeMin = -new Date().getTimezoneOffset();
  assert.ok(
    Math.abs(nodeMin - Number(m)) >= 60,
    `precondition: Node (${process.env.TZ}, ${nodeMin} min) must differ from MySQL (${m} min) by >= 1 h`
  );
  await admin.query(`CREATE DATABASE \`${dbName}\``);
  pool = createUtcPool(mysql, { host: 'localhost', user: 'root', password, database: dbName, connectionLimit: 2 }, log);
  await pool.query(`CREATE TABLE rooms (
    id INT AUTO_INCREMENT PRIMARY KEY,
    room_no VARCHAR(50) UNIQUE,
    last_synced_at DATETIME NULL
  ) ENGINE=InnoDB`);
});

test.after(async () => {
  if (pool) await pool.end();
  if (admin) {
    await admin.query(`DROP DATABASE IF EXISTS \`${dbName}\``);
    await admin.end();
  }
});

const ageS = (d) => Math.abs(Date.now() - new Date(d).getTime()) / 1000;

test('a stamp written by SQL NOW() reads back as the current instant', async () => {
  await pool.query("INSERT INTO rooms (room_no, last_synced_at) VALUES ('now-1', NOW())");
  const [[r]] = await pool.query("SELECT last_synced_at FROM rooms WHERE room_no = 'now-1'");
  assert.ok(ageS(r.last_synced_at) < 60, `read back ${r.last_synced_at.toISOString()}, ${ageS(r.last_synced_at)} s from now`);
});

test('a JS Date bound as a parameter is stored as UTC wall time', async () => {
  await pool.query("INSERT INTO rooms (room_no, last_synced_at) VALUES ('js-1', ?)", [new Date()]);
  const [[{ d }]] = await pool.query(
    "SELECT TIMESTAMPDIFF(SECOND, last_synced_at, UTC_TIMESTAMP()) AS d FROM rooms WHERE room_no = 'js-1'"
  );
  assert.ok(Math.abs(Number(d)) < 60, `stored ${d} s away from UTC_TIMESTAMP()`);
});

test('the one-time clear NULLs old stamps once, then never again', async () => {
  await pool.query('DELETE FROM rooms');
  // Two legacy rows as the live `ozkey` DB has them (stamps in an unknown zone)
  // plus a room that was never synced.
  await pool.query(`INSERT INTO rooms (room_no, last_synced_at) VALUES
    ('101', '2026-07-12 04:39:17'), ('102', '2026-07-12 04:39:17'), ('103', NULL)`);

  assert.strictEqual(await clearPreUtcSyncStamps(pool, log), 2);
  const [[{ n }]] = await pool.query('SELECT COUNT(*) AS n FROM rooms WHERE last_synced_at IS NOT NULL');
  assert.strictEqual(Number(n), 0);
  const [flag] = await pool.query('SELECT 1 FROM schema_flags WHERE name = ?', [S1E5_FLAG]);
  assert.strictEqual(flag.length, 1);
  assert.ok(logs.some((l) => l.includes('S1e-5 UTC migration: cleared last_synced_at on 2 room(s)')));

  // A sync after the fix writes a real stamp; a later boot must keep it.
  await pool.query("UPDATE rooms SET last_synced_at = NOW() WHERE room_no = '101'");
  assert.strictEqual(await clearPreUtcSyncStamps(pool, log), null);
  const [[r]] = await pool.query("SELECT last_synced_at FROM rooms WHERE room_no = '101'");
  assert.ok(r.last_synced_at && ageS(r.last_synced_at) < 60, 'post-fix stamp kept by the second boot');
});

test('on a fresh DB the clear touches nothing and just sets the flag', async () => {
  await pool.query('DELETE FROM schema_flags');
  await pool.query('UPDATE rooms SET last_synced_at = NULL');
  assert.strictEqual(await clearPreUtcSyncStamps(pool, log), 0);
  const [flag] = await pool.query('SELECT 1 FROM schema_flags WHERE name = ?', [S1E5_FLAG]);
  assert.strictEqual(flag.length, 1);
});
