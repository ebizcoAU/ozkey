'use strict';
/*
 * ozserver-core/audit.js — The control-plane audit trail (audit_log) and its 90-day purge (XF-125 P1).
 *
 * Moved verbatim from ozlock/server/server.js in S0b (ozkey-50 §7); comments
 * keep their original references. Dependencies come from createCore() via
 * ./ctx (see index.js); other core modules are reached through late-bound
 * forwarders so load order never matters.
 */

const { ctx, lazy } = require('./ctx');
const CONFIG = lazy(() => ctx.CONFIG);
const pool = lazy(() => ctx.pool);
const logEvent = (...args) => ctx.logEvent(...args);

/**
 * Record a control-plane action in the app-attributed audit trail. If appId is
 * omitted it's resolved from the device's current pairing. Best-effort — never
 * throws into the caller (an audit failure must not fail the real action).
 */
async function recordAudit(appId, deviceId, action, detail) {
  try {
    let aid = appId;
    if (!aid && deviceId) {
      const [[l]] = await pool.query('SELECT app_id FROM locks WHERE id = ?', [deviceId]);
      aid = l ? l.app_id : null;
    }
    await pool.query(
      'INSERT INTO audit_log (app_id, device_id, site_id, action, detail) VALUES (?, ?, ?, ?, ?)',
      [aid || null, deviceId || null, CONFIG.SITE_ID, action, String(detail || '').slice(0, 255)]
    );
  } catch (err) {
    logEvent('warn', `audit_log write failed (${action}): ${err.message}`);
  }
}

// XF-125 P1: audit_log ("security events", per the file header) was
// documented as retained per the Sovereign Edge whitepaper's 90-day target
// since 2026-07-31 — but nothing ever enforced it. Grepped clean before
// writing this: no cron, no age-based DELETE, anywhere in the prior code.
// `pending_queue` is deliberately NOT touched here — its rows already have
// their own per-command `expires_at` governing whether they still FIRE, and
// an `expired`/`sent` row is cheap, small, and useful for `GET /queue`
// debugging; only `audit_log` is named in the whitepaper's retained-data
// inventory, so only it gets a purge.
async function purgeOldAuditLog() {
  try {
    const [result] = await pool.query(
      'DELETE FROM audit_log WHERE created_at < (NOW() - INTERVAL ? SECOND)',
      [CONFIG.AUDIT_LOG_RETENTION_MS / 1000]
    );
    if (result.affectedRows > 0)
      logEvent('info', `XF-125 P1: purged ${result.affectedRows} audit_log row(s) past 90d retention`);
  } catch (err) {
    logEvent('error', `audit_log purge failed: ${err.message}`);
  }
}

module.exports = {
  recordAudit,
  purgeOldAuditLog,
};
