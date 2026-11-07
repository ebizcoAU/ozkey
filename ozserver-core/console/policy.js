'use strict';
/*
 * ozserver-core/console/policy.js: password rules and the fail-closed console
 * config (C0a).
 */

const fs = require('fs');
const path = require('path');
const util = require('../util');

/* -- password policy --------------------------------------------------------- */
const PASSWORD_MIN = 12;
const PASSWORD_MAX = 128;

// One lowercase password per line. The bundled list is short; a deployment
// can point CONSOLE_PASSWORD_DENYLIST at a fuller one (e.g. a top-100k list).
let denylist = null;
function loadDenylist() {
  if (denylist) return denylist;
  const files = [path.join(__dirname, 'common-passwords.txt'), process.env.CONSOLE_PASSWORD_DENYLIST].filter(Boolean);
  denylist = new Set();
  for (const f of files) {
    for (const line of fs.readFileSync(f, 'utf8').split(/\r?\n/)) {
      const w = line.trim().toLowerCase();
      if (w && !w.startsWith('#')) denylist.add(w);
    }
  }
  return denylist;
}

/** null if acceptable, else a language-neutral reason code (the UI translates it). */
function passwordProblem(password, { email = '' } = {}) {
  const pw = String(password || '');
  if (pw.length < PASSWORD_MIN) return 'password_too_short';
  if (pw.length > PASSWORD_MAX) return 'password_too_long';
  const lower = pw.toLowerCase();
  if (loadDenylist().has(lower)) return 'password_common';
  if (/^(.)\1+$/.test(pw)) return 'password_common';
  const local = String(email).split('@')[0].toLowerCase();
  if (local.length >= 4 && lower.includes(local)) return 'password_contains_email';
  return null;
}

/* -- fail-closed console config ---------------------------------------------- */
/** Run at load, like resolveSecret(), so enforceSecrets() at boot refuses
 *  to start on any problem. `prefix` is the profile's env prefix ('OZLOCK').
 *  - <P>_CONSOLE_TOTP_KEY: 64 hex chars (32 bytes), always required.
 *  - <P>_PUBLIC_URL: required. Must be https:// unless OZ_LAB_INSECURE=1, and
 *    that opt-in is refused in production. */
function resolveConsoleConfig(prefix) {
  const keyVar = `${prefix}_CONSOLE_TOTP_KEY`;
  const urlVar = `${prefix}_PUBLIC_URL`;
  const keyHex = util.resolveSecret([keyVar], { required: 'always' });
  let totpKey = null;
  if (keyHex !== null) {
    if (/^[0-9a-fA-F]{64}$/.test(keyHex)) totpKey = Buffer.from(keyHex, 'hex');
    else util.secretProblems.push(`${keyVar} must be 64 hex characters (32 bytes)`);
  }

  const insecure = process.env.OZ_LAB_INSECURE === '1';
  if (insecure && util.IS_PRODUCTION) util.secretProblems.push('OZ_LAB_INSECURE=1 is refused in production');
  const rawUrl = process.env[urlVar];
  let publicUrl = null;
  if (!rawUrl) {
    util.secretProblems.push(`${urlVar} is missing`);
  } else {
    try {
      const u = new URL(rawUrl);
      if (u.protocol === 'https:' || (insecure && !util.IS_PRODUCTION && u.protocol === 'http:')) {
        publicUrl = u.origin;
      } else {
        util.secretProblems.push(`${urlVar} must be https:// (http only with OZ_LAB_INSECURE=1, never in production)`);
      }
    } catch (_) {
      util.secretProblems.push(`${urlVar} is not a URL`);
    }
  }
  return { totpKey, publicUrl, insecure: insecure && !util.IS_PRODUCTION };
}

module.exports = { passwordProblem, resolveConsoleConfig, PASSWORD_MIN, PASSWORD_MAX };
