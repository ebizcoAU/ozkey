'use strict';
/*
 * ozserver-core/ozkie.js — the OZKIE v2 relay (ozkey-56; ozkey-48 rev 7 §2.4).
 *
 * The owner app sends ONE frame (type ‖ message) to POST /locks/:id/ozkie; we
 * publish it, binary, to the lock's chosen bridge and hand back the lock's
 * answer in the same HTTP response. The app is the EDHOC initiator and the
 * OSCORE client end to end; we never hold a key and never open a message.
 *
 * TRANSIT-ONLY (PM nexus-46, 2026-11-17): frames live in this process's memory
 * for at most one wait and touch no table. Nothing is queued, retried or
 * re-sent (§2.4.4: a v2 unlock is never held). The only stored trace is one
 * content-free audit row per completed relay EDHOC (ozkey-56 §2, F1), behind
 * AUDIT_RELAY_BOND so it is a one-line removal if the operator rules so.
 *
 * What we read of a frame:
 *   - the type byte (closed set, §2.4.1);
 *   - type 1: the OUTER CoAP header up to the token (RFC 8613 keeps it
 *     plaintext) — correlation is (lock_id, token), §2.4.2;
 *   - type 0: the first byte only, to tell message_1 (a CBOR int, < 0x40)
 *     from message_3 (a byte string), §2.4.3.
 */

const { ctx } = require('./ctx');
const logEvent = (...args) => ctx.logEvent(...args);
const recordAudit = (...args) => require('./audit').recordAudit(...args);
const mqttPublishRaw = (...args) => require('./transport').mqttPublishRaw(...args);

const AUDIT_RELAY_BOND = true; // F1 toggle (ozkey-56 §2)

const T_EDHOC = 0x00;
const T_OSCORE = 0x01;
const MAX_MSG = 1024; // §2.1: maximum reassembled message
const MIN_TOKEN = 4; // §2.4.2: random token of ≥ 4 bytes
const MAX_TOPIC = 200; // §2.4.6

const cfg = (k, dflt) => (ctx.CONFIG && ctx.CONFIG[k] !== undefined ? ctx.CONFIG[k] : dflt);
const waitMs = () => cfg('OZKIE_WAIT_MS', 10_000);
const leaseMs = () => cfg('OZKIE_EDHOC_LEASE_MS', 35_000);
const m3WaitMs = () => cfg('OZKIE_M3_WAIT_MS', 2_000);

// In-memory counters by reason; C0b metrics read them. Never persisted.
const counters = Object.create(null);
function count(reason) {
  counters[reason] = (counters[reason] || 0) + 1;
}

// (lockId|tokenHex) -> { bridgeId, finish(result) }
const pending = new Map();
// lockId -> { holder, bridgeId, timer, waiter: { finish } | null }
const leases = new Map();

const key = (lockId, token) => `${lockId}|${token.toString('hex')}`;

/** Splits and checks a frame. Returns { type, msg } or { error }. */
function parseFrame(buf) {
  if (!Buffer.isBuffer(buf) || buf.length < 2) return { error: 'empty' };
  if (buf.length > 1 + MAX_MSG) return { error: 'too_large' };
  const type = buf[0];
  if (type !== T_EDHOC && type !== T_OSCORE && type !== 0x02) return { error: 'bad_type' };
  return { type, msg: buf.subarray(1) };
}

/** The CoAP token of a message (RFC 7252 §3), or null if the header is malformed. */
function coapToken(msg) {
  if (msg.length < 4) return null;
  if (msg[0] >> 6 !== 1) return null; // version 1
  const tkl = msg[0] & 0x0f;
  if (tkl > 8 || msg.length < 4 + tkl) return null;
  return msg.subarray(4, 4 + tkl);
}

const downTopic = (site, bridgeId, lockId) => `ozkie/${site}/bridges/${bridgeId}/ozkie/${lockId}`;

function refuse(status, code, detail, extra = {}) {
  count(code);
  return { status, body: { ok: false, code, error: code, detail, ...extra } };
}

function releaseLease(lockId) {
  const l = leases.get(lockId);
  if (!l) return;
  clearTimeout(l.timer);
  leases.delete(lockId);
}

/** Publishes `frame` and waits for `register(finish)`'s answer or `ms`. */
function publishAndWait(topic, frame, ms, register, onTimeout) {
  return new Promise((resolve) => {
    let done = false;
    let timer = null;
    const finish = (result) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve(result);
    };
    const unregister = register(finish);
    timer = setTimeout(() => {
      unregister();
      finish(onTimeout());
    }, ms);
    if (!mqttPublishRaw(topic, frame)) {
      unregister();
      finish(refuse(503, 'mqtt_down', 'The server is not connected to the broker. Nothing was sent.'));
    }
  });
}

const answer = (frame, bridgeId) => ({
  status: 200,
  body: { ok: true, frame_b64: frame ? frame.toString('base64') : null, bridge_id: bridgeId },
});

/**
 * One relayed request. The caller (api.js) has already checked the lock is v2,
 * the caller is its owner, and the bridge is online. `frame` is a Buffer.
 */
async function relayFrame({ siteId, lockId, bridgeId, appId, frame }) {
  const f = parseFrame(frame);
  if (f.error) return refuse(400, 'bad_frame', `Frame refused: ${f.error} (ozkey-48 §2.4.1).`, { reason: f.error });
  if (f.type === 0x02) return refuse(400, 'bad_frame', 'Only the lock ends a handshake (type 2).', { reason: 'bad_type' });

  const topic = downTopic(siteId, bridgeId, lockId);
  if (Buffer.byteLength(topic) > MAX_TOPIC) return refuse(500, 'topic_too_long', 'Topic exceeds 200 B (§2.4.6).');

  if (f.type === T_OSCORE) {
    const token = coapToken(f.msg);
    if (!token) return refuse(400, 'bad_frame', 'Malformed CoAP header.', { reason: 'bad_coap' });
    if (token.length < MIN_TOKEN) {
      return refuse(400, 'token_too_short', `The CoAP token must be at least ${MIN_TOKEN} random bytes (§2.4.2).`);
    }
    const k = key(lockId, token);
    if (pending.has(k)) return refuse(409, 'token_in_flight', 'A request with this token is already in flight.');
    return publishAndWait(
      topic,
      frame,
      waitMs(),
      (finish) => {
        pending.set(k, { bridgeId, finish: (frm) => finish(answer(frm, bridgeId)), abort: (r) => finish(r) });
        return () => pending.delete(k);
      },
      () => {
        logEvent('warn', `OZKIE relay: no answer from ${lockId} via ${bridgeId} in ${waitMs()} ms (never re-sent)`);
        return refuse(504, 'no_ack', 'No answer from the lock. Nothing is re-sent; for an actuation the outcome is unknown.', {
          outcome: 'unknown',
        });
      }
    ).finally(() => pending.delete(k));
  }

  // EDHOC (type 0): one lease per lock across all senders (§2.4.3).
  const isMsg1 = f.msg[0] < 0x40;
  const lease = leases.get(lockId);
  if (lease && lease.holder !== appId) {
    return refuse(409, 'edhoc_busy', 'Another sender holds this lock\'s relay handshake. Try again shortly.');
  }
  if (!isMsg1 && !lease) {
    return refuse(409, 'edhoc_no_lease', 'message_3 without a handshake in progress (send message_1 first).');
  }
  if (isMsg1) {
    // A new message_1 restarts the handshake (the lock does the same).
    releaseLease(lockId);
    const timer = setTimeout(() => leases.delete(lockId), leaseMs());
    timer.unref?.();
    leases.set(lockId, { holder: appId, bridgeId, timer, waiter: null });
  }
  const l = leases.get(lockId);
  if (l.bridgeId !== bridgeId) {
    releaseLease(lockId);
    return refuse(503, 'bridge_moved', 'The lock moved to another bridge mid-handshake. Start again.', { outcome: 'unknown' });
  }
  const ms = isMsg1 ? waitMs() : m3WaitMs();
  const result = await publishAndWait(
    topic,
    frame,
    ms,
    (finish) => {
      l.waiter = { finish: (frm) => finish(answer(frm, bridgeId)), abort: (r) => finish(r) };
      return () => {
        if (l.waiter) l.waiter = null;
      };
    },
    () => {
      if (isMsg1) {
        releaseLease(lockId);
        return refuse(504, 'no_ack', 'No message_2 from the lock.', { outcome: 'unknown' });
      }
      // message_3 + silence = accepted: the lock sends no message_4 (§3).
      return answer(null, bridgeId);
    }
  );
  l.waiter = null;
  if (!isMsg1) {
    releaseLease(lockId);
    if (result.status === 200 && result.body.frame_b64 === null) {
      logEvent('key', `OZKIE relay: handshake completed for ${lockId} by app ${appId} via ${bridgeId}`);
      if (AUDIT_RELAY_BOND) await recordAudit(appId, lockId, 'ozkie_edhoc', 'relay handshake completed (no content)');
    }
  }
  return result;
}

/** Uplink on ozkie/<site>/locks/<lockId>/ozkie/<bridgeId>. `buf` is the raw payload. */
function handleOzkieUplink(lockId, bridgeId, buf) {
  const f = parseFrame(buf);
  if (f.error || f.type === 0x02) {
    // A lock's type 2 is not sent today (§2.4.1); without a waiter it has nowhere to go.
    if (f.error) {
      count(`uplink_${f.error}`);
      logEvent('warn', `OZKIE relay: uplink from ${lockId} via ${bridgeId} dropped (${f.error})`);
      return;
    }
  }
  if (f.type === T_OSCORE) {
    const token = coapToken(f.msg);
    const p = token && pending.get(key(lockId, token));
    if (!p) {
      count('uplink_unmatched');
      logEvent('info', `OZKIE relay: unmatched OSCORE uplink from ${lockId} via ${bridgeId} dropped`);
      return;
    }
    if (p.bridgeId !== bridgeId) {
      count('uplink_wrong_bridge');
      logEvent('warn', `OZKIE relay: answer for ${lockId} came via ${bridgeId}, pinned to ${p.bridgeId}; dropped`);
      return;
    }
    p.finish(buf);
    return;
  }
  // type 0 (or 2): belongs to the lease holder's open wait.
  const l = leases.get(lockId);
  if (!l || !l.waiter) {
    count('uplink_no_lease');
    logEvent('info', `OZKIE relay: EDHOC uplink from ${lockId} with no handshake waiting; dropped`);
    return;
  }
  if (l.bridgeId !== bridgeId) {
    count('uplink_wrong_bridge');
    return;
  }
  l.waiter.finish(buf);
}

/** §2.4.5 step 4: the lock's bridge_id just moved to `newBridgeId` (null =
 *  unpaired). End at once every exchange pinned to any other bridge. */
function abortPinned(lockId, newBridgeId) {
  const moved = () => refuse(503, 'bridge_moved', 'The lock moved to another bridge mid-exchange.', { outcome: 'unknown' });
  let n = 0;
  for (const [k, p] of pending) {
    if (k.startsWith(`${lockId}|`) && p.bridgeId !== newBridgeId) {
      pending.delete(k);
      p.abort(moved());
      n++;
    }
  }
  const l = leases.get(lockId);
  if (l && l.bridgeId !== newBridgeId) {
    if (l.waiter) l.waiter.abort(moved());
    releaseLease(lockId);
    n++;
  }
  if (n) logEvent('warn', `OZKIE relay: ${n} exchange(s) for ${lockId} aborted, bridge now ${newBridgeId || '(none)'}`);
}

/** The 400 for a frame that failed the route's quick check (ozkey-56 §3.1 #1). */
function refuseFrame(buf) {
  const f = buf ? parseFrame(buf) : { error: 'not_base64' };
  const reason = f.error || 'bad_type';
  return refuse(400, 'bad_frame', `Frame refused: ${reason} (ozkey-48 §2.4.1).`, { reason });
}

function ozkieCounters() {
  return { ...counters };
}

module.exports = { relayFrame, refuseFrame, handleOzkieUplink, abortPinned, ozkieCounters, coapToken, parseFrame };
