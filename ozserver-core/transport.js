'use strict';
/*
 * ozserver-core/transport.js — MQTT: connection, subscriptions, topic dispatch, publish, retained time.
 *
 * Moved verbatim from ozlock/server/server.js in S0b (ozkey-50 §7); comments
 * keep their original references. Dependencies come from createCore() via
 * ./ctx (see index.js); other core modules are reached through late-bound
 * forwarders so load order never matters.
 */

const { ctx, lazy } = require('./ctx');
const CONFIG = lazy(() => ctx.CONFIG);
const pool = lazy(() => ctx.pool);
const mqtt = lazy(() => ctx.mqtt);
const logEvent = (...args) => ctx.logEvent(...args);

// From other core modules.
const handleUplinkReset = (...args) => require('./relay').handleUplinkReset(...args);
const handleUplinkCaps = (...args) => require('./relay').handleUplinkCaps(...args);
const logUplinkMetadata = (...args) => require('./relay').logUplinkMetadata(...args);
const handleUplinkOutcome = (...args) => require('./relay').handleUplinkOutcome(...args);
const flushQueueForDevice = (...args) => require('./queue').flushQueueForDevice(...args);
const eventsAtRisk = (...args) => require('./registry').eventsAtRisk(...args);
const recomputeAndStorePresence = (...args) => require('./registry').recomputeAndStorePresence(...args);
const handleLockPresence = (...args) => require('./registry').handleLockPresence(...args);
const refreshOnlineBridgeTimePushes = (...args) => require('./registry').refreshOnlineBridgeTimePushes(...args);
const runBridgeFailoverSweep = (...args) => require('./registry').runBridgeFailoverSweep(...args);
const handleBridgeScanResult = (...args) => require('./registry').handleBridgeScanResult(...args);
const handleBridgePresence = (...args) => require('./registry').handleBridgePresence(...args);
const handlePeerStatus = (...args) => require('./registry').handlePeerStatus(...args);
const handleEnroll = (...args) => require('./registry').handleEnroll(...args);
const deviceIdentity = (...args) => require('./registry').deviceIdentity(...args);
const { eventsAtRiskLogged } = require('./registry');

/* ---------------------------------------------------------------------------
 * MQTT — rendezvous engine (device-scoped topics, ozkey-04 §9)
 * ------------------------------------------------------------------------- */
let mqttClient = null;

function mqttPublish(topic, payload, opts = {}) {
  if (!mqttClient || !mqttClient.connected) {
    logEvent('warn', `MQTT offline — dropped publish to ${topic}`);
    return false;
  }
  mqttClient.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), {
    qos: 1,
    retain: !!opts.retain,
  });
  return true;
}

// ozkey-33: refresh cadence for the retained time topic — same period as
// the existing bridge `utc` push (CONFIG.UTC_PUSH_REFRESH_MS) so there is
// one clock-freshness budget across both paths, not two to keep in sync.
let timeRefreshTimer = null;
// XF-144 (2026-09-01): the multi-bridge failover sweep's own timer —
// deliberately separate from timeRefreshTimer, see its arming site for why.
let bridgeFailoverTimer = null;

function publishRetainedTime(why) {
  const nowUtc = Math.floor(Date.now() / 1000);
  // Signed minutes east of UTC. `Date.getTimezoneOffset()` returns minutes
  // WEST of UTC (Node's local zone, i.e. this host's) — negate it to match
  // the sign convention firmware asked for.
  const tzMinutes = -new Date().getTimezoneOffset();
  const ok = mqttPublish(
    CONFIG.topicTime(CONFIG.SITE_ID),
    { utc: nowUtc, tz: tzMinutes },
    { retain: true }
  );
  if (ok) logEvent('info', `Published retained time (utc=${nowUtc}, tz=${tzMinutes}) to ${CONFIG.topicTime(CONFIG.SITE_ID)} (${why})`);
}

/** S8/S9 (ozkey-15 §3, async orchestrated removal). `request_remove` (banoi2
 * -> banoi1) and `ack_remove` (banoi1 -> banoi2) are already addressed to the
 * specific recipient's device_id in the topic path, so the broker delivers
 * publish -> subscribe directly between the two apps — nothing here needs to
 * republish anything. (It also MUST NOT: this process is itself subscribed
 * to both wildcards, so publishing back onto the same topic would be a
 * self-triggering infinite loop, not a relay.) This only logs for
 * visibility — no DB write, no `pending_queue`/`grants` involvement — per
 * §2 "pure relay, no state, no persistence". `targetDeviceId` is the
 * recipient named in the topic path (the admin's device_id for
 * request_remove, the member's for ack_remove).
 *
 * "Basic payload structure" check only, per §3 — not full validation. Who
 * may publish is an MQTT ACL matter (§"No authentication changes"), not
 * this code's job.
 */
function logMemberRelay(targetDeviceId, kind, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on members/${targetDeviceId}/${kind}: "${payload.slice(0, 60)}"`);
    return;
  }
  if (!obj.request_id || !obj.target_lock_id || !obj.target_member_app_id) {
    logEvent(
      'warn',
      `members/${targetDeviceId}/${kind} missing request_id/target_lock_id/target_member_app_id`
    );
    return;
  }
  const memberTag = String(obj.target_member_app_id).slice(0, 12) + '…';
  if (kind === 'request_remove') {
    logEvent(
      'key',
      `Removal request ${obj.request_id}: member ${memberTag} -> admin ${targetDeviceId} ` +
        `for lock ${obj.target_lock_id}`
    );
  } else {
    logEvent(
      'key',
      `Removal ACK ${obj.request_id}: status=${obj.status || '?'} -> member ${targetDeviceId} ` +
        `for lock ${obj.target_lock_id}`
    );
  }
}

function initMqtt() {
  mqttClient = mqtt.connect(CONFIG.MQTT_URL, {
    clientId: `${CONFIG.SERVICE_NAME}-${Math.random().toString(16).slice(2, 8)}`,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });

  mqttClient.on('connect', () => {
    logEvent('info', `MQTT online — broker ${CONFIG.MQTT_URL}`);
    // S16: subscribe to both ozkie/ (new) and ozkey/ (legacy) roots during
    // the migration — drop the *_LEGACY entries in a follow-up once every
    // publisher (firmware, bridge, this server, ozpmsserv) is confirmed on
    // ozkie/ only.
    mqttClient.subscribe(
      [
        CONFIG.SUB_ENROLL,
        CONFIG.SUB_ENROLL_LEGACY,
        CONFIG.SUB_HEARTBEAT,
        CONFIG.SUB_HEARTBEAT_LEGACY,
        CONFIG.SUB_MEMBER_REQUEST_REMOVE,
        CONFIG.SUB_MEMBER_REQUEST_REMOVE_LEGACY,
        CONFIG.SUB_MEMBER_ACK_REMOVE,
        CONFIG.SUB_MEMBER_ACK_REMOVE_LEGACY,
        CONFIG.SUB_UPLINK,
        CONFIG.SUB_UPLINK_LEGACY,
        CONFIG.SUB_PRESENCE_LOCKS,
        CONFIG.SUB_PRESENCE_LOCKS_LEGACY,
        CONFIG.SUB_PRESENCE_BRIDGES,
        CONFIG.SUB_PRESENCE_BRIDGES_LEGACY,
        CONFIG.SUB_BRIDGE_SCAN_RESULT,
        CONFIG.SUB_PEER_STATUS,
      ],
      { qos: 1 },
      (err) => {
        if (err) logEvent('error', `MQTT subscribe failed: ${err.message}`);
        else
          logEvent(
            'info',
            `Subscribed: ${CONFIG.SUB_ENROLL} + ${CONFIG.SUB_HEARTBEAT} + ` +
              `${CONFIG.SUB_MEMBER_REQUEST_REMOVE} + ${CONFIG.SUB_MEMBER_ACK_REMOVE} + ` +
              `${CONFIG.SUB_UPLINK} + ${CONFIG.SUB_PRESENCE_LOCKS} + ` +
              `${CONFIG.SUB_PRESENCE_BRIDGES} + ${CONFIG.SUB_BRIDGE_SCAN_RESULT} + ` +
              `${CONFIG.SUB_PEER_STATUS}` +
              ' (+ legacy ozkey/ roots during S16 migration)' +
              ' (door-event topic deliberately NOT subscribed — see header)'
          );
      }
    );

    // ozkey-33: publish immediately so a cold start (or a reconnect) never
    // leaves the retained topic stale, then keep it fresh on the same
    // cadence as the bridge `utc` push. Clear any timer from a prior
    // `connect` (a broker reconnect fires this handler again) so repeated
    // reconnects don't stack intervals.
    publishRetainedTime('MQTT connect');
    if (timeRefreshTimer) clearInterval(timeRefreshTimer);
    timeRefreshTimer = setInterval(() => {
      publishRetainedTime('periodic refresh');
      refreshOnlineBridgeTimePushes();
    }, CONFIG.UTC_PUSH_REFRESH_MS);

    // XF-144: a separate, much shorter-period timer than the UTC refresh
    // above — the failover sweep's own condition (FAILOVER_GRACE_MS, 2 min)
    // already provides the real debounce against reacting to a blip;
    // running the SWEEP itself only every 10 minutes (the UTC cadence)
    // would add up to 10 more minutes of delay on top of that grace period
    // for an actually-dead bridge, which defeats the point. 30s is frequent
    // enough to fail over promptly once the grace period genuinely elapses
    // without being a meaningful DB load.
    if (bridgeFailoverTimer) clearInterval(bridgeFailoverTimer);
    bridgeFailoverTimer = setInterval(() => {
      runBridgeFailoverSweep().catch((err) =>
        logEvent('error', `XF-144 failover sweep failed: ${err.message}`)
      );
      // XF-152 (2026-09-03): a THIRD incident, distinct from the write-
      // ordering race fixed above — a bridge's own retained presence topic
      // was already correctly 'online' at the broker, but this server's
      // subscription silently never delivered it, with no reconnect/error/
      // offline event logged anywhere in between. Root cause not confirmed
      // (transport-level — QoS/broker/network, not app code); republishing
      // the same payload fixed it live. Mitigation, root-cause-agnostic:
      // a bare re-SUBSCRIBE to an already-active topic filter does NOT
      // force retained redelivery in practice against this broker
      // (verified live — a duplicate subscribe produced zero redelivered
      // messages). UNSUBSCRIBE then SUBSCRIBE is unambiguously a fresh
      // subscription from the broker's point of view, so it does — this
      // self-heals a silently dropped presence message within ~30s instead
      // of sitting stale indefinitely. Not a diagnosis of why the original
      // delivery was missed.
      const presenceTopics = [
        CONFIG.SUB_PRESENCE_LOCKS,
        CONFIG.SUB_PRESENCE_LOCKS_LEGACY,
        CONFIG.SUB_PRESENCE_BRIDGES,
        CONFIG.SUB_PRESENCE_BRIDGES_LEGACY,
      ];
      mqttClient.unsubscribe(presenceTopics, (err) => {
        if (err) return logEvent('error', `XF-152 presence unsubscribe (resync) failed: ${err.message}`);
        mqttClient.subscribe(presenceTopics, { qos: 1 }, (err2) => {
          if (err2) logEvent('error', `XF-152 presence re-subscribe (resync) failed: ${err2.message}`);
        });
      });
    }, CONFIG.FAILOVER_SWEEP_MS);
  });

  mqttClient.on('reconnect', () => logEvent('warn', 'MQTT reconnecting...'));
  mqttClient.on('error', (err) => logEvent('error', `MQTT error: ${err.message}`));
  mqttClient.on('offline', () => logEvent('warn', 'MQTT broker offline'));

  mqttClient.on('message', async (topic, payloadBuf) => {
    const payload = payloadBuf.toString('utf8').trim();
    try {
      // S16: accept either the new `ozkie/` root or legacy `ozkey/` during
      // the migration window (see the SUB_* comment near CONFIG). Drop the
      // `(?:ozkey|ozkie)` alternation back to a plain `ozkie` once every
      // publisher is confirmed off the legacy root.
      const m = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/(enroll|heartbeat|log)$/);
      if (!m) {
        // S8/S9 (ozkey-15 §3): observe-only, see the SUB_MEMBER_* comment above.
        const mm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/members\/([^/]+)\/(request_remove|ack_remove)$/);
        if (mm) {
          logMemberRelay(mm[2], mm[3], payload);
          return;
        }
        // V1 (ozkey-17 §6/§6a/§6c): lock->app uplink, published under the
        // lock's own topic. Wire wrapper is `{from, to, envelope_hex}`
        // (§6c, normative) — "content you never parse" means never
        // touching `envelope_hex`; `from`/`to`/size are the addressing
        // metadata, recorded to audit_log for OZPMS/OZLODGE compliance,
        // exactly like grant/revoke/unlock already are.
        const um = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/uplink$/);
        if (um) {
          if (await handleUplinkReset(um[2], payload)) return;
          if (await handleUplinkCaps(um[2], payload)) return;
          await logUplinkMetadata(um[2], payloadBuf, payload);
          await handleUplinkOutcome(um[2], payload);
          return;
        }
        // ozkey-20 R1: retained LWT presence, lock or bridge. Pure metadata
        // (operational, per §6a's own class table) — updates `presence`
        // directly, no content to protect.
        const plm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/locks\/([^/]+)\/presence$/);
        if (plm) {
          await handleLockPresence(plm[2], payload);
          return;
        }
        const pbm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/presence$/);
        if (pbm) {
          await handleBridgePresence(pbm[2], payload);
          return;
        }
        const bsm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/scan_result$/);
        if (bsm) {
          handleBridgeScanResult(bsm[2], payload);
          return;
        }
        const psm = topic.match(/^(?:ozkey|ozkie)\/([^/]+)\/bridges\/([^/]+)\/peer_status$/);
        if (psm) {
          await handlePeerStatus(psm[2], payload);
          return;
        }
        return;
      }
      const [, siteId, topicDeviceId, kind] = m;

      let obj = {};
      try {
        obj = JSON.parse(payload);
      } catch (_) {
        logEvent('warn', `Non-JSON payload on ${topic}: "${payload.slice(0, 60)}"`);
        return;
      }

      /* -- Enrollment: verify token, bind device -> site/owner ------------- */
      if (kind === 'enroll') {
        await handleEnroll(siteId, topicDeviceId, obj);
        return;
      }

      const deviceId = String(obj.device_id || topicDeviceId);

      /* -- Wake heartbeat: update presence, flush the queue ----------------- */
      if (kind === 'heartbeat') {
        // XF-57: the heartbeat is the only message a lock in service sends
        // unprompted, so it is the only thing that can heal a stale row without
        // anyone visiting the door. Written on EVERY beat, not just on change —
        // we have no reliable "changed" signal and a conditional write here would
        // just be a second thing to get wrong.
        const id = deviceIdentity(obj);
        // `fw` too, and it was the same staleness bug as transport — caught on the
        // bench 2026-08-03 with a lock running 1.6 whose row still read 1.4. The
        // heartbeat has ALWAYS carried fw; only handleEnroll wrote it, and an
        // already-enrolled lock never re-enrolls, so the column froze at whatever
        // version first enrolled and never moved again.
        // It matters because `locks.fw` is how we and BANOI tell which contract a
        // device speaks. A frozen value answers that question wrongly and with
        // total confidence.
        const fw = typeof obj.fw === 'string' && obj.fw.length <= 50 ? obj.fw : null;
        // ozkey-20 R4 (health payload) — read opportunistically. Firmware
        // doesn't send battery_pct/pending_uplinks/last_mech_result yet
        // (only roster_epoch exists on the wire so far, per a live check
        // against ozdoorlock_core.h 2026-08-10) — COALESCE means an absent
        // field leaves the column untouched rather than clobbering it to
        // NULL/0, same discipline as fw/transport/caps above.
        const batteryPct = Number.isFinite(obj.battery_pct) ? Math.max(0, Math.min(100, obj.battery_pct)) : null;
        const pendingUplinks = Number.isFinite(obj.pending_uplinks) ? obj.pending_uplinks : null;
        const lastMechResult = typeof obj.last_mech_result === 'string' ? obj.last_mech_result.slice(0, 16) : null;
        const rosterEpoch = Number.isFinite(obj.roster_epoch) ? obj.roster_epoch : null;
        // ozkey-20 §5a (2026-08-11) — mcu_link_up/mcu_last_frame_s. Same
        // opportunistic COALESCE discipline: absent leaves the column
        // untouched, doesn't clobber a last-known value to NULL.
        const mcuLinkUp = typeof obj.mcu_link_up === 'boolean' ? (obj.mcu_link_up ? 1 : 0) : null;
        const mcuLastFrameS = Number.isFinite(obj.mcu_last_frame_s) ? obj.mcu_last_frame_s : null;
        // ozkey-32 §5, operator's call 2026-08-14: the lock is authoritative
        // for its own name (it can be renamed over BLE with no connectivity,
        // which the server would never see any other way), `locks.label` is
        // a cache. Not on the wire yet — firmware ships it on request — same
        // opportunistic COALESCE discipline as fw/transport/battery_pct etc.
        // above: absent leaves the column untouched, doesn't clobber a
        // last-known label to NULL.
        const name = typeof obj.name === 'string' && obj.name.length ? obj.name.slice(0, 255) : null;
        // XF-125 P0 — opportunistic, same discipline as everything above:
        // absent (a lock's `publishHeartbeat()` doesn't send these yet — only
        // the Thread beacon, verbatim-relayed onto this same topic by
        // bridge32, does today) leaves the columns untouched.
        const seqHighwater = Number.isFinite(obj.seq_highwater) ? obj.seq_highwater : null;
        const droppedBeforeSeq = Number.isFinite(obj.dropped_before_seq) ? obj.dropped_before_seq : null;
        // XF-127 — same opportunistic COALESCE discipline as everything
        // above: absent leaves the column untouched, doesn't clobber a
        // known `true` back to unknown.
        const hasDoorbell = typeof obj.has_doorbell === 'boolean' ? (obj.has_doorbell ? 1 : 0) : null;
        // XF-127 §8 ask 2 — same wire (Thread beacon, verbatim-relayed onto
        // `heartbeat`) as seq_highwater/dropped_before_seq/has_doorbell;
        // not yet sent on the direct-Wi-Fi publishHeartbeat() either.
        const profileMismatch = typeof obj.profile_mismatch === 'boolean' ? (obj.profile_mismatch ? 1 : 0) : null;
        // XF-127 §11 (reproduced on the bench, blocks XF-126 verification):
        // flagged as a real gap in §9 and deliberately left out of that
        // ticket's scope — XF-122 §5 ask 3 only wrote these at enrol. A
        // Thread lock learns its PID from the MCU seconds AFTER enrolling,
        // and a lock reflashed to a different profile without a full
        // re-enrol keeps reporting the new one forever while this stayed
        // frozen at NULL/stale. Same opportunistic COALESCE as every field
        // above — a lock that stops reporting doesn't blank a known value.
        const tuyaPidHb = typeof obj.tuya_pid === 'string' && obj.tuya_pid.length <= 32 ? obj.tuya_pid : null;
        const profileHb = typeof obj.profile === 'string' && obj.profile.length <= 64 ? obj.profile : null;
        // XF-134: diagnostic-only device fact, see the migration comment.
        const bonds = Number.isFinite(obj.bonds) ? obj.bonds : null;
        await pool.query(
          `UPDATE locks
              SET last_seen_at    = NOW(),
                  fw              = COALESCE(?, fw),
                  transport       = COALESCE(?, transport),
                  caps            = COALESCE(?, caps),
                  battery_pct     = COALESCE(?, battery_pct),
                  pending_uplinks = COALESCE(?, pending_uplinks),
                  last_mech_result = COALESCE(?, last_mech_result),
                  last_mech_at    = CASE WHEN ? IS NOT NULL THEN NOW() ELSE last_mech_at END,
                  roster_epoch    = COALESCE(?, roster_epoch),
                  mcu_link_up     = COALESCE(?, mcu_link_up),
                  mcu_last_frame_s = COALESCE(?, mcu_last_frame_s),
                  label           = COALESCE(?, label),
                  seq_highwater   = COALESCE(?, seq_highwater),
                  dropped_before_seq = COALESCE(?, dropped_before_seq),
                  has_doorbell    = COALESCE(?, has_doorbell),
                  profile_mismatch = COALESCE(?, profile_mismatch),
                  tuya_pid        = COALESCE(?, tuya_pid),
                  profile         = COALESCE(?, profile),
                  bonds           = COALESCE(?, bonds)
            WHERE id = ?`,
          [fw, id.transport, id.caps, batteryPct, pendingUplinks, lastMechResult, lastMechResult, rosterEpoch, mcuLinkUp, mcuLastFrameS, name, seqHighwater, droppedBeforeSeq, hasDoorbell, profileMismatch, tuyaPidHb, profileHb, bonds, deviceId]
        );
        if (droppedBeforeSeq !== null) {
          const [[riskRow]] = await pool.query(
            'SELECT dropped_before_seq, last_pulled_seq FROM locks WHERE id = ?',
            [deviceId]
          );
          const atRisk = riskRow && eventsAtRisk(riskRow);
          if (atRisk && !eventsAtRiskLogged.has(deviceId)) {
            eventsAtRiskLogged.add(deviceId);
            logEvent(
              'warn',
              `XF-125: ${deviceId} events AT RISK — rotation destroyed records up to ` +
                `seq ${riskRow.dropped_before_seq}, no app has confirmed past ` +
                `${riskRow.last_pulled_seq ?? '(never)'}`
            );
          } else if (!atRisk && eventsAtRiskLogged.has(deviceId)) {
            eventsAtRiskLogged.delete(deviceId);
            logEvent('info', `XF-125: ${deviceId} events risk cleared — a consumer caught up`);
          }
        }
        // ozkey-20 R5/R6: a heartbeat is direct proof of reachability for a
        // Wi-Fi-direct lock (no bridge_id) — set presence straight to
        // 'online' the way an R1 LWT eventually will, then let R6 compute
        // whatever reason applies (battery/mech/pending) on top of that. A
        // bridged lock's presence is NOT touched here — its reachability
        // comes from the bridge's own connect/disconnect relay, not its
        // heartbeat.
        //
        // XF-146 (2026-09-01): writes `direct_presence`, not `presence` —
        // same fix as `handleLockPresence()`, same reason: this is a raw
        // signal, and `recomputeAndStorePresence()` (called right below)
        // now reads `direct_presence` exclusively for that check, never
        // its own prior derived output.
        const [[hbLockRow]] = await pool.query('SELECT bridge_id FROM locks WHERE id = ?', [deviceId]);
        if (hbLockRow && !hbLockRow.bridge_id) {
          await pool.query("UPDATE locks SET direct_presence = 'online' WHERE id = ?", [deviceId]);
        }
        if (hbLockRow) await recomputeAndStorePresence(deviceId);
        const sent = await flushQueueForDevice(siteId, deviceId);
        if (sent > 0) return; // flush already logged
        return; // quiet heartbeat
      }

      /* -- Door access transactions: NOT HANDLED, deliberately ---------------- *
       * Removed 2026-07-31 with `lock_logs` (see header). We no longer subscribe
       * to the `log` topic, so `kind === 'log'` cannot arrive here — but if a
       * future change re-adds that subscription, note what USED to be here:
       * besides the INSERT, this branch called
       *   logEvent('lock', `Door GRANTED — <detail> @ "<label>"`)
       * which put the door event in the rolling event ring served to the
       * dashboard terminal AND on stdout. Dropping only the table would have
       * left the same data in the log stream and looked like a fix. If door
       * events are ever wanted, they belong on OZPMSSERV/OZKEYSERV — not here.
       */
    } catch (err) {
      logEvent('error', `MQTT message handler fault on ${topic}: ${err.message}`);
    }
  });
}


/** For /health: is the MQTT client connected right now. */
function isMqttConnected() {
  return !!(mqttClient && mqttClient.connected);
}

module.exports = {
  mqttPublish,
  publishRetainedTime,
  logMemberRelay,
  initMqtt,
  isMqttConnected,
};
