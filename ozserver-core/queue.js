'use strict';
/*
 * ozserver-core/queue.js — pending_queue: per-device serialized flush of queued sealed commands,
 * routed direct or via the lock's bridge (ozkey-11 §3).
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

// From other core modules.
const recordAudit = (...args) => require('./audit').recordAudit(...args);
const mqttPublish = (...args) => require('./transport').mqttPublish(...args);
const { lastGrantSentByDevice, OZKIE_V2 } = require('./relay');

/** Drain queued actions for a device; expired unlock-style rows are skipped.
 * `onSent(job, msgId)`, if given, fires for each job actually published —
 * lets a caller that just queued a specific job (e.g. `DELETE /locks/:id`)
 * learn the `msg_id` that went out for it, without every other caller
 * needing to care (ozkey-41 §5.3 correlation). It fires synchronously right
 * after the publish, before this function next yields, so a reply waiter
 * registered inside it is in place before any reply can be handled. */
// XF-155 (2026-09-03): found live while investigating App's report of a
// physical-lock regression after a burst of near-simultaneous revoke
// requests. Multiple callers can invoke this for the SAME deviceId
// concurrently (every write endpoint fires it fire-and-forget right after
// its own commit — grant, revoke, unlock, bond verbs — and App's expiry
// sweep did exactly that, 6-8 times within ~3s for one lock). With no
// per-device serialization, each concurrent call independently SELECTs the
// same still-'queued' rows before any of them has a chance to mark one
// 'sent', so the SAME command gets published multiple times. Confirmed
// live: 6 concurrent revoke requests produced 3-4x duplicate publishes of
// EVERY one of them on the real command topic — exactly the kind of
// self-inflicted BLE-link hammering XF-142/143/149/152 already documented
// from other causes, just never traced to this one before. Same fix shape
// as `handleBridgePresence()`'s XF-152 fix: chain concurrent calls for one
// deviceId onto that device's own promise-queue tail so they process
// strictly in arrival order — the second call's SELECT then correctly sees
// the first call's jobs already 'sent'.
const flushQueueQueues = new Map(); // deviceId -> Promise (tail of chain)

function flushQueueForDevice(siteId, deviceId, onSent) {
  const prev = flushQueueQueues.get(deviceId) || Promise.resolve();
  const next = prev.catch(() => {}).then(() => flushQueueForDeviceImpl(siteId, deviceId, onSent));
  flushQueueQueues.set(deviceId, next);
  return next;
}

async function flushQueueForDeviceImpl(siteId, deviceId, onSent) {
  const [queued] = await pool.query(
    "SELECT * FROM pending_queue WHERE device_id = ? AND status = 'queued' ORDER BY id ASC",
    [deviceId]
  );
  if (queued.length === 0) return 0;

  // Transport routing (ozkey-11 §3). A bridged (Thread) lock never subscribes
  // to MQTT itself, so publishing to its device-scoped topic reaches nobody —
  // the command must go to its BRIDGE's topic, naming the lock in `target`.
  const [[lockRow]] = await pool.query('SELECT bridge_id, ozkie_version FROM locks WHERE id = ?', [deviceId]);
  const bridgeId = lockRow && lockRow.bridge_id ? lockRow.bridge_id : null;

  // ozkey-48 backstop: every relay route already refuses a v2 lock before
  // queueing (`refuseIfOzkieV2`), so a row here means one slipped past — or
  // was queued before the lock's row was marked v2. A v1 envelope means
  // nothing to a v2 lock and an actuation must never sit in this queue
  // (§4.2), so drop it rather than publish it.
  if (lockRow && lockRow.ozkie_version === OZKIE_V2) {
    for (const job of queued) {
      await pool.query("UPDATE pending_queue SET status = 'expired' WHERE id = ?", [job.id]);
      logEvent('warn', `Queue #${job.id} (${job.action_type}) dropped — ${deviceId} is OZKIE v2 (ozkey-48)`);
    }
    return 0;
  }

  let sent = 0;
  for (const job of queued) {
    // ozkey-05 §6.3: commands must never fire stale.
    if (job.expires_at && new Date(job.expires_at).getTime() < Date.now()) {
      await pool.query("UPDATE pending_queue SET status = 'expired' WHERE id = ?", [job.id]);
      logEvent('warn', `Queue #${job.id} (${job.action_type}) expired before ${deviceId} woke`);
      // XF-155 (2026-09-03): a revoke-key job that expires unsent — the
      // device never woke to receive it in time — used to leave the grants
      // row silently stuck at 'revoking' forever, with nothing anywhere
      // pointing at it. Surface it the same way an explicit lock-reported
      // failure does.
      if (job.action_type === 'revoke-key' && job.grant_id) {
        await pool.query("UPDATE grants SET sync_status = 'revoke_failed' WHERE id = ?", [
          job.grant_id,
        ]);
        await recordAudit(
          null,
          deviceId,
          'revoke_failed',
          `revoke-key grant #${job.grant_id}: queue #${job.id} expired unsent`
        );
      }
      continue;
    }

    const sealed = job.msg_type === 'sealed_envelope';

    const commandTopic = bridgeId
      ? CONFIG.topicBridgeCommand(siteId, bridgeId)
      : CONFIG.topicCommand(siteId, deviceId);
    const envelope = {
      msg_id: `ozl-${job.id}-${Date.now()}`,
      device_id: deviceId,
      action: job.action_type,
      grant_id: job.grant_id,
      ...(sealed ? { envelope_hex: job.envelope_hex } : { payload_hex: job.payload_hex }),
      issued_at: new Date().toISOString(),
      source: CONFIG.SERVICE_NAME,
    };
    // bridge32 demuxes on {target, payload}/{target, envelope_hex}
    // (CONTRACT-BRIDGE / ozkey-11 §3, extended ozkey-13 §8 S7/BR1/F7). Send
    // it a MINIMAL envelope: the bridge reads only target + one content
    // field and rebuilds its own datagram from them, so msg_id/action/
    // grant_id/issued_at/source never cross the Thread hop and are pure
    // overhead on a constrained link. Keeping them nearly broke the product:
    // PubSubClient's default MQTT_MAX_PACKET_SIZE is 256 bytes and silently
    // discards anything larger, so the ~280-byte full envelope was dropped
    // by every bridge without a trace while short hand-made test publishes
    // sailed through (found live 2026-07-29). bridge32 now also calls
    // setBufferSize(1024), but keeping the wire small is the belt to that
    // braces — a stock-configured bridge, or one on an older build, still
    // works. The bridge never decodes `envelope_hex` — pure pass-through,
    // same as `payload` always was; only the field name differs.
    const publishBody = bridgeId
      ? (sealed
          ? { msg_id: envelope.msg_id, target: deviceId, envelope_hex: job.envelope_hex }
          : { msg_id: envelope.msg_id, target: deviceId, payload: job.payload_hex })
      : envelope;

    const ok = mqttPublish(commandTopic, publishBody);
    if (!ok) break;
    if (onSent) onSent(job, envelope.msg_id);

    await pool.query("UPDATE pending_queue SET status = 'sent' WHERE id = ?", [job.id]);
    if (job.grant_id) {
      // ozkey-31 §2 / XF-155 (2026-09-03): a publish is evidence the SERVER
      // sent something, not that the lock stored it — the exact gap that let
      // four failed remote grants sit recorded as 'synced' this week while
      // ozControlOpen rejected every one of them. Applies equally to
      // revoke-key, which used to optimistically flip straight to 'revoked'
      // here on publish alone (App found this live: a revoke against a
      // genuinely-dead lock reported "Đã thu hồi" exactly as fast as a real
      // one, no confirmation the credential was ever cleared). Stay in the
      // current in-flight state ('pending' for grant-key, 'revoking' for
      // revoke-key — set by the caller before this runs) and remember which
      // grant + which action this device's next uplink outcome code
      // (handleUplinkOutcome, below) should be attributed to; a reported
      // failure is the only state a publish can ever earn on its own now.
      lastGrantSentByDevice.set(deviceId, {
        grantId: job.grant_id,
        queueId: job.id,
        actionType: job.action_type,
      });
    }
    sent++;
    logEvent(
      'sync',
      `${deviceId} wake -> burst ${job.action_type} #${job.id} down ${commandTopic}` +
        (bridgeId ? ` (via bridge ${bridgeId})` : '')
    );
  }
  return sent;
}

module.exports = {
  flushQueueQueues,
  flushQueueForDevice,
  flushQueueForDeviceImpl,
};
