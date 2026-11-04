'use strict';
/*
 * C0a: the operator console's auth on the ozlock profile, end to end against
 * the real server.js: the CLI first-admin invite → password → TOTP
 * enrolment → recovery codes → full session, then login with 2-step, replay,
 * recovery codes, step-up, CSRF, roles, lockout, the last-admin guard and
 * session timeouts. The browser is played by fetch with a per-user cookie jar.
 */

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const harness = require('./harness');
const C = require('../../../ozserver-core/console/crypto');

const fx = new harness.Fixture();
let origin;

class Browser {
  constructor() {
    this.cookie = null;
    this.csrf = null;
  }
  async call(method, path, body, { csrf = true } = {}) {
    const headers = { 'content-type': 'application/json' };
    if (this.cookie) headers.cookie = this.cookie;
    if (csrf && this.csrf && method !== 'GET') headers['x-csrf-token'] = this.csrf;
    const res = await fetch(`${origin}/console/v1${path}`, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    for (const c of res.headers.getSetCookie()) {
      const [pair] = c.split(';');
      this.cookie = /=$/.test(pair) ? null : pair;
      this.lastSetCookie = c;
    }
    const json = await res.json().catch(() => null);
    if (json && json.csrf) this.csrf = json.csrf;
    return { status: res.status, body: json, headers: res.headers };
  }
}

const tokenOf = (link) => new URL(link).hash.replace(/^#token=/, '');
const code = (secretB32, offsetSteps = 0) => C.hotp(C.base32Decode(secretB32), C.totpStep() + offsetSteps);
const ADMIN = { email: 'ops-admin@example.com', password: 'harbour-signal-orchard-71' };
const VIEWER = { email: 'support-viewer@example.com', password: 'copper-meadow-lantern-38' };
const state = {};

before(async () => {
  await fx.start();
  origin = `http://127.0.0.1:${fx.httpPort}`;
});

after(async () => {
  await fx.stop();
});

test('fail-closed: every console route answers 401 without a session (and never as HTML)', async () => {
  const anon = new Browser();
  for (const [m, p] of [
    ['GET', '/me'],
    ['PATCH', '/me'],
    ['GET', '/users'],
    ['POST', '/users'],
    ['GET', '/auth/enroll'],
    ['POST', '/auth/totp'],
    ['POST', '/auth/step-up'],
    ['POST', '/auth/logout'],
  ]) {
    const r = await anon.call(m, p, m === 'GET' ? undefined : {});
    assert.equal(r.status, 401, `${m} ${p}`);
    assert.equal(r.body.code, 'not_signed_in');
    assert.equal(r.headers.get('cache-control'), 'no-store');
    assert.equal(r.headers.get('x-content-type-options'), 'nosniff');
  }
  const missing = await anon.call('GET', '/nope');
  assert.equal(missing.status, 404);
  assert.equal(missing.body.code, 'not_found');
});

test('the console is never served cross-origin (mounted before cors)', async () => {
  const res = await fetch(`${origin}/console/v1/me`, { headers: { origin: 'https://evil.example' } });
  assert.equal(res.headers.get('access-control-allow-origin'), null);
});

test('CLI: console:create-admin prints a one-time enrolment link', async () => {
  const r = await fx.runCli(['console:create-admin', ADMIN.email, 'Ops', 'Admin']);
  assert.equal(r.code, 0, r.out);
  const m = r.out.match(/CONSOLE ADMIN INVITE (\S+)/);
  assert.ok(m, r.out);
  assert.match(m[1], new RegExp(`^${origin.replace(/[.]/g, '\\.')}/enroll#token=[A-Za-z0-9_-]{43}$`));
  state.adminInvite = m[1];
});

test('invite: weak passwords are refused with a reason code; a good one gives an enrol-stage session', async () => {
  const b = new Browser();
  const token = tokenOf(state.adminInvite);
  for (const [pw, why] of [
    ['short-pw', 'password_too_short'],
    ['password1234', 'password_common'],
    ['ops-admin-forever', 'password_contains_email'],
  ]) {
    const r = await b.call('POST', '/auth/invite/accept', { token, password: pw });
    assert.equal(r.status, 400);
    assert.equal(r.body.code, why);
  }
  const bad = await b.call('POST', '/auth/invite/accept', { token: 'x'.repeat(43), password: ADMIN.password });
  assert.equal(bad.body.code, 'invite_invalid');
  const ok = await b.call('POST', '/auth/invite/accept', { token, password: ADMIN.password });
  assert.equal(ok.status, 200);
  assert.equal(ok.body.stage, 'enroll');
  assert.match(b.lastSetCookie, /^oz_session=[A-Za-z0-9_-]{43}; Path=\/; HttpOnly; SameSite=Strict$/, 'the lab opt-in drops Secure + __Host-');
  const again = await new Browser().call('POST', '/auth/invite/accept', { token, password: ADMIN.password });
  assert.equal(again.body.code, 'invite_invalid', 'an invite works once');
  state.adminEnroll = b;
});

test('enrol stage: nothing but enrolment works until 2-step is set up', async () => {
  const b = state.adminEnroll;
  const me = await b.call('GET', '/me');
  assert.equal(me.status, 403);
  assert.equal(me.body.code, 'wrong_stage');
  const e = await b.call('GET', '/auth/enroll');
  assert.equal(e.status, 200);
  assert.match(e.body.otpauth_uri, /^otpauth:\/\/totp\/OZLOCK%3Aops-admin%40example\.com\?/);
  assert.equal(new URL(e.body.otpauth_uri).searchParams.get('secret'), e.body.secret);
  const wrong = await b.call('POST', '/auth/enroll', { code: '000000' === code(e.body.secret) ? '111111' : '000000' });
  assert.equal(wrong.body.code, 'totp_invalid');
  const noCsrf = await b.call('POST', '/auth/enroll', { code: code(e.body.secret) }, { csrf: false });
  assert.equal(noCsrf.body.code, 'csrf');
  const ok = await b.call('POST', '/auth/enroll', { code: code(e.body.secret) });
  assert.equal(ok.status, 200);
  assert.equal(ok.body.stage, 'full');
  assert.equal(ok.body.recovery_codes.length, 10);
  assert.equal(ok.body.me.role, 'admin');
  state.adminSecret = e.body.secret;
  state.adminRecovery = ok.body.recovery_codes;
  state.admin = b;
});

test('the secret is stored sealed, never in the clear; tokens are stored hashed', async () => {
  const [u] = await fx.query('SELECT totp_secret_enc, password_hash FROM console_users WHERE email = ?', [ADMIN.email]);
  assert.match(u.totp_secret_enc, /^v1\./);
  assert.ok(!u.totp_secret_enc.includes(state.adminSecret));
  assert.match(u.password_hash, /^scrypt\$/);
  const sessions = await fx.query('SELECT id_hash FROM console_sessions');
  const cookieToken = state.admin.cookie.split('=')[1];
  assert.ok(sessions.every((s) => s.id_hash !== cookieToken && /^[0-9a-f]{64}$/.test(s.id_hash)));
});

test('full session: /me, profile settings (locale/tz validated), CSRF on writes', async () => {
  const b = state.admin;
  const me = await b.call('GET', '/me');
  assert.equal(me.status, 200);
  assert.deepEqual(me.body.me.permissions, ['*']);
  assert.equal((await b.call('PATCH', '/me', { locale: 'vi', tz: 'Asia/Ho_Chi_Minh' })).body.me.locale, 'vi');
  assert.equal((await b.call('PATCH', '/me', { locale: 'fr' })).body.code, 'locale_invalid');
  assert.equal((await b.call('PATCH', '/me', { tz: 'Mars/Olympus' })).body.code, 'tz_invalid');
  assert.equal((await b.call('PATCH', '/me', { locale: 'zh' }, { csrf: false })).body.code, 'csrf');
});

test('admin adds a viewer (step-up is fresh from enrolment); the viewer enrols and is read-only', async () => {
  const add = await state.admin.call('POST', '/users', { email: VIEWER.email, display_name: 'Support', role: 'viewer' });
  assert.equal(add.status, 201, JSON.stringify(add.body));
  assert.equal((await state.admin.call('POST', '/users', { email: VIEWER.email, display_name: 'x', role: 'viewer' })).body.code, 'email_taken');
  assert.equal((await state.admin.call('POST', '/users', { email: 'a@b.co', display_name: 'x', role: 'root' })).body.code, 'role_invalid');
  state.viewerId = add.body.id;

  const v = new Browser();
  await v.call('POST', '/auth/invite/accept', { token: tokenOf(add.body.invite_link), password: VIEWER.password });
  const e = await v.call('GET', '/auth/enroll');
  const ok = await v.call('POST', '/auth/enroll', { code: code(e.body.secret) });
  assert.equal(ok.body.me.role, 'viewer');
  assert.equal((await v.call('GET', '/users')).body.code, 'forbidden');
  assert.equal((await v.call('GET', '/me')).status, 200, 'self needs no role permission');
  state.viewer = v;
});

test('the last active admin cannot be demoted or disabled', async () => {
  const [{ id }] = await fx.query('SELECT id FROM console_users WHERE email = ?', [ADMIN.email]);
  state.adminId = id;
  assert.equal((await state.admin.call('PATCH', `/users/${id}`, { role: 'viewer' })).body.code, 'last_admin');
  assert.equal((await state.admin.call('PATCH', `/users/${id}`, { status: 'disabled' })).body.code, 'last_admin');
});

test('CLI refuses a second first-admin once an active admin exists', async () => {
  const r = await fx.runCli(['console:create-admin', 'someone-else@example.com']);
  assert.equal(r.code, 1);
  assert.match(r.out, /an active admin already exists/);
});

test('login: password then TOTP; a replayed code is refused; a recovery code works once and is no step-up', async () => {
  await state.admin.call('POST', '/auth/logout');
  assert.equal((await state.admin.call('GET', '/me')).status, 401);

  const b = new Browser();
  const pw = await b.call('POST', '/auth/login', { email: ADMIN.email.toUpperCase(), password: ADMIN.password });
  assert.equal(pw.body.stage, 'password');
  assert.equal((await b.call('GET', '/me')).body.code, 'wrong_stage');
  // Codes for exact steps, read from the server's own record, so a 30 s
  // boundary between enrolment and here can't change what's tested.
  const [{ totp_last_step: used }] = await fx.query('SELECT totp_last_step FROM console_users WHERE email = ?', [ADMIN.email]);
  const stepCode = (s) => C.hotp(C.base32Decode(state.adminSecret), s);
  const replay = await b.call('POST', '/auth/totp', { code: stepCode(Number(used)) });
  assert.equal(replay.body.code, 'totp_invalid', 'the enrolment step cannot be reused');
  const next = stepCode(Number(used) + 1);
  const full = await b.call('POST', '/auth/totp', { code: next });
  assert.equal(full.body.stage, 'full', JSON.stringify(full.body));
  await b.call('POST', '/auth/logout');

  const b2 = new Browser();
  await b2.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  assert.equal((await b2.call('POST', '/auth/totp', { code: next })).body.code, 'totp_invalid', 'same code twice');
  const rec = await b2.call('POST', '/auth/recovery', { code: state.adminRecovery[0].toUpperCase() });
  assert.equal(rec.body.stage, 'full');
  assert.equal(rec.body.recovery_codes_left, 9);
  assert.equal(
    (await b2.call('POST', '/users', { email: 'x@example.com', display_name: 'x', role: 'viewer' })).body.code,
    'step_up_required',
    'a recovery login does not count as a fresh 2-step'
  );
  await b2.call('POST', '/auth/logout');

  const b3 = new Browser();
  await b3.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  assert.equal((await b3.call('POST', '/auth/recovery', { code: state.adminRecovery[0] })).body.code, 'recovery_invalid', 'a code works once');
  // A wrong recovery code counted as a failure; reset the counter for the lockout test below.
  await fx.query('UPDATE console_users SET failed_logins = 0 WHERE email = ?', [ADMIN.email]);
});

test('lockout: 5 wrong passwords lock the account; the right one then gets the same generic refusal', async () => {
  const b = new Browser();
  for (let i = 0; i < 5; i++) {
    const r = await b.call('POST', '/auth/login', { email: VIEWER.email, password: `wrong-password-${i}-xx` });
    assert.equal(r.status, 401);
    assert.equal(r.body.code, 'login_failed');
  }
  const locked = await b.call('POST', '/auth/login', { email: VIEWER.email, password: VIEWER.password });
  assert.equal(locked.status, 401);
  assert.equal(locked.body.code, 'login_failed');
  const unknown = await b.call('POST', '/auth/login', { email: 'nobody@example.com', password: VIEWER.password });
  assert.equal(unknown.body.code, 'login_failed', 'an unknown email reads the same');
  const [u] = await fx.query('SELECT failed_logins, locked_until FROM console_users WHERE email = ?', [VIEWER.email]);
  assert.equal(u.failed_logins, 5);
  assert.ok(new Date(u.locked_until).getTime() > Date.now() + 14 * 60 * 1000, 'locked for ~15 min');
});

test('sessions expire: 30 min idle and 12 h absolute, measured server-side', async () => {
  const idle = new Browser();
  await idle.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  await idle.call('POST', '/auth/recovery', { code: state.adminRecovery[1] });
  assert.equal((await idle.call('GET', '/me')).status, 200);
  const [s] = await fx.query('SELECT id_hash FROM console_sessions ORDER BY last_seen_at DESC LIMIT 1');
  await fx.query('UPDATE console_sessions SET last_seen_at = UTC_TIMESTAMP() - INTERVAL 31 MINUTE WHERE id_hash = ?', [s.id_hash]);
  assert.equal((await idle.call('GET', '/me')).status, 401);

  const abs = new Browser();
  await abs.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  await abs.call('POST', '/auth/recovery', { code: state.adminRecovery[2] });
  const [s2] = await fx.query('SELECT id_hash FROM console_sessions ORDER BY last_seen_at DESC LIMIT 1');
  await fx.query('UPDATE console_sessions SET created_at = UTC_TIMESTAMP() - INTERVAL 13 HOUR WHERE id_hash = ?', [s2.id_hash]);
  assert.equal((await abs.call('GET', '/me')).status, 401);
});

test('step-up: 3 wrong codes end the session, and each counts toward the account lock', async () => {
  const b = new Browser();
  await b.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  await b.call('POST', '/auth/recovery', { code: state.adminRecovery[4] });
  const [{ failed_logins: before }] = await fx.query('SELECT failed_logins FROM console_users WHERE email = ?', [ADMIN.email]);
  const wrong = C.hotp(C.base32Decode(state.adminSecret), 1); // a step from 1970: never valid now
  assert.equal((await b.call('POST', '/auth/step-up', { code: wrong })).body.code, 'totp_invalid');
  assert.equal((await b.call('POST', '/auth/step-up', { code: wrong })).body.code, 'totp_invalid');
  assert.equal((await b.call('POST', '/auth/step-up', { code: wrong })).body.code, 'session_ended');
  assert.equal((await b.call('GET', '/me')).status, 401);
  const [{ failed_logins: afterN }] = await fx.query('SELECT failed_logins FROM console_users WHERE email = ?', [ADMIN.email]);
  assert.equal(afterN - before, 3);
  await fx.query('UPDATE console_users SET failed_logins = 0, locked_until = NULL WHERE email = ?', [ADMIN.email]);
});

test('disabling a user ends their sessions at once', async () => {
  // A fresh admin session with a step-up: login + TOTP two steps ahead isn't
  // accepted (±1 window), so step up via the DB clock instead.
  const b = new Browser();
  await b.call('POST', '/auth/login', { email: ADMIN.email, password: ADMIN.password });
  await b.call('POST', '/auth/recovery', { code: state.adminRecovery[3] });
  const [s] = await fx.query('SELECT id_hash FROM console_sessions ORDER BY last_seen_at DESC LIMIT 1');
  await fx.query('UPDATE console_sessions SET stepped_up_at = UTC_TIMESTAMP() WHERE id_hash = ?', [s.id_hash]);
  assert.equal((await state.viewer.call('GET', '/me')).status, 200);
  const r = await b.call('PATCH', `/users/${state.viewerId}`, { status: 'disabled' });
  assert.equal(r.status, 200, JSON.stringify(r.body));
  assert.equal((await state.viewer.call('GET', '/me')).status, 401);
});

test('every console action is in audit_log with the staff member as actor', async () => {
  const rows = await fx.query("SELECT actor, action FROM audit_log WHERE action LIKE 'console%' ORDER BY id");
  const actions = new Set(rows.map((r) => r.action));
  for (const a of ['console_admin_invite', 'console_invite_accept', 'console_2step_enrol', 'console_user_add',
    'console_login', 'console_login_fail', 'console_user_edit']) {
    assert.ok(actions.has(a), `missing ${a}`);
  }
  assert.ok(rows.filter((r) => r.action === 'console_user_add').every((r) => r.actor === ADMIN.email));
});

test('per-IP limit: login-type calls from one address are cut off with 429', async () => {
  const b = new Browser();
  let status = 0;
  for (let i = 0; i < 120 && status !== 429; i++) {
    status = (await b.call('POST', '/auth/login', { email: `spray-${i}@example.com`, password: 'not-the-password-1' })).status;
  }
  assert.equal(status, 429);
});
