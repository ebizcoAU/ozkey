'use strict';
/*
 * S1e-4 (ozkey-50 §7): the one-time purge of pre-UTC DATETIME state, tested
 * like the S1e-1 broker-secret migration. Rows are seeded the way an old
 * server left them, the migration flag is cleared, the server restarts on the
 * same DB, and the rows are read back.
 *
 * Old rows are naive local time in a zone we can't know (lab: JS wrote AEDT
 * +11, NOW() wrote MySQL's +07), so seeds are given as UTC + an offset. The
 * rule under test is keep-biased (PM nexus-e3, 2026-11-05): a kept verb row
 * goes only when expires_at < UTC_TIMESTAMP() - 12 h. The two rows on either
 * side of that margin are the point of the suite.
 *
 * A profile's test/utcmigration.test.js calls
 *   require('<core>/testkit/utcmigration')(harness)
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');

const UTC = 'UTC_TIMESTAMP()';
const PLUS11 = 'UTC_TIMESTAMP() + INTERVAL 11 HOUR'; // "now" as an AEDT writer saw it

module.exports = function utcMigrationSuite(harness) {
  const fx = new harness.Fixture();

  // label: [action_type, status, expires_at SQL, kept?]
  const QUEUE = {
    'revoke-live-in-+11': ['revoke-key', 'queued', `${PLUS11} + INTERVAL 10 MINUTE`, true],
    'revoke-expired-in-+11': ['revoke-key', 'queued', `${PLUS11} - INTERVAL 1 MINUTE`, true], // inside the margin
    'revoke-past-margin': ['revoke-key', 'queued', `${UTC} - INTERVAL 13 HOUR`, false],
    'bond-live': ['bond-revoke', 'queued', `${PLUS11} + INTERVAL 10 MINUTE`, true],
    'reset-live': ['factory-reset', 'queued', `${PLUS11} + INTERVAL 10 MINUTE`, true],
    'grant-noexpiry': ['grant-key', 'queued', 'NULL', true],
    'unlock-live': ['unlock', 'queued', `${PLUS11} + INTERVAL 1 MINUTE`, false],
    'assisted-live': ['assisted-unlock', 'queued', `${PLUS11} + INTERVAL 1 MINUTE`, false],
    'sent-past-margin': ['unlock', 'sent', `${UTC} - INTERVAL 2 DAY`, true], // not queued: history
  };
  // token: [used_at SQL, expires_at SQL, kept?]
  const TOKENS = {
    'tok-unused-live': ['NULL', `${PLUS11} + INTERVAL 10 MINUTE`, false],
    'tok-used-live': [PLUS11, `${PLUS11} + INTERVAL 10 MINUTE`, true],
    'tok-used-expired-in-+11': [`${PLUS11} - INTERVAL 1 HOUR`, `${PLUS11} - INTERVAL 30 MINUTE`, true],
    'tok-used-past-margin': [`${UTC} - INTERVAL 1 DAY`, `${UTC} - INTERVAL 13 HOUR`, false],
  };
  const ids = {}; // queue label -> row id

  async function seedQueue(label, [type, status, expires]) {
    const r = await fx.query(
      `INSERT INTO pending_queue (device_id, site_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES ('ozk-utc-mig', ?, ?, '00', 'sealed_envelope', ?, ${expires})`,
      [fx.siteId, type, status]
    );
    ids[label] = r.insertId;
  }

  before(async () => {
    await fx.start();
    fx.siteId = (await fx.query('SELECT id FROM sites LIMIT 1'))[0].id;
    for (const [label, row] of Object.entries(QUEUE)) await seedQueue(label, row);
    for (const [token, [used, expires]] of Object.entries(TOKENS)) {
      await fx.query(
        `INSERT INTO enroll_tokens (token, site_id, used_at, expires_at) VALUES (?, ?, ${used}, ${expires})`,
        [token, fx.siteId]
      );
    }
    await fx.query(
      `INSERT INTO locks (id, site_id, broker_username, last_seen_at, presence_at, last_mech_at)
       VALUES ('ozk-utc-mig', ?, 'ozk-utc-mig', ${PLUS11}, ${PLUS11}, ${PLUS11})`,
      [fx.siteId]
    );
    await fx.query(
      `INSERT INTO lock_bridges (lock_id, bridge_id, last_confirmed_reachable_at) VALUES ('ozk-utc-mig', 'ozb-utc-mig', ${PLUS11})`
    );
    await fx.query(
      `INSERT INTO bridges_presence (bridge_id, presence, presence_at) VALUES ('ozb-utc-mig', 'offline', ${PLUS11})`
    );
    // The first boot already ran the migration on the empty DB; undo the flag
    // so the restart sees this DB as pre-S1e-4.
    await fx.query("DELETE FROM schema_flags WHERE name = 's1e4_utc_datetimes'");
    await fx.restartServer();
  });

  after(async () => {
    await fx.stop();
  });

  const queueLeft = async () =>
    new Set((await fx.query('SELECT id FROM pending_queue')).map((r) => r.id));

  test('queue: unlocks and rows ≥12 h past expiry go; everything that may still be live stays', async () => {
    const left = await queueLeft();
    for (const [label, row] of Object.entries(QUEUE)) {
      assert.equal(left.has(ids[label]), row[3], `${label} should ${row[3] ? 'stay' : 'be gone'}`);
    }
  });

  test('enroll_tokens: unused ones and used ones past the margin go', async () => {
    const left = new Set((await fx.query('SELECT token FROM enroll_tokens')).map((r) => r.token));
    for (const [token, row] of Object.entries(TOKENS)) {
      assert.equal(left.has(token), row[2], `${token} should ${row[2] ? 'stay' : 'be gone'}`);
    }
  });

  test('presence fields are cleared; the rows themselves stay', async () => {
    const [lock] = await fx.query("SELECT last_seen_at, presence_at, last_mech_at FROM locks WHERE id = 'ozk-utc-mig'");
    assert.deepEqual(lock, { last_seen_at: null, presence_at: null, last_mech_at: null });
    const [lb] = await fx.query("SELECT last_confirmed_reachable_at AS t FROM lock_bridges WHERE lock_id = 'ozk-utc-mig'");
    assert.equal(lb.t, null);
    const [bp] = await fx.query("SELECT presence, presence_at FROM bridges_presence WHERE bridge_id = 'ozb-utc-mig'");
    assert.deepEqual(bp, { presence: 'offline', presence_at: null });
  });

  test('the migration logs its counts and sets its flag', async () => {
    assert.match(
      fx.log(),
      /S1e-4 UTC migration: deleted 3 queue row\(s\), 2 enroll token\(s\); cleared presence on 1 lock\(s\), 1 lock_bridges row\(s\), 1 bridge\(s\)/
    );
    const flags = await fx.query("SELECT name FROM schema_flags WHERE name = 's1e4_utc_datetimes'");
    assert.equal(flags.length, 1);
  });

  test('a second boot runs nothing: a new live unlock survives it', async () => {
    await seedQueue('unlock-after', ['unlock', 'queued', `${UTC} + INTERVAL 1 MINUTE`]);
    const before = await queueLeft();
    await fx.restartServer();
    assert.deepEqual(await queueLeft(), before);
  });
};
