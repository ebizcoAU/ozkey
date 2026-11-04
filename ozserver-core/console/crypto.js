'use strict';
/*
 * ozserver-core/console/crypto.js: the console's crypto primitives (C0a).
 * Node's built-in crypto only, no dependencies, so offline and Docker builds
 * stay simple.
 *
 *   - TOTP (RFC 6238 over RFC 4226 HOTP, HMAC-SHA1, 30 s, 6 digits), base32 secrets
 *   - seal/open: AES-256-GCM for TOTP secrets at rest (CONSOLE_TOTP_KEY)
 *   - human passwords: scrypt (kept separate from the PBKDF2 broker-secret
 *     helper in util.js, which is shaped for mosquitto dynsec, not people)
 *   - random tokens, and SHA-256 for tokens we only need to look up
 */

const crypto = require('crypto');
const { promisify } = require('util');
const scryptAsync = promisify(crypto.scrypt);

/* -- base32 (RFC 4648, no padding): the authenticator-app secret format ---- */
const B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';

function base32Encode(buf) {
  let bits = 0;
  let value = 0;
  let out = '';
  for (const byte of buf) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out += B32[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += B32[(value << (5 - bits)) & 31];
  return out;
}

function base32Decode(str) {
  const clean = String(str).toUpperCase().replace(/[\s=-]/g, '');
  let bits = 0;
  let value = 0;
  const out = [];
  for (const ch of clean) {
    const i = B32.indexOf(ch);
    if (i < 0) throw new Error('invalid base32');
    value = (value << 5) | i;
    bits += 5;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 255);
      bits -= 8;
    }
  }
  return Buffer.from(out);
}

/* -- HOTP/TOTP --------------------------------------------------------------- */
const TOTP_STEP_S = 30;
const TOTP_DIGITS = 6;

function hotp(secret, counter, digits = TOTP_DIGITS, algo = 'sha1') {
  const msg = Buffer.alloc(8);
  msg.writeBigUInt64BE(BigInt(counter));
  const mac = crypto.createHmac(algo, secret).update(msg).digest();
  const off = mac[mac.length - 1] & 0x0f;
  const bin = ((mac[off] & 0x7f) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | mac[off + 3];
  return String(bin % 10 ** digits).padStart(digits, '0');
}

const totpStep = (nowMs = Date.now()) => Math.floor(nowMs / 1000 / TOTP_STEP_S);

/** The step a `code` matches within ±`window` steps of now, or null. The
 *  comparison is constant-time per candidate. The caller refuses a step <=
 *  the last one used (replay). */
function verifyTotp(secret, code, { nowMs = Date.now(), window = 1 } = {}) {
  const c = String(code || '').replace(/\s/g, '');
  if (!/^\d{6}$/.test(c)) return null;
  const now = totpStep(nowMs);
  let matched = null;
  for (let s = now - window; s <= now + window; s++) {
    const want = Buffer.from(hotp(secret, s));
    if (crypto.timingSafeEqual(want, Buffer.from(c)) && matched === null) matched = s;
  }
  return matched;
}

function newTotpSecret() {
  return crypto.randomBytes(20); // 160 bits, as RFC 4226 recommends
}

/** otpauth:// URI for the enrolment QR code. */
function otpauthUri({ issuer, account, secret }) {
  const label = encodeURIComponent(`${issuer}:${account}`);
  const q = new URLSearchParams({
    secret: base32Encode(secret),
    issuer,
    algorithm: 'SHA1',
    digits: String(TOTP_DIGITS),
    period: String(TOTP_STEP_S),
  });
  return `otpauth://totp/${label}?${q}`;
}

/* -- seal/open: AES-256-GCM, "v1.<iv>.<tag>.<ct>" base64url ---------------- */
function seal(key, plain, aad = '') {
  const iv = crypto.randomBytes(12);
  const c = crypto.createCipheriv('aes-256-gcm', key, iv);
  c.setAAD(Buffer.from(aad));
  const ct = Buffer.concat([c.update(plain), c.final()]);
  return ['v1', iv, c.getAuthTag(), ct].map((p) => (typeof p === 'string' ? p : p.toString('base64url'))).join('.');
}

/** Throws on any tampering, a wrong key or a wrong aad. */
function open(key, sealed, aad = '') {
  const [v, iv, tag, ct] = String(sealed).split('.');
  if (v !== 'v1' || !iv || !tag || ct === undefined) throw new Error('sealed value: bad format');
  const d = crypto.createDecipheriv('aes-256-gcm', key, Buffer.from(iv, 'base64url'));
  d.setAAD(Buffer.from(aad));
  d.setAuthTag(Buffer.from(tag, 'base64url'));
  return Buffer.concat([d.update(Buffer.from(ct, 'base64url')), d.final()]);
}

/* -- human passwords: scrypt$<logN>$<r>$<p>$<salt b64>$<key b64> ----------- */
const SCRYPT = { logN: 15, r: 8, p: 1, keylen: 64 };
const scryptOpts = (logN, r, p) => ({ N: 2 ** logN, r, p, maxmem: 256 * 2 ** logN * r });

async function hashPassword(password) {
  const salt = crypto.randomBytes(16);
  const { logN, r, p, keylen } = SCRYPT;
  const key = await scryptAsync(String(password).normalize('NFKC'), salt, keylen, scryptOpts(logN, r, p));
  return `scrypt$${logN}$${r}$${p}$${salt.toString('base64')}$${key.toString('base64')}`;
}

async function verifyPassword(password, stored) {
  const parts = String(stored || '').split('$');
  if (parts.length !== 6 || parts[0] !== 'scrypt') return false;
  const [, logN, r, p, saltB64, keyB64] = parts;
  const want = Buffer.from(keyB64, 'base64');
  const got = await scryptAsync(String(password).normalize('NFKC'), Buffer.from(saltB64, 'base64'), want.length, scryptOpts(+logN, +r, +p));
  return crypto.timingSafeEqual(want, got);
}

// A real hash of a random password, so a login for an unknown email costs
// the same scrypt as a known one (no account-existence timing oracle).
let dummyHash = null;
async function burnPasswordTime(password) {
  if (!dummyHash) dummyHash = await hashPassword(crypto.randomBytes(16).toString('hex'));
  await verifyPassword(password, dummyHash);
  return false;
}

/* -- tokens ------------------------------------------------------------------ */
const randomToken = (bytes = 32) => crypto.randomBytes(bytes).toString('base64url');
const sha256 = (s) => crypto.createHash('sha256').update(String(s)).digest('hex');

/** 10 recovery codes as xxxxx-xxxxx (base32, 50 bits each). */
function newRecoveryCodes(n = 10) {
  return Array.from({ length: n }, () => {
    const s = base32Encode(crypto.randomBytes(7)).slice(0, 10).toLowerCase();
    return `${s.slice(0, 5)}-${s.slice(5)}`;
  });
}
const normalizeRecoveryCode = (c) => String(c || '').toLowerCase().replace(/[^a-z2-7]/g, '');

module.exports = {
  base32Encode,
  base32Decode,
  hotp,
  totpStep,
  verifyTotp,
  newTotpSecret,
  otpauthUri,
  TOTP_STEP_S,
  seal,
  open,
  hashPassword,
  verifyPassword,
  burnPasswordTime,
  randomToken,
  sha256,
  newRecoveryCodes,
  normalizeRecoveryCode,
};
