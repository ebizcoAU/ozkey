'use strict';
/*
 * OZLOCK-AUTH-1 (CN-LAB-1 R1): a required caller key on a product's REST API.
 *
 *   <PREFIX>_API_KEYS = "name:key,name:key"   one key per client, so one can
 *                                             be revoked without the others
 *   <PREFIX>_API_OPEN = 1                     lab only: serve without a key
 *
 * Fail-closed: resolveApiKeys() runs at load and adds to util.secretProblems,
 * so enforceSecrets() refuses the boot before any DB, MQTT or HTTP when the
 * keys are missing or malformed. Problems name the variable and the client,
 * never a key.
 *
 * The middleware checks `Authorization: Bearer <key>` before any route runs
 * (so before any DB access). Keys are compared as SHA-256 digests with
 * timingSafeEqual against EVERY configured key, so neither the length nor
 * the position of a match shows in the timing.
 */

const crypto = require('crypto');
const { IS_PRODUCTION, secretProblems, isPlaceholderSecret } = require('./util');

const KEY_MIN_LEN = 32;
const NAME_RE = /^[a-z0-9][a-z0-9_-]{0,31}$/;
const digest = (s) => crypto.createHash('sha256').update(String(s), 'utf8').digest();

/** Read and validate the product's key list. Returns
 *  { open: boolean, clients: [{ name, digest }] }. */
function resolveApiKeys(prefix, env = process.env) {
  const keysVar = `${prefix}_API_KEYS`;
  const openVar = `${prefix}_API_OPEN`;
  const rawKeys = String(env[keysVar] || '').trim();
  const open = env[openVar] === '1';
  const problems = [];
  const clients = [];

  if (open && IS_PRODUCTION) problems.push(`${openVar}=1 is not allowed in production`);
  if (open && rawKeys) problems.push(`${openVar}=1 and ${keysVar} are both set; choose one`);
  if (!open && !rawKeys) {
    problems.push(`${keysVar} is missing (name:key,name:key); for a lab instance only, ${openVar}=1 serves without a key`);
  }

  if (rawKeys) {
    const seen = new Map(); // digest hex -> name
    rawKeys.split(',').forEach((entry, i) => {
      const at = entry.indexOf(':');
      if (at < 0) return problems.push(`${keysVar}: entry ${i + 1} is not name:key`);
      const name = entry.slice(0, at).trim();
      const key = entry.slice(at + 1).trim();
      if (!NAME_RE.test(name)) return problems.push(`${keysVar}: entry ${i + 1} has an invalid client name (a-z 0-9 _ -, max 32)`);
      if (clients.some((c) => c.name === name)) return problems.push(`${keysVar}: client name "${name}" appears twice`);
      if (isPlaceholderSecret(key)) return problems.push(`${keysVar}: client "${name}" key is a placeholder`);
      if (key.length < KEY_MIN_LEN) return problems.push(`${keysVar}: client "${name}" key is shorter than ${KEY_MIN_LEN} characters`);
      const d = digest(key);
      const hex = d.toString('hex');
      if (seen.has(hex)) return problems.push(`${keysVar}: clients "${seen.get(hex)}" and "${name}" share a key`);
      seen.set(hex, name);
      clients.push({ name, digest: d });
    });
  }

  secretProblems.push(...problems);
  return { open: open && !problems.length, clients: problems.length ? [] : clients, keysVar, openVar };
}

/** The configured client whose key this is, or null. Compares against all. */
function matchClient(clients, presented) {
  const d = digest(presented);
  let found = null;
  for (const c of clients) if (crypto.timingSafeEqual(c.digest, d) && !found) found = c.name;
  return found;
}

/** Express middleware. `realm` names the product in WWW-Authenticate. */
function requireApiKey(cfg, { realm, logEvent }) {
  return (req, res, next) => {
    if (cfg.open) return next();
    const h = req.get('authorization') || '';
    const m = h.match(/^Bearer\s+(\S+)\s*$/i);
    let code = 'api_key_required';
    if (m) {
      const name = matchClient(cfg.clients, m[1]);
      if (name) {
        req.apiClient = name;
        return next();
      }
      code = 'api_key_invalid';
    }
    logEvent('warn', `OZLOCK-AUTH-1: refused ${req.method} ${req.baseUrl}${req.path} from ${req.ip}: ${code}`);
    res.set('WWW-Authenticate', `Bearer realm="${realm}"`);
    res.status(401).json({
      ok: false,
      code,
      error: code === 'api_key_required' ? 'Authorization: Bearer <api key> is required' : 'API key not recognised',
    });
  };
}

/** One boot line: how the API is protected (client names only). */
function describe(cfg) {
  if (cfg.open) return `${cfg.openVar}=1: the HTTP API is OPEN, no caller key (lab only, refused in production)`;
  return `API keys loaded for ${cfg.clients.length} client(s): ${cfg.clients.map((c) => c.name).join(', ')}`;
}

module.exports = { resolveApiKeys, requireApiKey, describe, KEY_MIN_LEN };
