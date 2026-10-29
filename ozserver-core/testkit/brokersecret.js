'use strict';
/*
 * S1e-1 (ozkey-50 §8.2): the boot migration that hashes plaintext broker
 * secrets, tested the only honest way: seed rows in the old shape, restart the
 * server on the same DB, read the rows back. This is the step that rewrites a
 * real DB irreversibly (S1e-2 on the ozlock lab DB), so it gets its own suite.
 *
 * A profile's test/brokersecret.test.js calls
 *   require('<core>/testkit/brokersecret')(harness, { hashLocks })
 * where hashLocks is that profile's CONFIG.HASH_LOCK_BROKER_SECRETS.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const { hashBrokerSecret, verifyBrokerSecret } = require('../util');

const PBKDF2_RE = /^PBKDF2\$sha512\$100000\$[A-Za-z0-9+/]{22}==\$[A-Za-z0-9+/]{86}==$/;

module.exports = function brokerSecretMigrationSuite(harness, { hashLocks }) {
  const fx = new harness.Fixture();

  const PLAIN = 'ozl_00112233445566778899aabbccddeeff';
  const KEPT = 'ozl_ffeeddccbbaa99887766554433221100';
  let kept; // hashed in before()
  const scryptSalt = crypto.randomBytes(16);
  const legacyScrypt = `scrypt$${scryptSalt.toString('hex')}$${crypto
    .scryptSync(KEPT, scryptSalt, 32)
    .toString('hex')}`;

  before(async () => {
    kept = await hashBrokerSecret(KEPT);
    await fx.start();
    const [{ id: site }] = await fx.query('SELECT id FROM sites LIMIT 1');
    await fx.query('INSERT INTO bridges (id, broker_username, broker_secret) VALUES ?', [
      [
        ['ozb-mig-plain', 'ozb-mig-plain', PLAIN],
        ['ozb-mig-hashed', 'ozb-mig-hashed', kept],
        ['ozb-mig-none', 'ozb-mig-none', null],
      ],
    ]);
    // The lock column is still VARCHAR(64) where the profile doesn't hash
    // (ozlock), so the legacy scrypt row is only seeded where it fits.
    const locks = [
      ['ozk-mig-plain', site, 'ozk-mig-plain', PLAIN],
      ['ozk-mig-none', site, 'ozk-mig-none', null],
    ];
    if (hashLocks) locks.push(['ozk-mig-scrypt', site, 'ozk-mig-scrypt', legacyScrypt]);
    await fx.query('INSERT INTO locks (id, site_id, broker_username, broker_secret) VALUES ?', [locks]);
    await fx.restartServer();
  });

  after(async () => {
    await fx.stop();
  });

  const secretOf = async (table, id) =>
    (await fx.query(`SELECT broker_secret FROM ${table} WHERE id = ?`, [id]))[0].broker_secret;

  test('boot hashes plaintext bridge secrets; hashed and empty rows are left alone', async () => {
    const plain = await secretOf('bridges', 'ozb-mig-plain');
    assert.match(plain, PBKDF2_RE);
    assert.ok(verifyBrokerSecret(PLAIN, plain), 'the hash is of the old plaintext');
    assert.equal(await secretOf('bridges', 'ozb-mig-hashed'), kept);
    assert.equal(await secretOf('bridges', 'ozb-mig-none'), null);
    assert.match(fx.log(), /hashed 1 plaintext bridges\.broker_secret row\(s\)/);
  });

  if (hashLocks) {
    test('boot hashes plaintext lock secrets; a legacy scrypt row still verifies', async () => {
      const plain = await secretOf('locks', 'ozk-mig-plain');
      assert.match(plain, PBKDF2_RE);
      assert.ok(verifyBrokerSecret(PLAIN, plain));
      assert.equal(await secretOf('locks', 'ozk-mig-scrypt'), legacyScrypt);
      assert.ok(verifyBrokerSecret(KEPT, legacyScrypt));
      assert.equal(await secretOf('locks', 'ozk-mig-none'), null);
    });
  } else {
    test('HASH_LOCK_BROKER_SECRETS off: lock secrets are left as they were', async () => {
      assert.equal(await secretOf('locks', 'ozk-mig-plain'), PLAIN);
      assert.doesNotMatch(fx.log(), /plaintext locks\.broker_secret/);
    });
  }

  test('a second boot changes nothing (idempotent)', async () => {
    const before = await fx.query('SELECT id, broker_secret FROM bridges UNION ALL SELECT id, broker_secret FROM locks');
    await fx.restartServer();
    const again = await fx.query('SELECT id, broker_secret FROM bridges UNION ALL SELECT id, broker_secret FROM locks');
    assert.deepEqual(again, before);
  });
};
