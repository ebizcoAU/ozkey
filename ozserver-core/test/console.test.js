'use strict';
/*
 * C0a: the console's crypto and policy primitives, against published
 * vectors where they exist. No DB, broker or server is involved.
 */

const { test } = require('node:test');
const assert = require('node:assert/strict');
const C = require('../console/crypto');
const { passwordProblem, PASSWORD_MIN } = require('../console/policy');

const RFC_SECRET = Buffer.from('12345678901234567890'); // RFC 4226 App. D / RFC 6238 App. B (SHA-1)

test('HOTP matches RFC 4226 Appendix D', () => {
  const want = ['755224', '287082', '359152', '969429', '338314', '254676', '287922', '162583', '399871', '520489'];
  want.forEach((code, counter) => assert.equal(C.hotp(RFC_SECRET, counter), code, `counter ${counter}`));
});

test('TOTP matches RFC 6238 Appendix B (SHA-1, 8 digits)', () => {
  const rows = [
    [59, '94287082'],
    [1111111109, '07081804'],
    [1111111111, '14050471'],
    [1234567890, '89005924'],
    [2000000000, '69279037'],
    [20000000000, '65353130'],
  ];
  for (const [t, code] of rows) assert.equal(C.hotp(RFC_SECRET, C.totpStep(t * 1000), 8), code, `T=${t}`);
});

test('verifyTotp: accepts ±1 step, refuses ±2, and returns the matched step', () => {
  const now = 1_700_000_000_000;
  const step = C.totpStep(now);
  for (const d of [-1, 0, 1]) assert.equal(C.verifyTotp(RFC_SECRET, C.hotp(RFC_SECRET, step + d), { nowMs: now }), step + d);
  for (const d of [-2, 2]) assert.equal(C.verifyTotp(RFC_SECRET, C.hotp(RFC_SECRET, step + d), { nowMs: now }), null);
  for (const bad of ['', '12345', '1234567', 'abcdef', null]) assert.equal(C.verifyTotp(RFC_SECRET, bad, { nowMs: now }), null);
});

test('base32 matches RFC 4648 §10 and round-trips', () => {
  const vectors = { '': '', f: 'MY', fo: 'MZXQ', foo: 'MZXW6', foob: 'MZXW6YQ', fooba: 'MZXW6YTB', foobar: 'MZXW6YTBOI' };
  for (const [plain, enc] of Object.entries(vectors)) {
    assert.equal(C.base32Encode(Buffer.from(plain)), enc);
    assert.equal(C.base32Decode(enc).toString(), plain);
  }
  const s = C.newTotpSecret();
  assert.deepEqual(C.base32Decode(C.base32Encode(s)), s);
  assert.throws(() => C.base32Decode('MZ1W'), /invalid base32/);
});

test('otpauth URI carries the issuer, account and secret', () => {
  const uri = C.otpauthUri({ issuer: 'OZLOCK', account: 'ops@example.com', secret: Buffer.from('foobar') });
  assert.match(uri, /^otpauth:\/\/totp\/OZLOCK%3Aops%40example\.com\?/);
  const q = new URL(uri).searchParams;
  assert.equal(q.get('secret'), 'MZXW6YTBOI');
  assert.equal(q.get('issuer'), 'OZLOCK');
  assert.equal(q.get('digits'), '6');
  assert.equal(q.get('period'), '30');
});

test('seal/open round-trips; a wrong key, a wrong aad or any tampering is refused', () => {
  const key = Buffer.alloc(32, 7);
  const sealed = C.seal(key, Buffer.from('secret'), 'totp:1');
  assert.equal(C.open(key, sealed, 'totp:1').toString(), 'secret');
  assert.notEqual(C.seal(key, Buffer.from('secret'), 'totp:1'), sealed, 'a fresh IV every time');
  assert.throws(() => C.open(Buffer.alloc(32, 8), sealed, 'totp:1'));
  assert.throws(() => C.open(key, sealed, 'totp:2'), undefined, 'aad binds the row');
  const parts = sealed.split('.');
  const ct = Buffer.from(parts[3], 'base64url');
  ct[0] ^= 1;
  assert.throws(() => C.open(key, [parts[0], parts[1], parts[2], ct.toString('base64url')].join('.'), 'totp:1'));
  assert.throws(() => C.open(key, 'v2.a.b.c'), /bad format/);
});

test('scrypt password hash verifies, refuses a wrong password, and salts each hash', async () => {
  const h = await C.hashPassword('correct horse battery');
  assert.match(h, /^scrypt\$15\$8\$1\$[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+$/);
  assert.equal(await C.verifyPassword('correct horse battery', h), true);
  assert.equal(await C.verifyPassword('correct horse batterY', h), false);
  assert.notEqual(await C.hashPassword('correct horse battery'), h);
  assert.equal(await C.verifyPassword('x', 'PBKDF2$sha512$1$a$b'), false, 'a foreign format never verifies');
});

test('recovery codes: 10, distinct, xxxxx-xxxxx, normalised for lookup', () => {
  const codes = C.newRecoveryCodes();
  assert.equal(codes.length, 10);
  assert.equal(new Set(codes).size, 10);
  for (const c of codes) assert.match(c, /^[a-z2-7]{5}-[a-z2-7]{5}$/);
  assert.equal(C.normalizeRecoveryCode(' ABCDE-fghij '), 'abcdefghij');
});

test('password policy: length, the denylist, repeats, and the email local part', () => {
  assert.equal(passwordProblem('a'.repeat(PASSWORD_MIN - 1)), 'password_too_short');
  assert.equal(passwordProblem('x'.repeat(129)), 'password_too_long');
  assert.equal(passwordProblem('Password1234'), 'password_common', 'the denylist is case-insensitive');
  assert.equal(passwordProblem('zzzzzzzzzzzzzz'), 'password_common');
  assert.equal(passwordProblem('vince.phan-2026!', { email: 'vince.phan@example.com' }), 'password_contains_email');
  assert.equal(passwordProblem('tram-river-lantern-42'), null);
});
