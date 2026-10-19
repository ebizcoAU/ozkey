'use strict';
/*
 * ozserver-core/relay.js — v1 relay: the OZKIE v2 fail-fast gate, and the lock->app uplink
 * channel (outcome codes, caps/reset reports, sealed-envelope metadata).
 *
 * Moved verbatim from ozlock/server/server.js in S0b (ozkey-50 §7); comments
 * keep their original references. Dependencies come from createCore() via
 * ./ctx (see index.js); other core modules are reached through late-bound
 * forwarders so load order never matters.
 */

const { ctx, lazy } = require('./ctx');
const pool = lazy(() => ctx.pool);
const logEvent = (...args) => ctx.logEvent(...args);

// From other core modules.
const recordAudit = (...args) => require('./audit').recordAudit(...args);
const deviceIdentity = (...args) => require('./registry').deviceIdentity(...args);

// ozkey-31 §2: deviceId -> the most recently PUBLISHED, not-yet-confirmed
// grant-key command for that device. The uplink's plaintext outcome code
// carries no grant_id to correlate against directly, but the lock processes
// credential commands synchronously and one at a time (ozAwaitMcuAck blocks
// before any reply), so "this device's most recent unconfirmed grant" is
// what a fresh outcome code is answering, not a guess dressed up as one.
const lastGrantSentByDevice = new Map(); // deviceId -> { grantId, queueId }

/* -- OZKIE v2 gate (ozkey-48, operator-approved 2026-10-18) -------------------
 * Every relay route below carries v1 sealed envelopes. A v2 (AK1037) lock
 * speaks EDHOC + OSCORE instead, and its relay path (pass OSCORE bytes
 * through, route `4.01 + Echo` back to the sender, ozkey-48 §7) is not built
 * yet — no v2 lock or simulator exists to build it against. Until it is,
 * refuse cleanly instead of queueing bytes the lock cannot read.
 */
const OZKIE_V2 = 2;

/** Sends the refusal and returns true when `lock` is OZKIE v2; false otherwise. */
function refuseIfOzkieV2(res, lock, deviceId, verb) {
  if (!lock || lock.ozkie_version !== OZKIE_V2) return false;
  logEvent('warn', `${verb} refused for ${deviceId} — OZKIE v2 relay not built yet (ozkey-48 §7)`);
  res.status(501).json({
    ok: false,
    code: 'ozkie_v2_relay_not_built',
    error: 'ozkie_v2_relay_not_built',
    detail:
      'This lock speaks OZKIE v2 (EDHOC + OSCORE). The server cannot relay v2 messages yet, ' +
      'and a v1 envelope would mean nothing to it. Use a direct BLE session at the door.',
    device_id: deviceId,
    ref: 'ozkey-48 §7',
  });
  return true;
}

/** XF-138 P5 relay channel (2026-08-31): a CH584M lock's physical/local
 * factory reset has no other delivery path — no Thread, no MQTT of its own —
 * so it rides the bridge's relay of the lock's `member` characteristic onto
 * this same uplink topic, as a plaintext `{"type":"reset",...}` message
 * rather than the usual sealed `envelope_hex` wrapper (a reset lock has no
 * bond left to seal anything with anyway, same reasoning as the plaintext
 * `code` field elsewhere on this pipeline). Deliberately NOT
 * `DELETE /locks/:id`'s `purgeLockRows()` — this must keep the row and its
 * history (including this very event), just clear ownership so the lock is
 * re-commissionable. Returns true if the payload was a reset message
 * (handled either way, including "unknown device"), false otherwise, so the
 * caller knows whether to fall through to the normal sealed-uplink path.
 */
async function handleUplinkReset(deviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return false;
  }
  if (obj.type !== 'reset') return false;

  const [[lock]] = await pool.query('SELECT app_id, bridge_id FROM locks WHERE id = ?', [
    deviceId,
  ]);
  if (!lock) {
    logEvent('warn', `Reset notification from unknown lock ${deviceId} — ignored`);
    return true;
  }
  await pool.query(
    "UPDATE locks SET app_id = NULL, bridge_id = NULL, status = 'registered' WHERE id = ?",
    [deviceId]
  );
  logEvent(
    'key',
    `Lock ${deviceId} reported factory reset via relay channel — ownership cleared ` +
      `(was app=${lock.app_id || '(none)'}, bridge=${lock.bridge_id || '(none)'})`
  );
  await recordAudit(
    lock.app_id,
    deviceId,
    'reset',
    'device-reported factory reset via relay channel — ownership cleared'
  );
  return true;
}

/** XF-138/139 P5 (2026-08-31, PM decision): the durable fix for
 * `caps_source` staying permanently `'inferred'` on a CH584M lock —
 * `publishEnroll()`'s full identity/capability document (the same shape
 * `handleEnroll()` already parses via `deviceIdentity()`), relayed by the
 * bridge over the reused `member` characteristic onto this same uplink
 * topic, since a CH584M lock has no MQTT session of its own to publish it
 * on directly.
 *
 * Deliberately NOT `handleEnroll()` — reaching that function has always
 * meant "the lock itself spoke MQTT," proof of being un-bridged, and it
 * unconditionally sets `bridge_id = NULL` on that basis (its own header
 * comment). Calling it here would silently unpair every bridged lock the
 * instant it reported its capabilities. This only ever touches
 * `caps`/`transport` on an already-bonded (`app_id` set) lock — it does
 * not enroll, does not touch `bridge_id`/`status`/broker credentials.
 *
 * Detected by content shape (`caps` present as an array), not a `type`
 * field — firmware's payload is `addIdentity()`'s existing document, which
 * (like the enroll payload it's shared with) carries no `type`
 * discriminator of its own.
 */
async function handleUplinkCaps(deviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return false;
  }
  if (!Array.isArray(obj.caps)) return false;

  const [[lock]] = await pool.query('SELECT app_id FROM locks WHERE id = ?', [deviceId]);
  if (!lock) {
    logEvent('warn', `Capability report from unknown lock ${deviceId} — ignored`);
    return true;
  }
  // Directive: "this report should only come from bonded locks anyway" — an
  // unbonded lock has no owner to attribute the audit record to, and
  // nothing server-side should trust a capability claim from a lock that
  // isn't paired to anyone yet.
  if (!lock.app_id) {
    logEvent('warn', `Capability report from unbonded lock ${deviceId} — ignored`);
    return true;
  }

  const ident = deviceIdentity(obj);
  if (!ident.caps) {
    logEvent('warn', `Capability report from ${deviceId} — no recognised caps in payload, ignored`);
    return true;
  }

  await pool.query('UPDATE locks SET caps = ?, transport = COALESCE(?, transport) WHERE id = ?', [
    ident.caps,
    ident.transport,
    deviceId,
  ]);
  logEvent('info', `Lock ${deviceId} capability report via relay channel: caps=${ident.caps}`);
  await recordAudit(lock.app_id, deviceId, 'caps_update', `device-reported caps=${ident.caps}`);
  return true;
}

/** V1 (ozkey-17 §6a/§6c, normative wire wrapper corrected 2026-08-10):
 * lock->app uplink's wrapper is `{from, to, envelope_hex}` — "read by the
 * bridge for routing only" per §6c, i.e. addressing metadata, same as a
 * postal envelope's To/From. `kind`/`reason`/`bonds`/anything else lives
 * INSIDE `envelope_hex`, sealed, and is never touched. (Earlier revision
 * of this function assumed a top-level `msg_id` — that field does not
 * exist in the actual wire format; §6c is authoritative over the general
 * §6 prose that suggested it.)
 *
 * Persisted via recordAudit(), not just logEvent() — per the operator's
 * directive this is retained for OZPMS/OZLODGE audit-trail compliance,
 * unlike S8/S9's member-relay observation which is deliberately
 * ephemeral-only. `to` is read directly from the wrapper when present
 * (it's the actual routing address ozSemanticDispatch put there); falls
 * back to recordAudit()'s own `locks.app_id` lookup otherwise, so a
 * malformed/legacy payload still gets an audit row.
 */
async function logUplinkMetadata(fromDeviceId, payloadBuf, payloadStr) {
  const size = payloadBuf.length;
  let to = null;
  let isRealUplink = false;
  try {
    const obj = JSON.parse(payloadStr);
    if (typeof obj.to === 'string') to = obj.to;
    // XF-138 P5 (2026-08-31): the bridge now relays EVERY notification on the
    // lock's `member` characteristic here, not just genuine uplink content —
    // ordinary phone<->lock chatter (e.g. a `list_bonds` reply) rides the
    // same BLE notify broadcast and lands on this same topic as a side
    // effect, since BLE notify goes to every subscriber, not just the
    // requester (found live, firmware's own post above). Only the normative
    // §6c wrapper actually carries `envelope_hex` — anything else reaching
    // here is overheard chatter, not real uplink content, and must stay
    // `logEvent()`-only per this function's own long-standing rule (see
    // above: "unlike S8/S9's member-relay observation which is deliberately
    // ephemeral-only"). Without this check, every overheard exchange would
    // silently pollute the compliance-purposed audit_log table.
    isRealUplink = typeof obj.envelope_hex === 'string';
  } catch (_) {
    // Not JSON — fine, log size/from anyway. Not a warn like the other
    // topics: firmware/bridge versions may still be settling on this
    // still-new wire shape.
  }
  logEvent(
    'info',
    `Uplink from ${fromDeviceId} to ${to || '(unresolved)'}: ${size}B (content sealed, not read)` +
      (isRealUplink ? '' : ' — no envelope_hex, treating as overheard chatter, not persisting')
  );
  if (isRealUplink) await recordAudit(to, fromDeviceId, 'uplink', `size=${size}B`);
}

/** ozkey-31 §2. The uplink wrapper's `code` field is plaintext by design
 * (ozdoorlock_core.h ~2903, "1.64 — the outcome CODE travels in the clear,
 * the detail does not") specifically so a relay that never opens the
 * envelope can still see WHICH STAGE failed. As of doorlock-1.65 every code
 * the lock emits this way — ENVELOPE_BAD_HEX, ENVELOPE_NOT_OPENED,
 * MCU_TIMEOUT, COUNTER_REPLAY — is a refusal; success (`UNLOCK_OK`) only
 * ever rides BLE's notifyStatus(), which this server never sees. So this
 * treats ANY non-empty `code` as bad news rather than matching a fixed
 * whitelist — a whitelist is one more place a new refusal string firmware
 * adds later walks straight past unnoticed, the exact bug class this doc is
 * about. If firmware ever starts sending a positive confirmation on this
 * same field, this function needs to stop treating `code` as failure-only.
 */
async function handleUplinkOutcome(fromDeviceId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    return; // logUplinkMetadata already warned about non-JSON uplinks
  }
  if (typeof obj.code !== 'string' || !obj.code) return;

  const pending = lastGrantSentByDevice.get(fromDeviceId);
  if (!pending) {
    logEvent(
      'warn',
      `${fromDeviceId} reported outcome ${obj.code} — no pending grant on record to attribute it to`
    );
    return;
  }
  lastGrantSentByDevice.delete(fromDeviceId);

  // XF-155 (2026-09-03): distinguish which verb failed — 'revoke-key' gets
  // its own visible state ('revoke_failed') rather than reusing 'failed',
  // which read ambiguous ("failed to issue" vs "failed to revoke") once
  // revoke started tracking through this same path instead of optimistically
  // marking itself 'revoked' on publish alone.
  const isRevoke = pending.actionType === 'revoke-key';
  const failedStatus = isRevoke ? 'revoke_failed' : 'failed';
  await pool.query('UPDATE grants SET sync_status = ? WHERE id = ?', [failedStatus, pending.grantId]);
  logEvent(
    'warn',
    `Grant #${pending.grantId} (queue #${pending.queueId}, ${pending.actionType}) on ${fromDeviceId} FAILED — lock reported ${obj.code}`
  );
  await recordAudit(
    null,
    fromDeviceId,
    isRevoke ? 'revoke_failed' : 'grant_failed',
    `${pending.actionType} grant #${pending.grantId}: ${obj.code}`
  );
}

module.exports = {
  lastGrantSentByDevice,
  OZKIE_V2,
  refuseIfOzkieV2,
  handleUplinkReset,
  handleUplinkCaps,
  logUplinkMetadata,
  handleUplinkOutcome,
};
