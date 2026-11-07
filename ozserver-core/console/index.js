'use strict';
/*
 * ozserver-core/console: the operator console's auth core (C0a, dashboards
 * proposal §3, PM GO 2026-11-05). It is written once and mounted by each
 * product with its own fixed roles. Console users are staff (OZLOCK: our own
 * staff, PM Q3); lock owners never log in here, they use the BANOI app. App
 * JWTs and console sessions never mix.
 *
 *   mountConsole(router, { product, issuer, roles, config })  // config from policy.resolveConsoleConfig()
 *   initConsoleDatabase()                                     // after core.initDatabase()
 *   createAdminInvite(email, displayName)                     // the CLI's first-admin path
 *
 * What it guarantees:
 * - 2-step is mandatory: a session is 'full' only after TOTP (or a recovery code).
 * - No default credentials. Accounts start from an invite that expires in 30 min.
 * - Sessions are server-side rows: the cookie carries 256 random bits, and the
 *   DB holds only their SHA-256. 30 min idle, 12 h absolute; the id rotates
 *   at every stage change.
 * - CSRF: every state-changing call with a session needs x-csrf-token, plus
 *   SameSite=Strict.
 * - Step-up: routes marked stepUp need a TOTP code from the last 5 minutes.
 * - Every route declares its permission; routes() lists them so a test can
 *   assert none is missing.
 * - The audit_log gets an `actor` column; logins, failures and every
 *   state-changing call are recorded.
 */

const { ctx, lazy } = require('../ctx');
const pool = lazy(() => ctx.pool);
const CONFIG = lazy(() => ctx.CONFIG);
const logEvent = (...args) => ctx.logEvent(...args);
const C = require('./crypto');
const { passwordProblem } = require('./policy');

const IDLE_MS = 30 * 60 * 1000;
const ABSOLUTE_MS = 12 * 3600 * 1000;
const STEP_UP_MS = 5 * 60 * 1000;
const STEP_UP_TRIES = 3; // failed step-ups before the session ends
const INVITE_MS = 30 * 60 * 1000;
const LOCK_AFTER = 5; // failed logins before a lock
const LOCK_BASE_MS = 15 * 60 * 1000; // doubles with each further 5 failures
const IP_WINDOW_MS = 15 * 60 * 1000;
// Login-type attempts per IP per window. The per-account lock is the real
// guard; this one slows spraying across accounts. It's sized for staff behind
// one hotel NAT.
const IP_MAX = 100;
const LOCALES = ['en', 'vi', 'zh'];
const ROLE_RE = /^[a-z][a-z_]{1,19}$/;

let opts = null; // set by mountConsole()
const routeTable = []; // { method, path, perm, stage, stepUp }

/* -- schema ------------------------------------------------------------------ */
async function initConsoleDatabase() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS console_users (
      id INT AUTO_INCREMENT PRIMARY KEY,
      email VARCHAR(254) NOT NULL UNIQUE,
      display_name VARCHAR(120) NOT NULL,
      role VARCHAR(20) NOT NULL,
      status ENUM('invited','active','disabled') NOT NULL DEFAULT 'invited',
      password_hash VARCHAR(200) NULL,
      totp_secret_enc VARCHAR(200) NULL,
      totp_last_step BIGINT NULL,
      locale VARCHAR(5) NOT NULL DEFAULT 'en',
      tz VARCHAR(64) NOT NULL DEFAULT 'UTC',
      failed_logins INT NOT NULL DEFAULT 0,
      locked_until DATETIME NULL,
      last_login_at DATETIME NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);
  await pool.query(`
    CREATE TABLE IF NOT EXISTS console_sessions (
      id_hash CHAR(64) PRIMARY KEY,
      user_id INT NOT NULL,
      stage ENUM('password','enroll','full') NOT NULL,
      csrf VARCHAR(64) NOT NULL,
      totp_pending_enc VARCHAR(200) NULL,
      created_at DATETIME NOT NULL,
      last_seen_at DATETIME NOT NULL,
      stepped_up_at DATETIME NULL,
      ip VARCHAR(45) NULL,
      user_agent VARCHAR(200) NULL,
      stepup_fails TINYINT UNSIGNED NOT NULL DEFAULT 0,
      INDEX idx_user (user_id)
    ) ENGINE=InnoDB`);
  await pool.query(`
    CREATE TABLE IF NOT EXISTS console_recovery_codes (
      user_id INT NOT NULL,
      code_hash CHAR(64) NOT NULL,
      used_at DATETIME NULL,
      PRIMARY KEY (user_id, code_hash)
    ) ENGINE=InnoDB`);
  await pool.query(`
    CREATE TABLE IF NOT EXISTS console_invites (
      token_hash CHAR(64) PRIMARY KEY,
      user_id INT NOT NULL,
      expires_at DATETIME NOT NULL,
      used_at DATETIME NULL
    ) ENGINE=InnoDB`);
  // audit_log.actor is added by the core's initDatabase() (every profile).
}

/* -- audit ------------------------------------------------------------------- */
async function audit(actor, action, detail) {
  try {
    await pool.query('INSERT INTO audit_log (actor, site_id, action, detail) VALUES (?, ?, ?, ?)', [
      actor ? String(actor).slice(0, 120) : null,
      CONFIG.SITE_ID,
      String(action).slice(0, 30),
      String(detail || '').slice(0, 255),
    ]);
  } catch (err) {
    logEvent('warn', `console audit write failed (${action}): ${err.message}`);
  }
}

/* -- cookies ----------------------------------------------------------------- */
const cookieName = () => (opts.config.insecure ? 'oz_session' : '__Host-oz_session');

function readCookie(req, name) {
  for (const part of String(req.headers.cookie || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0 && part.slice(0, i).trim() === name) return part.slice(i + 1).trim();
  }
  return null;
}

function setSessionCookie(res, token) {
  const attrs = ['Path=/', 'HttpOnly', 'SameSite=Strict'];
  if (!opts.config.insecure) attrs.push('Secure');
  res.append('Set-Cookie', `${cookieName()}=${token}; ${attrs.join('; ')}`);
}

function clearSessionCookie(res) {
  const attrs = ['Path=/', 'HttpOnly', 'SameSite=Strict', 'Max-Age=0'];
  if (!opts.config.insecure) attrs.push('Secure');
  res.append('Set-Cookie', `${cookieName()}=; ${attrs.join('; ')}`);
}

/* -- sessions ---------------------------------------------------------------- */
async function createSession(req, res, userId, stage, extra = {}) {
  const token = C.randomToken(32);
  const csrf = C.randomToken(24);
  const now = new Date();
  await pool.query(
    `INSERT INTO console_sessions (id_hash, user_id, stage, csrf, totp_pending_enc, created_at, last_seen_at, stepped_up_at, ip, user_agent)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    [C.sha256(token), userId, stage, csrf, extra.totpPendingEnc || null, extra.createdAt || now, now, extra.steppedUpAt || null,
      clientIp(req), String(req.headers['user-agent'] || '').slice(0, 200)]
  );
  setSessionCookie(res, token);
  return { csrf, stage };
}

/** Rotate: a new id (and csrf) for a stage change. The absolute clock keeps
 *  running from the original login. */
async function rotateSession(req, res, sess, stage, extra = {}) {
  await pool.query('DELETE FROM console_sessions WHERE id_hash = ?', [sess.id_hash]);
  return createSession(req, res, sess.user_id, stage, { createdAt: sess.created_at, ...extra });
}

async function loadSession(req) {
  const token = readCookie(req, cookieName());
  if (!token || !/^[A-Za-z0-9_-]{43}$/.test(token)) return null;
  const idHash = C.sha256(token);
  const [[row]] = await pool.query(
    `SELECT s.*, u.email, u.display_name, u.role, u.status, u.locale, u.tz, u.totp_secret_enc, u.totp_last_step
       FROM console_sessions s JOIN console_users u ON u.id = s.user_id WHERE s.id_hash = ?`,
    [idHash]
  );
  if (!row) return null;
  const now = Date.now();
  const expired =
    row.status === 'disabled' ||
    now - new Date(row.last_seen_at).getTime() > IDLE_MS ||
    now - new Date(row.created_at).getTime() > ABSOLUTE_MS;
  if (expired) {
    await pool.query('DELETE FROM console_sessions WHERE id_hash = ?', [idHash]);
    return null;
  }
  await pool.query('UPDATE console_sessions SET last_seen_at = ? WHERE id_hash = ?', [new Date(), idHash]);
  return row;
}

/* -- rate limits ------------------------------------------------------------- */
const ipHits = new Map(); // ip -> [timestamps]
function clientIp(req) {
  return String(req.ip || req.socket.remoteAddress || '').slice(0, 45);
}
let ipSweepAt = 0;
function ipLimited(req) {
  const ip = clientIp(req);
  const now = Date.now();
  // Drop idle addresses once a window, so a spray from many IPs can't grow
  // the map without bound.
  if (now - ipSweepAt > IP_WINDOW_MS) {
    for (const [k, v] of ipHits) if (!v.length || now - v[v.length - 1] >= IP_WINDOW_MS) ipHits.delete(k);
    ipSweepAt = now;
  }
  const hits = (ipHits.get(ip) || []).filter((t) => now - t < IP_WINDOW_MS);
  hits.push(now);
  ipHits.set(ip, hits.length > IP_MAX + 1 ? hits.slice(-(IP_MAX + 1)) : hits);
  return hits.length > IP_MAX;
}

async function noteFailure(user) {
  const n = user.failed_logins + 1;
  const lockedUntil = n >= LOCK_AFTER && n % LOCK_AFTER === 0
    ? new Date(Date.now() + LOCK_BASE_MS * 2 ** (n / LOCK_AFTER - 1))
    : user.locked_until;
  await pool.query('UPDATE console_users SET failed_logins = ?, locked_until = ? WHERE id = ?', [n, lockedUntil || null, user.id]);
}
const isLocked = (user) => user.locked_until && new Date(user.locked_until).getTime() > Date.now();

/* -- permissions ------------------------------------------------------------- */
function can(role, perm) {
  const set = opts.roles[role];
  return !!set && (set.includes('*') || set.includes(perm));
}

/** Declare a console route. `perm`: 'public' (no session), 'session' (any
 *  stage; used by the 2-step steps themselves), 'self' (any role, full
 *  session: the user's own profile), or an operation name that requires a
 *  full session and a role holding it. */
function route(router, method, path, { perm, stage, stepUp = false }, handler) {
  if (!perm) throw new Error(`console route ${method} ${path} declares no permission`);
  routeTable.push({ method: method.toUpperCase(), path, perm, stage: stage || null, stepUp });
  router[method](path, async (req, res) => {
    try {
      if (perm !== 'public') {
        const sess = await loadSession(req);
        if (!sess) return res.status(401).json({ ok: false, code: 'not_signed_in' });
        const needStage = stage || 'full';
        if (sess.stage !== needStage) return res.status(403).json({ ok: false, code: 'wrong_stage', stage: sess.stage });
        if (req.method !== 'GET' && req.get('x-csrf-token') !== sess.csrf) {
          return res.status(403).json({ ok: false, code: 'csrf' });
        }
        if (perm !== 'session' && perm !== 'self' && !can(sess.role, perm)) {
          return res.status(403).json({ ok: false, code: 'forbidden' });
        }
        if (stepUp && !(sess.stepped_up_at && Date.now() - new Date(sess.stepped_up_at).getTime() <= STEP_UP_MS)) {
          return res.status(403).json({ ok: false, code: 'step_up_required' });
        }
        req.consoleSession = sess;
      }
      await handler(req, res);
    } catch (err) {
      logEvent('error', `console ${method.toUpperCase()} ${path}: ${err.message}`);
      if (!res.headersSent) res.status(500).json({ ok: false, code: 'internal' });
    }
  });
}

/* -- helpers ----------------------------------------------------------------- */
const totpKey = () => opts.config.totpKey;
const userTotpSecret = (u) => C.open(totpKey(), u.totp_secret_enc, `totp:${u.user_id || u.id}`);
const normEmail = (e) => String(e || '').trim().toLowerCase();
const EMAIL_RE = /^[^\s@]{1,64}@[^\s@]{1,190}\.[^\s@]{2,}$/;
const meView = (s) => ({
  email: s.email,
  display_name: s.display_name,
  role: s.role,
  locale: s.locale,
  tz: s.tz,
  permissions: opts.roles[s.role] || [],
});
const validTz = (tz) => {
  try {
    new Intl.DateTimeFormat('en', { timeZone: tz });
    return typeof tz === 'string' && tz.length <= 64;
  } catch (_) {
    return false;
  }
};

/** Consume a TOTP code for user `u` (row with id/user_id, totp_secret_enc,
 *  totp_last_step). Refuses a replayed step. */
async function consumeTotp(u, code) {
  const step = C.verifyTotp(userTotpSecret(u), code);
  if (step === null) return false;
  if (u.totp_last_step !== null && step <= Number(u.totp_last_step)) return false;
  const [r] = await pool.query(
    'UPDATE console_users SET totp_last_step = ? WHERE id = ? AND (totp_last_step IS NULL OR totp_last_step < ?)',
    [step, u.user_id || u.id, step]
  );
  return r.affectedRows === 1;
}

async function newInvite(userId) {
  const token = C.randomToken(32);
  await pool.query('INSERT INTO console_invites (token_hash, user_id, expires_at) VALUES (?, ?, ?)', [
    C.sha256(token),
    userId,
    new Date(Date.now() + INVITE_MS),
  ]);
  // The token rides in the fragment, so it never reaches a server log or a Referer.
  return `${opts.config.publicUrl}/enroll#token=${token}`;
}

async function activeAdminCount(exceptId = 0) {
  const adminRoles = Object.keys(opts.roles).filter((r) => opts.roles[r].includes('*'));
  const [[{ n }]] = await pool.query(
    "SELECT COUNT(*) AS n FROM console_users WHERE status = 'active' AND role IN (?) AND id <> ?",
    [adminRoles, exceptId]
  );
  return n;
}

/* -- routes ------------------------------------------------------------------ */
function mountConsole(router, o) {
  if (!o || !o.product || !o.issuer || !o.roles || !o.config) throw new Error('mountConsole: product, issuer, roles and config are required');
  if (!o.config.totpKey || !o.config.publicUrl) throw new Error('mountConsole: console config did not resolve (see FATAL S−1 above)');
  for (const [role, perms] of Object.entries(o.roles)) {
    if (!ROLE_RE.test(role) || !Array.isArray(perms)) throw new Error(`mountConsole: bad role ${role}`);
  }
  if (!Object.values(o.roles).some((p) => p.includes('*'))) throw new Error('mountConsole: one role must hold "*"');
  opts = o;

  // Nothing under the console is cacheable, framable or sniffable. HSTS too,
  // unless this is the http-only lab opt-in.
  router.use((req, res, next) => {
    res.set({
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
      'Referrer-Policy': 'no-referrer',
      'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'",
    });
    if (!o.config.insecure) res.set('Strict-Transport-Security', 'max-age=31536000; includeSubDomains');
    next();
  });

  route(router, 'post', '/auth/invite/accept', { perm: 'public' }, async (req, res) => {
    if (ipLimited(req)) return res.status(429).json({ ok: false, code: 'rate_limited' });
    const { token, password } = req.body || {};
    const [[inv]] = await pool.query(
      `SELECT i.token_hash, i.user_id, u.email, u.status FROM console_invites i JOIN console_users u ON u.id = i.user_id
        WHERE i.token_hash = ? AND i.used_at IS NULL AND i.expires_at > ?`,
      [C.sha256(String(token || '')), new Date()]
    );
    if (!inv || inv.status === 'disabled') return res.status(400).json({ ok: false, code: 'invite_invalid' });
    const problem = passwordProblem(password, { email: inv.email });
    if (problem) return res.status(400).json({ ok: false, code: problem });
    await pool.query('UPDATE console_invites SET used_at = ? WHERE token_hash = ?', [new Date(), inv.token_hash]);
    await pool.query(
      "UPDATE console_users SET password_hash = ?, totp_secret_enc = NULL, totp_last_step = NULL, status = 'invited', failed_logins = 0, locked_until = NULL WHERE id = ?",
      [await C.hashPassword(password), inv.user_id]
    );
    await pool.query('DELETE FROM console_sessions WHERE user_id = ?', [inv.user_id]);
    await pool.query('DELETE FROM console_recovery_codes WHERE user_id = ?', [inv.user_id]);
    await audit(inv.email, 'console_invite_accept', 'password set; 2-step enrolment next');
    res.json({ ok: true, ...(await createSession(req, res, inv.user_id, 'enroll')) });
  });

  route(router, 'post', '/auth/login', { perm: 'public' }, async (req, res) => {
    if (ipLimited(req)) return res.status(429).json({ ok: false, code: 'rate_limited' });
    const email = normEmail((req.body || {}).email);
    const password = String((req.body || {}).password || '');
    const [[u]] = await pool.query('SELECT * FROM console_users WHERE email = ?', [email]);
    const fail = async (why) => {
      await audit(email || null, 'console_login_fail', why);
      res.status(401).json({ ok: false, code: 'login_failed' }); // one generic answer for every cause
    };
    if (!u || !u.password_hash || u.status === 'disabled') {
      await C.burnPasswordTime(password);
      return fail(u ? `status ${u.status}` : 'unknown email');
    }
    if (isLocked(u)) {
      await C.burnPasswordTime(password);
      return fail('locked');
    }
    if (!(await C.verifyPassword(password, u.password_hash))) {
      await noteFailure(u);
      return fail('bad password');
    }
    const stage = u.totp_secret_enc ? 'password' : 'enroll';
    res.json({ ok: true, ...(await createSession(req, res, u.id, stage)) });
  });

  route(router, 'get', '/auth/enroll', { perm: 'session', stage: 'enroll' }, async (req, res) => {
    const s = req.consoleSession;
    const secret = C.newTotpSecret();
    const sealed = C.seal(totpKey(), secret, `totp:${s.user_id}`);
    await pool.query('UPDATE console_sessions SET totp_pending_enc = ? WHERE id_hash = ?', [sealed, s.id_hash]);
    res.json({
      ok: true,
      otpauth_uri: C.otpauthUri({ issuer: opts.issuer, account: s.email, secret }),
      secret: C.base32Encode(secret),
    });
  });

  route(router, 'post', '/auth/enroll', { perm: 'session', stage: 'enroll' }, async (req, res) => {
    const s = req.consoleSession;
    if (!s.totp_pending_enc) return res.status(400).json({ ok: false, code: 'enroll_not_started' });
    const secret = C.open(totpKey(), s.totp_pending_enc, `totp:${s.user_id}`);
    const step = C.verifyTotp(secret, (req.body || {}).code);
    if (step === null) return res.status(400).json({ ok: false, code: 'totp_invalid' });
    const codes = C.newRecoveryCodes();
    await pool.query(
      "UPDATE console_users SET totp_secret_enc = ?, totp_last_step = ?, status = 'active', failed_logins = 0, locked_until = NULL, last_login_at = ? WHERE id = ?",
      [s.totp_pending_enc, step, new Date(), s.user_id]
    );
    await pool.query('DELETE FROM console_recovery_codes WHERE user_id = ?', [s.user_id]);
    await pool.query('INSERT INTO console_recovery_codes (user_id, code_hash) VALUES ?', [
      codes.map((c) => [s.user_id, C.sha256(C.normalizeRecoveryCode(c))]),
    ]);
    await audit(s.email, 'console_2step_enrol', 'TOTP enrolled, 10 recovery codes issued');
    const out = await rotateSession(req, res, s, 'full', { steppedUpAt: new Date() });
    res.json({ ok: true, ...out, recovery_codes: codes, me: meView(s) });
  });

  route(router, 'post', '/auth/totp', { perm: 'session', stage: 'password' }, async (req, res) => {
    if (ipLimited(req)) return res.status(429).json({ ok: false, code: 'rate_limited' });
    const s = req.consoleSession;
    const [[u]] = await pool.query('SELECT * FROM console_users WHERE id = ?', [s.user_id]);
    if (isLocked(u)) return res.status(401).json({ ok: false, code: 'login_failed' });
    if (!(await consumeTotp(u, (req.body || {}).code))) {
      await noteFailure(u);
      await audit(s.email, 'console_login_fail', 'bad 2-step code');
      return res.status(401).json({ ok: false, code: 'totp_invalid' });
    }
    await pool.query('UPDATE console_users SET failed_logins = 0, locked_until = NULL, last_login_at = ? WHERE id = ?', [new Date(), u.id]);
    await audit(s.email, 'console_login', 'TOTP');
    const out = await rotateSession(req, res, s, 'full', { steppedUpAt: new Date() });
    res.json({ ok: true, ...out, me: meView(s) });
  });

  route(router, 'post', '/auth/recovery', { perm: 'session', stage: 'password' }, async (req, res) => {
    if (ipLimited(req)) return res.status(429).json({ ok: false, code: 'rate_limited' });
    const s = req.consoleSession;
    const [[u]] = await pool.query('SELECT * FROM console_users WHERE id = ?', [s.user_id]);
    if (isLocked(u)) return res.status(401).json({ ok: false, code: 'login_failed' });
    const [r] = await pool.query(
      'UPDATE console_recovery_codes SET used_at = ? WHERE user_id = ? AND code_hash = ? AND used_at IS NULL',
      [new Date(), s.user_id, C.sha256(C.normalizeRecoveryCode((req.body || {}).code))]
    );
    if (r.affectedRows !== 1) {
      await noteFailure(u);
      await audit(s.email, 'console_login_fail', 'bad recovery code');
      return res.status(401).json({ ok: false, code: 'recovery_invalid' });
    }
    const [[{ n_left: left }]] = await pool.query(
      'SELECT COUNT(*) AS n_left FROM console_recovery_codes WHERE user_id = ? AND used_at IS NULL',
      [s.user_id]
    );
    await pool.query('UPDATE console_users SET failed_logins = 0, locked_until = NULL, last_login_at = ? WHERE id = ?', [new Date(), u.id]);
    await audit(s.email, 'console_login', `recovery code (${left} left)`);
    // A recovery login is not a step-up: sensitive actions still need TOTP.
    const out = await rotateSession(req, res, s, 'full');
    res.json({ ok: true, ...out, recovery_codes_left: left, me: meView(s) });
  });

  route(router, 'post', '/auth/step-up', { perm: 'session', stage: 'full' }, async (req, res) => {
    if (ipLimited(req)) return res.status(429).json({ ok: false, code: 'rate_limited' });
    const s = req.consoleSession;
    if (!(await consumeTotp(s, (req.body || {}).code))) {
      // Step-up guards against a stolen session, so a session can't guess
      // codes: after STEP_UP_TRIES misses it ends, and every miss counts
      // toward the account lock.
      const [[u]] = await pool.query('SELECT * FROM console_users WHERE id = ?', [s.user_id]);
      await noteFailure(u);
      const [r] = await pool.query(
        'UPDATE console_sessions SET stepup_fails = stepup_fails + 1 WHERE id_hash = ?',
        [s.id_hash]
      );
      const [[row]] = await pool.query('SELECT stepup_fails FROM console_sessions WHERE id_hash = ?', [s.id_hash]);
      await audit(s.email, 'console_stepup_fail', 'bad 2-step code');
      if (r.affectedRows && row && row.stepup_fails >= STEP_UP_TRIES) {
        await pool.query('DELETE FROM console_sessions WHERE id_hash = ?', [s.id_hash]);
        clearSessionCookie(res);
        await audit(s.email, 'console_session_end', `${STEP_UP_TRIES} failed step-ups`);
        return res.status(401).json({ ok: false, code: 'session_ended' });
      }
      return res.status(401).json({ ok: false, code: 'totp_invalid' });
    }
    await pool.query('UPDATE console_sessions SET stepped_up_at = ?, stepup_fails = 0 WHERE id_hash = ?', [new Date(), s.id_hash]);
    res.json({ ok: true, valid_s: STEP_UP_MS / 1000 });
  });

  route(router, 'post', '/auth/logout', { perm: 'session', stage: 'full' }, async (req, res) => {
    await pool.query('DELETE FROM console_sessions WHERE id_hash = ?', [req.consoleSession.id_hash]);
    clearSessionCookie(res);
    res.json({ ok: true });
  });

  route(router, 'get', '/me', { perm: 'self' }, async (req, res) => {
    res.json({ ok: true, me: meView(req.consoleSession), csrf: req.consoleSession.csrf });
  });

  route(router, 'patch', '/me', { perm: 'self' }, async (req, res) => {
    const { locale, tz, display_name } = req.body || {};
    const set = {};
    if (locale !== undefined) {
      if (!LOCALES.includes(locale)) return res.status(400).json({ ok: false, code: 'locale_invalid' });
      set.locale = locale;
    }
    if (tz !== undefined) {
      if (!validTz(tz)) return res.status(400).json({ ok: false, code: 'tz_invalid' });
      set.tz = tz;
    }
    if (display_name !== undefined) {
      const n = String(display_name).trim();
      if (!n || n.length > 120) return res.status(400).json({ ok: false, code: 'name_invalid' });
      set.display_name = n;
    }
    if (Object.keys(set).length) await pool.query('UPDATE console_users SET ? WHERE id = ?', [set, req.consoleSession.user_id]);
    res.json({ ok: true, me: meView({ ...req.consoleSession, ...set }) });
  });

  route(router, 'get', '/users', { perm: 'users.manage' }, async (req, res) => {
    const [rows] = await pool.query(
      `SELECT id, email, display_name, role, status, last_login_at, created_at,
              (locked_until IS NOT NULL AND locked_until > ?) AS locked
         FROM console_users ORDER BY email`,
      [new Date()]
    );
    res.json({ ok: true, users: rows.map((r) => ({ ...r, locked: !!r.locked })) });
  });

  route(router, 'post', '/users', { perm: 'users.manage', stepUp: true }, async (req, res) => {
    const email = normEmail((req.body || {}).email);
    const name = String((req.body || {}).display_name || '').trim();
    const role = (req.body || {}).role;
    if (!EMAIL_RE.test(email)) return res.status(400).json({ ok: false, code: 'email_invalid' });
    if (!name || name.length > 120) return res.status(400).json({ ok: false, code: 'name_invalid' });
    if (!opts.roles[role]) return res.status(400).json({ ok: false, code: 'role_invalid' });
    const [[exists]] = await pool.query('SELECT id FROM console_users WHERE email = ?', [email]);
    if (exists) return res.status(409).json({ ok: false, code: 'email_taken' });
    const [r] = await pool.query('INSERT INTO console_users (email, display_name, role) VALUES (?, ?, ?)', [email, name, role]);
    const link = await newInvite(r.insertId);
    await audit(req.consoleSession.email, 'console_user_add', `${email} as ${role}`);
    res.status(201).json({ ok: true, id: r.insertId, invite_link: link, invite_expires_s: INVITE_MS / 1000 });
  });

  route(router, 'patch', '/users/:id', { perm: 'users.manage', stepUp: true }, async (req, res) => {
    const id = Number(req.params.id);
    const [[u]] = await pool.query('SELECT * FROM console_users WHERE id = ?', [id]);
    if (!u) return res.status(404).json({ ok: false, code: 'not_found' });
    const { role, status } = req.body || {};
    const set = {};
    if (role !== undefined) {
      if (!opts.roles[role]) return res.status(400).json({ ok: false, code: 'role_invalid' });
      set.role = role;
    }
    if (status !== undefined) {
      if (!['active', 'disabled'].includes(status)) return res.status(400).json({ ok: false, code: 'status_invalid' });
      if (status === 'active' && !u.totp_secret_enc) return res.status(400).json({ ok: false, code: 'not_enrolled' });
      set.status = status;
    }
    const losesAdmin = (set.role && !opts.roles[set.role].includes('*')) || set.status === 'disabled';
    if (losesAdmin && opts.roles[u.role].includes('*') && u.status === 'active' && (await activeAdminCount(id)) === 0) {
      return res.status(409).json({ ok: false, code: 'last_admin' });
    }
    if (Object.keys(set).length) await pool.query('UPDATE console_users SET ? WHERE id = ?', [set, id]);
    if (set.status === 'disabled' || set.role) await pool.query('DELETE FROM console_sessions WHERE user_id = ?', [id]);
    await audit(req.consoleSession.email, 'console_user_edit', `${u.email} ${JSON.stringify(set)}`);
    res.json({ ok: true });
  });

  route(router, 'post', '/users/:id/reinvite', { perm: 'users.manage', stepUp: true }, async (req, res) => {
    // Lost authenticator AND recovery codes: an admin re-invites, which resets
    // the password and the 2-step on acceptance. There's no other path.
    const id = Number(req.params.id);
    const [[u]] = await pool.query('SELECT id, email, status FROM console_users WHERE id = ?', [id]);
    if (!u) return res.status(404).json({ ok: false, code: 'not_found' });
    if (u.status === 'disabled') return res.status(409).json({ ok: false, code: 'user_disabled' });
    if (id === req.consoleSession.user_id) return res.status(409).json({ ok: false, code: 'not_self' });
    const link = await newInvite(id);
    await audit(req.consoleSession.email, 'console_user_reinvite', u.email);
    res.json({ ok: true, invite_link: link, invite_expires_s: INVITE_MS / 1000 });
  });

  route(router, 'post', '/users/:id/end-sessions', { perm: 'users.manage', stepUp: true }, async (req, res) => {
    const id = Number(req.params.id);
    const [r] = await pool.query('DELETE FROM console_sessions WHERE user_id = ?', [id]);
    await audit(req.consoleSession.email, 'console_end_sessions', `user ${id}: ${r.affectedRows}`);
    res.json({ ok: true, ended: r.affectedRows });
  });

  // Unknown console paths get a JSON 404, not the app's HTML.
  router.use((req, res) => res.status(404).json({ ok: false, code: 'not_found' }));
  return router;
}

/** First admin (CLI). Refuses if any active admin exists, so the CLI can't
 *  be a back door into a running console. */
async function createAdminInvite(email, displayName) {
  if (!opts) throw new Error('createAdminInvite: mountConsole() first');
  const e = normEmail(email);
  if (!EMAIL_RE.test(e)) throw new Error('email is not valid');
  if ((await activeAdminCount()) > 0) throw new Error('an active admin already exists; add users from the console');
  const adminRole = Object.keys(opts.roles).find((r) => opts.roles[r].includes('*'));
  let [[u]] = await pool.query('SELECT id FROM console_users WHERE email = ?', [e]);
  if (!u) {
    const [r] = await pool.query('INSERT INTO console_users (email, display_name, role) VALUES (?, ?, ?)', [
      e,
      String(displayName || e).slice(0, 120),
      adminRole,
    ]);
    u = { id: r.insertId };
  } else {
    await pool.query('UPDATE console_users SET role = ? WHERE id = ?', [adminRole, u.id]);
  }
  const link = await newInvite(u.id);
  await audit('cli', 'console_admin_invite', e);
  return link;
}

const routes = () => routeTable.slice();

module.exports = { mountConsole, initConsoleDatabase, createAdminInvite, routes };
