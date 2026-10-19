'use strict';
/*
 * ozserver-core/util.js — Pure helpers with no core state: LAN address, S−1 fail-closed secrets
 * (ozkey-50 §8.2), MAC/device-id helpers, random secrets.
 *
 * Moved verbatim from ozlock/server/server.js in S0b (ozkey-50 §7); comments
 * keep their original references. Dependencies come from createCore() via
 * ./ctx (see index.js); other core modules are reached through late-bound
 * forwarders so load order never matters.
 */

const crypto = require('crypto');
const os = require('os');
const fs = require('fs');
const { ctx, lazy } = require('./ctx');
const logEvent = (...args) => ctx.logEvent(...args);

/** First non-internal IPv4 of this host. Profiles override it via env (OZLOCK_SERVER_IP, …). */
function detectLanIp() {
  for (const ifaces of Object.values(os.networkInterfaces())) {
    for (const iface of ifaces || []) {
      if (iface.family === 'IPv4' && !iface.internal) return iface.address;
    }
  }
  return '127.0.0.1';
}

/* ---------------------------------------------------------------------------
 * S−1 (ozkey-50 §8.2): fail-closed secrets. Values come from the process env,
 * then from an optional `.env` beside this file (gitignored, must be 0600),
 * never from the source. resolveSecret() runs at load so CONFIG can read the
 * values; enforceSecrets() is the first thing boot() does, before any DB,
 * MQTT or HTTP, and exits on any required secret that is missing. Problems
 * name the variable, never the value. Same block in all three servers until
 * S0 lifts it into the shared core.
 * ------------------------------------------------------------------------- */
const IS_PRODUCTION = process.env.NODE_ENV === 'production';
// The one placeholder list: empty, these literals, and anything 'lab-…'.
const SECRET_PLACEHOLDERS = ['changeme', 'change-me', 'change_me', '<set-me>'];
const SECRET_MIN_PROD_LEN = 32;
const secretProblems = [];

function isPlaceholderSecret(value) {
  const s = String(value || '').trim().toLowerCase();
  return !s || s.startsWith('lab-') || SECRET_PLACEHOLDERS.includes(s);
}

/** KEY=VALUE lines; never overrides a var already set in the env. */
function loadDotEnv(file) {
  if (!fs.existsSync(file)) return;
  if (fs.statSync(file).mode & 0o077) {
    secretProblems.push(`${file} is readable by group/others (chmod 600 it)`);
    return;
  }
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/);
    if (!m || process.env[m[1]] !== undefined) continue;
    process.env[m[1]] = m[2].replace(/^(['"])(.*)\1$/, '$2');
  }
}

/** First set var in `vars` (primary first, then legacy names). `required`:
 *  'always' | 'production' | false. Returns null when unset/placeholder. */
function resolveSecret(vars, { required = false, minProdLen = 0 } = {}) {
  const found = vars.find((v) => process.env[v] !== undefined);
  const value = found ? process.env[found] : undefined;
  const needed = required === 'always' || (required === 'production' && IS_PRODUCTION);
  if (isPlaceholderSecret(value)) {
    if (needed) secretProblems.push(`${vars[0]} is missing or a placeholder`);
    return null;
  }
  if (IS_PRODUCTION && value.length < minProdLen) {
    secretProblems.push(`${vars[0]} is shorter than ${minProdLen} characters`);
    return null;
  }
  return value;
}

function enforceSecrets(warnings = []) {
  const mode = IS_PRODUCTION ? 'production' : 'lab';
  if (secretProblems.length) {
    for (const p of secretProblems) logEvent('error', `FATAL S−1: ${p}`);
    logEvent('error', `FATAL S−1: refusing to start (${mode}); set these in the env or a 0600 .env beside server.js`);
    process.exit(1);
  }
  for (const w of warnings) logEvent('warn', `S−1: ${w}`);
  logEvent('info', `S−1: secrets OK (${mode})`);
}


// S4 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08: the server
// building a credential frame at all was the plaintext-storage breach the
// whitepaper named. credentialValueBytes()/buildCredentialFrame()/
// buildDeleteFrame() are gone — the app always sends a pre-sealed
// envelope_hex now (A1-A5, ftpos shipped), the server only relays it opaque.
// See migrations/S3_drop_raw_value.sql for the paired schema cut.

// S10 (ozkey-17 §4/§9, XF-72..78, XF-120 §2 step 2): buildUnlockFrame()
// removed 2026-08-21 — it built DP 1, an invented DP with no authentication
// (XF-120 §1.2b: on a lock whose profile happened to carry DP 1, ANY sender
// able to reach the command topic could open the door, no bond, no key
// check). Deletable once both REST-unlock callers ship sealing: `unlock()`
// (XF-120 §4/§8, bench-verified DP 76 opening a real lock) and
// `assistedUnlock()`, the one caller that fix missed (XF-120 §9, caught
// while scoping this exact deletion) — both landed `ftpos 34bc1e6`. This
// was "the last server-composed Tuya frame in the system" (ozkey-17 §4);
// deleting it makes `envelope_hex` unconditionally required below.

/* ---------------------------------------------------------------------------
 * Identity helpers
 * ------------------------------------------------------------------------- */
function normalizeMac(raw) {
  const hex = String(raw || '')
    .replace(/[^0-9a-fA-F]/g, '')
    .toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(':');
}

/** Lab interim device id (ozkey-04 §3: real hardware derives from a keypair). */
function deviceIdFromMac(mac) {
  return `ozk-${mac.replace(/:/g, '').toLowerCase()}`;
}

function makeSecret(bytes = 16, prefix = '') {
  return prefix + crypto.randomBytes(bytes).toString('hex');
}

module.exports = {
  detectLanIp,
  IS_PRODUCTION,
  SECRET_PLACEHOLDERS,
  SECRET_MIN_PROD_LEN,
  secretProblems,
  isPlaceholderSecret,
  loadDotEnv,
  resolveSecret,
  enforceSecrets,
  normalizeMac,
  deviceIdFromMac,
  makeSecret,
};
