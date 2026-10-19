'use strict';
/*
 * ozserver-core/registry.js — Locks and bridges: enrollment, presence + fault attribution (ozkey-20
 * R5/R6), reconciliation, XF-144 failover/scan, reset verdict waiters,
 * capabilities (XF-48/57/58).
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
const normalizeMac = (...args) => require('./util').normalizeMac(...args);
const makeSecret = (...args) => require('./util').makeSecret(...args);
const recordAudit = (...args) => require('./audit').recordAudit(...args);
const mqttPublish = (...args) => require('./transport').mqttPublish(...args);

/** ozkey-23 §10.2a: mint-once, same shape as the lock's `handleEnroll()`
 *  (`brokerUsername = id`, `brokerSecret = makeSecret(16, 'ozl_')`). Returns
 *  {broker_username, broker_secret, minted: bool} — `minted` tells the
 *  caller whether this is the first time (worth pushing/returning) or a
 *  repeat lookup of an already-provisioned principal. */
async function getOrMintBridgeCredentials(bridgeId) {
  const [[row]] = await pool.query('SELECT broker_username, broker_secret FROM bridges WHERE id = ?', [
    bridgeId,
  ]);
  if (row && row.broker_secret) return { ...row, minted: false };
  const broker_username = bridgeId;
  const broker_secret = makeSecret(16, 'ozl_');
  await pool.query(
    `INSERT INTO bridges (id, broker_username, broker_secret) VALUES (?, ?, ?)
       ON DUPLICATE KEY UPDATE broker_username = VALUES(broker_username),
         broker_secret = VALUES(broker_secret)`,
    [bridgeId, broker_username, broker_secret]
  );
  return { broker_username, broker_secret, minted: true };
}

// XF-125 P0/§3: a lock is AT RISK exactly when rotation has destroyed events
// (`dropped_before_seq`) that no app has ever confirmed pulling
// (`last_pulled_seq`, via PATCH /locks/:id — there is no wire signal to
// observe instead, since `query_events` is a BLE-only exchange the server
// never sees, confirmed against ozdoorlock_core.h:6103-6161).
// This is the precise "silent data loss" case firmware's own comment names
// (heartbeat handler, "a consumer whose cursor is below dropped_before_seq
// has provably missed records") — not a fuzzy staleness threshold, an exact
// check against a fact firmware already computes and reports.
function eventsAtRisk(lock) {
  // XF directive S7, found live 2026-08-23: `!= null` is true for 0, but
  // firmware's own wire contract (XFtposDecisions-126 §13.2 area /
  // ozdoorlock_core.h `hb["dropped_before_seq"]`) documents 0 as the "no
  // records lost yet" sentinel, not a real loss. `!= null` was flagging
  // every healthy lock with a fresh ring buffer as at-risk on its very
  // first heartbeat carrying the field — confirmed live: LockB logged
  // "events AT RISK — rotation destroyed records up to seq 0" the instant
  // it started reporting, having lost nothing. `> 0` is null-safe in JS
  // (`null > 0` is false) and only trips on an actual positive loss count.
  return (
    lock.dropped_before_seq > 0 &&
    (lock.last_pulled_seq == null || lock.dropped_before_seq > lock.last_pulled_seq)
  );
}

// XF-131 Ask 2: `eventsAtRisk` only fires once rotation has actually
// destroyed something unread — the LATE warning. This is the EARLY one:
// the lock has events (`seq_highwater > 0`) nobody has confirmed pulling
// yet, whether or not any have been lost. A backlog nobody can see is the
// same failure as no backlog at all (the ticket's own framing) — this is
// what makes it visible before it becomes unrecoverable. `> 0` guard for
// the same reason as eventsAtRisk: a lock with zero events ever written
// and last_pulled_seq still null must read false, not true.
function eventsPending(lock) {
  return (
    lock.seq_highwater > 0 &&
    (lock.last_pulled_seq == null || lock.seq_highwater > lock.last_pulled_seq)
  );
}

// Log once per episode, not once per heartbeat — a lock stuck at-risk would
// otherwise re-log on every beat for as long as nobody reads it, which is
// exactly the kind of noise that makes a real alert easy to tune out.
const eventsAtRiskLogged = new Set(); // deviceId

/* ---------------------------------------------------------------------------
 * ozkey-20 R5/R6 — observed presence + fault attribution
 *
 * Built ahead of firmware's R1/R2/R4 (LWT, bridge liveness table, health
 * payload) — none of those exist on the wire yet, same position SUB_UPLINK
 * was in before V1 shipped. The receiving/attribution logic is complete and
 * live-testable with synthetic MQTT publishes now; it starts producing real
 * verdicts with zero further server change the moment firmware catches up.
 * ------------------------------------------------------------------------- */

const BATTERY_LOW_PCT = 15; // ozkey-20 R6 table's own figure, not derived here.

/** ozkey-20 R6, evaluated exactly as the doc's table specifies — top-down,
 * first match wins. `bridgePresence` is looked up by the caller so this
 * function stays a pure function of its inputs (easy to unit-test/verify
 * with synthetic rows, matching this session's "verify, don't assume"
 * discipline). Q2 (flap damping) is NOT implemented here — server-side
 * hysteresis is agreed (ozkey-20 reply) but is a separate, later addition;
 * this is the bare attribution logic the doc calls "the deliverable".
 *
 * XF-138 P5 (2026-08-31): every lock now carries its own direct presence
 * signal into this function — Wi-Fi-direct locks via their own LWT, bridged
 * (BLE-native) locks via the bridge's connect/disconnect relay onto
 * `.../presence` — both written by `handleLockPresence()` before this
 * runs. The old Thread-specific `isThread`/`thread_age_s` branch (dead
 * reckoning a specific lock's reachability from a Thread mesh child-table
 * age, since a Thread lock had no direct signal of its own) is gone along
 * with Thread support itself: `bridgePresence === 'offline'` still catches
 * "the whole bridge is down" as the stronger, bridge-level fault, and
 * `direct_presence === 'offline'` now uniformly catches "this specific
 * lock says it's unreachable," for bridged and direct locks alike.
 *
 * XF-146 (2026-09-01): reads `lock.direct_presence`, NOT `lock.presence` —
 * see the schema migration's comment for why sharing one column between
 * "raw signal" and "derived verdict" created a self-reinforcing stuck-
 * offline bug the moment a bridge had any transient blip unrelated to the
 * actual lock link.
 */
function computeFaultAttribution(lock, bridgePresence) {
  const isBridged = !!lock.bridge_id;

  if (isBridged && bridgePresence === 'offline') return 'bridge_offline';
  if (lock.direct_presence === 'offline') return isBridged ? 'lock_unreachable' : 'lock_offline_wifi';

  // "either" rows — only reached once the lock is confirmed reachable over
  // its own transport (the checks above). mcu_link_down sits above
  // battery_low deliberately (ozkey-20 §5a, added 2026-08-11 on firmware
  // review): a dead DL MCU link is total loss of function — no remote, no
  // PIN, no card, no fingerprint — where a low battery is still a working
  // lock. `=== 0` (not falsy) so a never-reported NULL doesn't trigger it.
  if (lock.mcu_link_up === 0) return 'mcu_link_down';
  if (lock.battery_pct != null && lock.battery_pct < BATTERY_LOW_PCT) return 'battery_low';
  if (lock.last_mech_result && lock.last_mech_result !== 'ok') return 'mech_fault';
  if (lock.pending_uplinks > 0) return 'pending_sync';
  return 'ok';
}

/** Re-derives `presence`/`presence_reason` for one lock from whatever raw
 * inputs are currently in its row + its bridge's row, and writes both.
 * Called after any event that could change the verdict (presence,
 * heartbeat, a bridged lock's own connect/disconnect relay).
 *
 * XF-138 P5 (2026-08-31): `presence` used to be re-DERIVED for any bridged
 * lock (the Thread case had no direct signal of its own to preserve — see
 * computeFaultAttribution's header). Now every lock, bridged or not, gets a
 * direct presence write from `handleLockPresence()` before this runs, so
 * this simply trusts that direct signal unless a stronger, independently-
 * computed fault (bridge_offline, etc.) overrides it to 'offline'.
 *
 * XF-146 (2026-09-01): falls back to `lock.direct_presence`, NOT
 * `lock.presence` — this function's own OUTPUT column must never also be
 * its input, or a transient bridge-level fault (which clears on its own
 * seconds later) permanently overwrites the real, still-healthy direct
 * signal with no way back short of a brand-new physical reconnect. See the
 * schema migration and `computeFaultAttribution()` for the full story —
 * found live on LockA, confirmed against firmware's own serial log
 * showing the actual BLE link never dropped at all.
 */
async function recomputeAndStorePresence(deviceId) {
  const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
  if (!lock) return;

  let bridgePresence = 'unknown';
  if (lock.bridge_id) {
    const [[bp]] = await pool.query('SELECT presence FROM bridges_presence WHERE bridge_id = ?', [
      lock.bridge_id,
    ]);
    bridgePresence = bp ? bp.presence : 'unknown';
  }

  const reason = computeFaultAttribution(lock, bridgePresence);
  const unreachable = reason === 'bridge_offline' || reason === 'lock_unreachable' || reason === 'lock_offline_wifi';
  const presence = unreachable ? 'offline' : lock.direct_presence;

  // XF-144/XF-152 (2026-09-03): XF-152's periodic MQTT unsubscribe/resubscribe
  // forces a redundant retained redelivery every 30s, re-triggering this
  // function for every lock behind a bridge even when nothing changed.
  // Unconditionally bumping presence_at on every no-op re-verdict silently
  // restarted XF-144's failover sweep's own 2-minute grace clock (keyed off
  // this exact column) every 30s, so it could mathematically never reach its
  // threshold — a lock whose only recovery path was the sweep got starved
  // indefinitely. Only advance presence_at on an actual state transition.
  if (lock.presence === presence && lock.presence_reason === reason) {
    return { presence, reason };
  }

  await pool.query('UPDATE locks SET presence = ?, presence_reason = ?, presence_at = NOW() WHERE id = ?', [
    presence,
    reason,
    deviceId,
  ]);
  return { presence, reason };
}

/** ozkey-20 R1 + ozkey-41: `locks/<id>/presence`. `reason` is one of
 * `lwt` (retained, broker-published Will), no-reason (retained, published by
 * the lock on every connect — §4.2), or a reset outcome — `factory_reset`
 * (retained, before the wipe), `factory_reset_denied` / `no_bond` (not
 * retained). Reset outcomes also carry `msg_id`, echoed from the request,
 * for `pendingLockResets` correlation (§5.3) — a Thread lock's outcome
 * arrives on this exact same topic, relayed verbatim by bridge32 (§2.1), so
 * no transport branching is needed here. */
async function handleLockPresence(deviceId, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on locks/${deviceId}/presence: "${payload.slice(0, 60)}"`);
    return;
  }
  const state = obj.state === 'online' || obj.state === 'offline' ? obj.state : 'unknown';

  // XF-144 (2026-09-02): reconciliation-on-connect. `via` (bridge's own
  // device_id, added to ozBuildLockPresence() this session) is stamped on
  // BOTH the online and offline publish for audit symmetry — Firmware's own
  // caveat, respected here: gate strictly on `state === 'online'`. A bridge
  // reporting `via` on an OFFLINE message is saying "I lost this lock," not
  // "I now own it" — treating that as a claim would misfire on every normal
  // disconnect. Closes the race found live tonight: a recovering bridge
  // silently reclaiming a lock the server had already failed over
  // elsewhere, with nobody telling the DB the ground truth changed.
  // Fetched unconditionally now (not just inside the online branch below) —
  // the new ownership gate a few lines down needs `bridge_id` for OFFLINE
  // `via` reports too (a non-owning bridge's own disconnect/lwt as it
  // complies with `release_lock`), not just online claims.
  const [[lock]] = await pool.query('SELECT bridge_id, presence, ozkie_version FROM locks WHERE id = ?', [
    deviceId,
  ]);
  let effectiveBridgeId = lock ? lock.bridge_id : null;

  // ozkey-48 §7: the bridge saw which GATT service the lock advertises
  // (…4b31 = v1, …4b32 = v2) — that beats the app's word at POST /pairings,
  // which an older app build never sends. Absent = the bridge has no opinion
  // (hasn't seen it advertise), NOT v1, so only a present 1|2 is applied.
  if (lock && (obj.ozkie_version === 1 || obj.ozkie_version === 2) && obj.ozkie_version !== lock.ozkie_version) {
    await pool.query('UPDATE locks SET ozkie_version = ? WHERE id = ?', [obj.ozkie_version, deviceId]);
    logEvent(
      'warn',
      `Lock ${deviceId} ozkie_version ${lock.ozkie_version} → ${obj.ozkie_version} per bridge ` +
        `${obj.via || '(unknown)'} scan — disagrees with the stored (app-reported) value (ozkey-48 §7)`
    );
  }

  if (obj.via && state === 'online') {
    if (lock && lock.bridge_id !== obj.via) {
      const oldBridgeId = lock.bridge_id;
      // XF-144 (2026-09-02, second pass): the naive "most recent connect
      // wins" version of this check caused a real, confirmed dual-BLE-slot
      // collision live tonight — a recovering Bridge 1 silently reclaimed
      // LockA/LockC from a healthy, actively-working Bridge 2 (both bridges
      // held real live connections simultaneously, verified via both
      // bridges' own logs, see XF-151). Firmware's proposed fix, built here:
      // gate on the LOCK's own current presence, not the bridge's general
      // health. `presence === 'online'` means the CURRENT bridge_id is
      // confirmed still working right now — a new claimant in that state is
      // racing a healthy assignment, not recovering a dead one, and gets
      // rejected + told to let go. `presence !== 'online'` (offline/unknown,
      // e.g. bridge_offline) means the current assignment is already
      // known-bad — this is the legitimate-failover case, unaffected,
      // same accept-and-reconcile behavior as before.
      //
      // Deliberately does NOT touch `PATCH /locks/:id` (manual "Ghép khoá
      // vào Bridge" re-pairing) — that's a separate, explicit, human-driven
      // code path that unconditionally overwrites `bridge_id` today and
      // still should; this gate only governs automatic reconciliation
      // triggered by a bridge's own presence report.
      if (lock.presence === 'online') {
        mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, obj.via), {
          op: 'release_lock',
          id: deviceId,
        });
        logEvent(
          'key',
          `XF-144 reconciliation REJECTED: lock ${deviceId} claim from ${obj.via} refused — ` +
            `current bridge ${oldBridgeId} still confirmed healthy (presence=online). ` +
            `release_lock sent to ${obj.via} to prevent a dual-connection collision.`
        );
        await recordAudit(
          null,
          deviceId,
          'bridge_reconcile_rejected',
          `claim from ${obj.via} rejected — ${oldBridgeId} still healthy; release_lock sent to ${obj.via}`
        );
      } else {
        await pool.query(
          'UPDATE locks SET bridge_id = ?, mcu_link_up = NULL, mcu_last_frame_s = NULL WHERE id = ?',
          [obj.via, deviceId]
        );
        await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
          deviceId,
          obj.via,
        ]);
        await pool.query(
          `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
             VALUES (?, ?, 0, NOW())
           ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
          [deviceId, obj.via]
        );
        logEvent(
          'key',
          `XF-144 reconciliation-on-connect: lock ${deviceId} bridge_id corrected from ` +
            `${oldBridgeId || '(none)'} to ${obj.via} (current bridge was not confirmed healthy)`
        );
        await recordAudit(
          null,
          deviceId,
          'bridge_reconcile',
          `bridge_id corrected to ${obj.via} via reconciliation-on-connect (was ${oldBridgeId || '(none)'})`
        );
        effectiveBridgeId = obj.via;
      }
    }
  }

  // XF-151 (2026-09-02): found live, real hardware — a rejected (or simply
  // non-owning) bridge's own report was still overwriting `direct_presence`
  // unconditionally below, poisoning the raw signal for a lock it doesn't
  // actually hold. Exact sequence that happened: Bridge 2's stale claim got
  // correctly REJECTED above (bridge_id stayed on the real owner, Bridge 1),
  // but the code still fell through and wrote `direct_presence='online'`
  // from Bridge 2's rejected message; moments later Bridge 2 complied with
  // `release_lock` and disconnected, publishing its OWN offline/lwt — which
  // ALSO got written unconditionally, flipping the lock to a false
  // stale-offline read even though Bridge 1 (the real, working owner) never
  // lost the connection. A remote unlock was refused as a result.
  //
  // Fix: only trust a presence report for the raw signal if it either
  // carries no `via` at all (Wi-Fi-direct locks, or firmware predating this
  // field — unconditional write preserved exactly as before, no regression)
  // or `via` matches the lock's EFFECTIVE bridge_id (the current owner,
  // already updated above if this message just legitimately won the
  // reconciliation). A report from any other bridge — rejected, or a
  // straggler from a bridge that was never the owner — is real signal
  // about THAT bridge, but not about this lock's actual reachability, and
  // must not overwrite it.
  // Only the raw-signal write is gated — `pendingLockResets` below still
  // runs regardless of which bridge relayed the message. A reset outcome is
  // a fact about the LOCK itself (device-initiated), not about a bridge's
  // reachability, and whoever currently holds the connection when a reset
  // fires is the one that relays it — that isn't necessarily the bridge_id
  // on record if it's already stale (the exact class of gap XF-150 found).
  // Not skipping it here would risk a real reset waiter hanging on a
  // timeout instead of resolving cleanly.
  const ownershipMismatch = !!(obj.via && lock && effectiveBridgeId && obj.via !== effectiveBridgeId);
  if (ownershipMismatch) {
    logEvent(
      'info',
      `Presence report for lock ${deviceId} from non-owning bridge ${obj.via} ` +
        `(current owner: ${effectiveBridgeId}) — ignored, raw signal not overwritten`
    );
  } else {
    // XF-119 §6/§9.3 ask 2: raw reason, not the derived R6 one — cleared to
    // NULL by the canonical online message (no `reason` field, ozpresence.h),
    // so this can never outlive a re-pair.
    //
    // XF-146 (2026-09-01): writes `direct_presence`, NOT the public
    // `presence` column — that one belongs exclusively to
    // `recomputeAndStorePresence()`'s derived verdict now. This is the raw
    // signal only; see that function and the schema migration for why
    // sharing one column between "what was directly reported" and "what we
    // last concluded" created a stuck-offline bug on a transient bridge
    // blip.
    await pool.query('UPDATE locks SET direct_presence = ?, last_reset_reason = ? WHERE id = ?', [
      state,
      obj.reason || null,
      deviceId,
    ]);
    const result = await recomputeAndStorePresence(deviceId);
    if (result)
      logEvent(
        'info',
        `Presence: lock ${deviceId} -> ${state}${obj.reason ? ` (${obj.reason})` : ''}, verdict=${result.reason}`
      );
  }

  // ozkey-41 §5.2: `no_bond` means the sender's request couldn't even be
  // decrypted — the lock is ALREADY unowned, which for a removal is the
  // desired end state, not a refusal. Resolve it the same as a confirmed
  // reset. `lwt` and a plain online/reconnect message are not reset
  // outcomes and are deliberately not resolved here.
  if (pendingLockResets.has(deviceId)) {
    if (obj.reason === 'factory_reset' || obj.reason === 'no_bond') {
      notifyLockResetWaiters(deviceId, obj.msg_id, 'reset_confirmed', 'presence_confirmed');
    } else if (obj.reason === 'factory_reset_denied') {
      notifyLockResetWaiters(deviceId, obj.msg_id, 'reset_denied', 'presence_denied');
    }
  }
}

/** ozkey-20 R1, bridge half. Same payload shape, `bridges_presence` instead
 * of `locks`. A bridge going offline is the aggregation trigger (R6
 * "mandatory") — every Thread lock behind it gets re-verdicted in one pass
 * rather than waiting for each lock's own next signal. */
// ozkey-P2 (courier rule, XF-84 — "the server knows it sent the message; it
// does not know the bridge executed it"): POST /bridges/:id/reset used to
// return ok:true the instant the MQTT publish succeeded, which reports the
// SERVER's success, not the BRIDGE's. bridge32's only wire-level signal for
// a successful reset is publishing `{"state":"offline","reason":
// "factory_reset"}` to its own presence topic immediately before wiping
// (bridge32.ino ~733-740, confirmed by reading the firmware — a DENIED
// reset, by contrast, only Serial.printf()s and returns; nothing is
// published, so denial cannot be distinguished from silence on the wire —
// see the route below). This map lets a pending REST request wait on that
// one real signal instead of guessing from the publish alone.
const pendingBridgeResets = new Map(); // bridgeId -> Set<(verdict) => void>

// ozkey-27 §9: last time we successfully published `utc` to a given bridge
// (ms, Date.now()), so a caller can tell "recently confirmed" from "stale"
// without a new timer.
const lastUtcPushAt = new Map(); // bridgeId -> ms

function pushUtcToBridge(bridgeId, why) {
  const nowUtc = Math.floor(Date.now() / 1000);
  mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), { utc: nowUtc });
  lastUtcPushAt.set(bridgeId, Date.now());
  logEvent('info', `Pushed utc=${nowUtc} to bridge ${bridgeId} (${why})`);
}

// XF-138 P5 (2026-08-31): `pushUtcToBridge()` used to get its ongoing
// refresh cadence for free by piggybacking on the Thread-liveness message
// each bridge sent periodically — removed along with Thread support (no
// lock uses it anymore, and that message was Thread-mesh-specific). A
// bridge still needs fresh time on an ongoing basis (it's what relays a
// connected lock's clock, per today's whole time-sync fix), so this is now
// its own explicit periodic sweep of every currently-online bridge, run on
// the same `timeRefreshTimer` cadence as the retained-time-topic refresh
// rather than a second independent timer.
async function refreshOnlineBridgeTimePushes() {
  const [bridges] = await pool.query("SELECT bridge_id FROM bridges_presence WHERE presence = 'online'");
  for (const { bridge_id } of bridges) {
    pushUtcToBridge(bridge_id, 'periodic refresh sweep, ozkey-27 §9');
  }
}

// XF-144 (2026-09-01, PM directive): grace period before a lock's assigned
// bridge is treated as failed enough to fail over. Deliberately NOT
// instant — the entire reason this feature exists (XF-146, same session)
// is that a transient ~9s bridge-level MQTT blip, completely unrelated to
// the actual BLE link, produced a false `bridge_offline` reading. Reacting
// to that instantly here would have started a real failover — connecting
// a backup bridge to a lock whose original bridge was fine seconds later
// — on the exact class of signal already proven unreliable this session.
// 2 minutes is comfortably longer than every transient blip observed
// tonight (the worst was ~17s of confusion end-to-end) while still being
// fast enough to matter for an actually-dead bridge.
// S0a.1: overridable so the regression suite can exercise a real failover
// in seconds; unset = the values above/below, unchanged.
const FAILOVER_GRACE_MS = Number(process.env.OZLOCK_FAILOVER_GRACE_MS) || 2 * 60 * 1000;
// Tick of the sweep timer armed in initMqtt() (it also drives the XF-152
// presence resubscribe).
const FAILOVER_SWEEP_MS = Number(process.env.OZLOCK_FAILOVER_SWEEP_MS) || 30_000;

/** Periodic sweep, same cadence as the time-refresh timer. For any lock
 * whose CURRENT bridge has read `bridge_offline` for at least
 * `FAILOVER_GRACE_MS` (a real, sustained outage — not a blip), looks up
 * the next-priority candidate in `lock_bridges` that is itself currently
 * online, and — only then, on-demand, never speculatively — promotes it
 * to be the new active `bridge_id`, exactly the same effect as a manual
 * `PATCH /locks/:id {bridge_id:...}` would have. Does nothing if no
 * online candidate exists — a lock with no working fallback just stays
 * `bridge_offline`, honestly, rather than routing to a bridge nobody has
 * confirmed can actually reach it.
 */
async function runBridgeFailoverSweep() {
  const [stuck] = await pool.query(
    `SELECT id, bridge_id FROM locks
      WHERE bridge_id IS NOT NULL AND presence_reason = 'bridge_offline'
        AND presence_at <= DATE_SUB(NOW(), INTERVAL ? SECOND)`,
    [FAILOVER_GRACE_MS / 1000]
  );
  for (const lock of stuck) {
    let [[next]] = await pool.query(
      `SELECT lb.bridge_id FROM lock_bridges lb
         JOIN bridges_presence bp ON bp.bridge_id = lb.bridge_id AND bp.presence = 'online'
        WHERE lb.lock_id = ? AND lb.bridge_id != ?
        ORDER BY lb.priority ASC
        LIMIT 1`,
      [lock.id, lock.bridge_id]
    );
    let viaScanConfirm = false;
    // XF-144 (2026-09-02): a lock that has only ever lived on one bridge has
    // no `lock_bridges` candidate row at all — confirmed live (LockB) to get
    // stuck here forever otherwise, even with other healthy bridges in range
    // that can actually see it. Fall back to a live scan-confirm before
    // giving up; still on-demand only, no speculative hot standbys.
    if (!next) {
      const { results } = await scanForLockAcrossBridges(lock.id);
      const candidates = results
        .filter((r) => r.found && r.bridge_id !== lock.bridge_id)
        .sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
      if (candidates.length) {
        next = { bridge_id: candidates[0].bridge_id };
        viaScanConfirm = true;
      }
    }
    if (!next) continue; // no confirmed-reachable fallback candidate — nothing safe to do
    const oldBridgeId = lock.bridge_id;
    await pool.query(
      `UPDATE locks SET bridge_id = ?, mcu_link_up = NULL, mcu_last_frame_s = NULL,
         direct_presence = 'unknown', presence = 'unknown', presence_reason = NULL
       WHERE id = ?`,
      [next.bridge_id, lock.id]
    );
    // Same demote-then-promote shape PATCH /locks/:id already uses (XF-138
    // P5) — without this, the old bridge is left stuck at priority 0
    // alongside the new one (found live on real hardware, XF-144
    // 2026-09-02: a real auto-failover left BOTH ozb-98a316a7e638 and
    // ozb-206ef116f390 at priority 0 for LockA), and a future sweep's
    // `ORDER BY priority ASC LIMIT 1` could arbitrarily re-pick the
    // already-failed bridge on a tie.
    await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
      lock.id,
      next.bridge_id,
    ]);
    await pool.query(
      `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
         VALUES (?, ?, 0, NOW())
       ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
      [lock.id, next.bridge_id]
    );
    // XF-144 (2026-09-02): the DB write above updates routing INTENT only —
    // Firmware confirmed the new bridge never actually adopts the lock
    // without this, so a "successful" failover was reachable-looking
    // (caps still said remote_unlock:true) while genuinely broken
    // (forwardOverBle() requires the lock in the bridge's own g_bleLocks
    // allowlist first). Same shape as unpair_lock (XF-147): fire-and-
    // forget, no result topic — the real confirmation is the subsequent
    // BLE connect + presence-online event this sweep/server already
    // watches for, same as a manual "Ghép khoá vào Bridge" pairing.
    mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, next.bridge_id), {
      op: 'pair_lock',
      id: lock.id,
    });
    logEvent(
      'key',
      `XF-144 auto-failover: lock ${lock.id} moved from bridge ${oldBridgeId} ` +
        `(bridge_offline ${FAILOVER_GRACE_MS / 1000}s+) to bridge ${next.bridge_id}, ` +
        `pair_lock dispatched${viaScanConfirm ? ' (via scan-confirm, no lock_bridges history)' : ''}`
    );
    await recordAudit(
      null,
      lock.id,
      'bridge_failover',
      `auto-failover from ${oldBridgeId} to ${next.bridge_id} after sustained bridge_offline (pair_lock dispatched` +
        `${viaScanConfirm ? ', candidate found via scan-confirm — no prior lock_bridges history)' : ')'}`
    );
  }
}

function waitForBridgeResetVerdict(bridgeId, timeoutMs) {
  return new Promise((resolve) => {
    const settle = (verdict, cause) => {
      clearTimeout(timer);
      const set = pendingBridgeResets.get(bridgeId);
      if (set) {
        set.delete(settle);
        if (!set.size) pendingBridgeResets.delete(bridgeId);
      }
      resolve({ verdict, cause });
    };
    // `cause` distinguishes "we waited the full budget and heard nothing"
    // from "the bridge went offline for an unrelated/ambiguous reason and
    // we stopped waiting early" — both resolve 'unknown', but conflating
    // the two in a log line would be exactly the kind of imprecision this
    // fix exists to remove.
    const timer = setTimeout(() => settle('unknown', 'timeout'), timeoutMs);
    if (!pendingBridgeResets.has(bridgeId)) pendingBridgeResets.set(bridgeId, new Set());
    pendingBridgeResets.get(bridgeId).add(settle);
  });
}

/** XF-144 (2026-09-01): a lock has no opinion on which bridge is "best" for
 * it — only a bridge's own targeted BLE scan measures real bridge-to-lock
 * link quality (RSSI), which is what routing should actually be based on,
 * per the operator's reasoning for this feature (replacing the phone's own
 * BLE-scan RSSI ranking, which only ever measured "can the PHONE hear the
 * bridge"). Keyed by the TARGET device_id being scanned for, not by
 * bridge_id — several bridges each report back about the same target, and
 * every response arriving inside the window is collected, not just the
 * first, so the caller can compare all candidates that actually answered.
 */
const pendingLockScans = new Map(); // deviceId -> Array<{bridge_id, found, rssi}>

function handleBridgeScanResult(bridgeId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${bridgeId}/scan_result: "${payloadStr.slice(0, 60)}"`);
    return;
  }
  const deviceId = typeof obj.target === 'string' ? obj.target : null;
  if (!deviceId) return;
  const pending = pendingLockScans.get(deviceId);
  if (!pending) {
    // Not an error — a bridge answering after the window closed (or a scan
    // nobody requested) is expected under real network timing, not a bug.
    logEvent(
      'info',
      `Scan result from bridge ${bridgeId} for ${deviceId} arrived with no pending request — dropped`
    );
    return;
  }
  pending.push({
    bridge_id: bridgeId,
    found: obj.found === true,
    rssi: Number.isFinite(obj.rssi) ? obj.rssi : null,
  });
}

// XF-144 (2026-09-01, post-live-verification): a real bridge's scan_result
// consistently arrived ~150-170ms AFTER a bare `timeoutMs`-length wait here —
// confirmed via `/api/events` timestamps against BOTH real bench bridges,
// twice, not a one-off ("... arrived with no pending request — dropped").
// The requested `duration_s` is a scan-hardware budget on the bridge's side;
// it does not include the MQTT publish round-trip or the bridge's own
// processing before it publishes. Waiting exactly `timeoutMs` here raced
// every real response and silently dropped 100% of them — this margin is
// the fix, not a new discovery of "responses are slow."
const SCAN_RESULT_GRACE_MS = 3000;

/** Broadcasts a targeted scan request to every currently-online bridge,
 * waits `timeoutMs` (plus SCAN_RESULT_GRACE_MS network/processing margin)
 * for whatever `scan_result` responses arrive, and returns all of them (not
 * just the best) so the caller applies its own ranking. Verified live
 * against real firmware (`bridgeBLE-0.34`) on both bench bridges as of
 * 2026-09-01 — see XF-144 for the dropped-response bug this margin fixes.
 */
async function scanForLockAcrossBridges(deviceId, timeoutMs = 8000) {
  const [bridges] = await pool.query("SELECT bridge_id FROM bridges_presence WHERE presence = 'online'");
  if (!bridges.length) return { requested: 0, results: [] };

  pendingLockScans.set(deviceId, []);
  for (const { bridge_id } of bridges) {
    mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridge_id), {
      op: 'scan',
      target: deviceId,
      duration_s: Math.ceil(timeoutMs / 1000),
    });
  }
  await new Promise((resolve) => setTimeout(resolve, timeoutMs + SCAN_RESULT_GRACE_MS));
  const results = pendingLockScans.get(deviceId) || [];
  pendingLockScans.delete(deviceId);
  return { requested: bridges.length, results };
}

function notifyBridgeResetWaiters(bridgeId, verdict, cause) {
  const set = pendingBridgeResets.get(bridgeId);
  if (!set) return;
  for (const settle of Array.from(set)) settle(verdict, cause);
}

// ozkey-41 §5: the lock sibling of pendingBridgeResets, built once firmware
// actually shipped a wire-level ack for locks (doorlock-1.96/bridge32-1.40 —
// `locks/<id>/presence`, `reason` one of `factory_reset`/
// `factory_reset_denied`/`no_bond`). Unlike the bridge map, a lock waiter
// also carries the request's `msg_id` — §5.3: a real ack and the app's own
// 3-minute escape hatch both end with the entry disappearing, and `msg_id`
// is the only thing on the wire that tells them apart, so a waiter must only
// settle on a matching id.
const pendingLockResets = new Map(); // deviceId -> Set<{ msgId, settle }>

function waitForLockResetVerdict(deviceId, msgId, timeoutMs) {
  return new Promise((resolve) => {
    const waiter = {
      msgId,
      settle: (verdict, cause) => {
        clearTimeout(timer);
        const set = pendingLockResets.get(deviceId);
        if (set) {
          set.delete(waiter);
          if (!set.size) pendingLockResets.delete(deviceId);
        }
        resolve({ verdict, cause });
      },
    };
    const timer = setTimeout(() => waiter.settle('unknown', 'timeout'), timeoutMs);
    if (!pendingLockResets.has(deviceId)) pendingLockResets.set(deviceId, new Set());
    pendingLockResets.get(deviceId).add(waiter);
  });
}

function notifyLockResetWaiters(deviceId, msgId, verdict, cause) {
  const set = pendingLockResets.get(deviceId);
  if (!set) return;
  // XF-115 §3: today every Thread-relayed reset outcome arrives with no
  // msg_id at all (fixed once firmware ships bridge32/doorlock carrying it
  // through — see ozkey-41 §10 discussion). An id-less message can't be
  // correlated by id, so fall back to device-only matching — but ONLY when
  // there is exactly one waiter, since that's the sole case with nothing to
  // disambiguate. With 2+ concurrent waiters for the same device (e.g. a
  // rapid double DELETE), an id-less message is genuinely ambiguous and
  // must settle none of them rather than guessing and resolving the wrong
  // one.
  if (!msgId) {
    if (set.size === 1) for (const waiter of set) waiter.settle(verdict, cause);
    return;
  }
  for (const waiter of Array.from(set)) {
    if (waiter.msgId && waiter.msgId !== msgId) continue;
    waiter.settle(verdict, cause);
  }
}

// XF-152 (2026-09-02): mqttClient.on('message', async ...) fires one
// listener invocation per incoming packet without awaiting the previous
// one — no per-bridge ordering guarantee. A rapid offline/online flap pair
// for the SAME bridge (confirmed live: 11+ brief MQTT reconnect cycles on
// one bridge in a single session) can have their handleBridgePresenceImpl()
// DB writes complete out of delivery order, leaving `bridges_presence`
// stuck on the stale value even though the bridge is genuinely healthy —
// confirmed live twice: once via a stuck sweep candidate, once via
// GET /bridges showing a healthy bridge as offline for 4.5+ minutes.
// Fix: chain every call for a given bridgeId onto the tail of that
// bridge's own promise queue, so overlapping calls process strictly in
// arrival (enqueue) order instead of racing on the event loop. The
// `.catch(() => {})` keeps one failed call from wedging the queue for
// every future presence report on that bridge.
const bridgePresenceQueues = new Map(); // bridgeId -> Promise (tail of chain)

function handleBridgePresence(bridgeId, payload) {
  const prev = bridgePresenceQueues.get(bridgeId) || Promise.resolve();
  const next = prev.catch(() => {}).then(() => handleBridgePresenceImpl(bridgeId, payload));
  bridgePresenceQueues.set(bridgeId, next);
  return next;
}

async function handleBridgePresenceImpl(bridgeId, payload) {
  let obj;
  try {
    obj = JSON.parse(payload);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${bridgeId}/presence: "${payload.slice(0, 60)}"`);
    return;
  }
  const state = obj.state === 'online' || obj.state === 'offline' ? obj.state : 'unknown';
  await pool.query(
    `INSERT INTO bridges_presence (bridge_id, presence, presence_at) VALUES (?, ?, NOW())
     ON DUPLICATE KEY UPDATE presence = VALUES(presence), presence_at = VALUES(presence_at)`,
    [bridgeId, state]
  );
  logEvent('info', `Presence: bridge ${bridgeId} -> ${state}${obj.reason ? ` (${obj.reason})` : ''}`);

  // ozkey-P2: resolve any REST call currently waiting on this bridge's reset
  // outcome. `reason==='factory_reset'` is the one positive signal that
  // exists; going offline for any OTHER reason while a reset is pending
  // (dropped connection, unrelated reboot, ...) is genuinely ambiguous —
  // could be mid-reset, could be nothing to do with it — so it resolves as
  // 'unknown' immediately rather than making the caller wait out the full
  // timeout for a state that already can't get more informative.
  // ozkey-25 §5.2/§5.3: firmware's `bridge32-1.34` adds a real denial
  // signal — {"state":"online",...,"reason":"factory_reset_denied"},
  // deliberately NOT retained (a refusal is an event, not a liveness
  // state; see their reply for why retaining it would be a bug). That's
  // why this checks `reason` on BOTH online and offline, not just offline
  // as before — the denial arrives on the opposite state from a confirm.
  if (pendingBridgeResets.has(bridgeId)) {
    if (state === 'offline' && obj.reason === 'factory_reset') {
      notifyBridgeResetWaiters(bridgeId, 'reset_confirmed', 'presence_confirmed');
    } else if (state === 'online' && obj.reason === 'factory_reset_denied') {
      notifyBridgeResetWaiters(bridgeId, 'reset_denied', 'presence_denied');
    } else if (state === 'offline') {
      notifyBridgeResetWaiters(bridgeId, 'unknown', 'offline_unrelated_reason');
    }
    // else: online with no matching reason (e.g. a routine reconnect) —
    // not informative either way, keep waiting for the real timeout.
  }

  // ozkey-20 §23.1 (2026-08-12, firmware's live-test finding): UDP 123 is
  // blocked on this network (see the NTP memory note), so a freshly
  // connected bridge has no clock until something injects one. `bridge32`
  // >=1.20 already accepts `{"utc": <unix_seconds>}` on its own command
  // topic — this was previously a manual bench injection; push it
  // proactively the moment we observe the bridge online rather than
  // waiting to be asked, same "use the mechanism that already exists"
  // discipline as everything else in this document.
  if (state === 'online') {
    pushUtcToBridge(bridgeId, 'presence online, ozkey-20 §23.1');

    // ozkey-23 §10.2a: mint once, push once — same "v1 plaintext ack, bench
    // only" posture handleEnroll() already uses for locks (the broker
    // doesn't enforce credentials yet, so an unauthenticated publish is how
    // the credential-bearing publish itself gets delivered the first time).
    // Firmware persists to NVS and presents on future connects; we don't
    // resend to an already-provisioned bridge.
    const bridgeCreds = await getOrMintBridgeCredentials(bridgeId);
    if (bridgeCreds.minted) {
      mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), {
        op: 'broker_credentials',
        broker_username: bridgeCreds.broker_username,
        broker_secret: bridgeCreds.broker_secret,
      });
      logEvent('pair', `Minted + pushed broker credentials to bridge ${bridgeId} (first sighting)`);
    }
  }

  // Aggregation: re-verdict every lock behind this bridge in one pass, so
  // "bridge offline" produces one log line here and N correct per-lock
  // verdicts, not N separate discoveries on each lock's own next report.
  const [locks] = await pool.query('SELECT id FROM locks WHERE bridge_id = ?', [bridgeId]);
  for (const { id } of locks) await recomputeAndStorePresence(id);
  if (locks.length)
    logEvent('info', `Presence: re-verdicted ${locks.length} lock(s) behind bridge ${bridgeId}`);
}

/** XF-144 Phase 2 (2026-09-02, PM-approved): a Thread-detecting bridge's
 * fast-path report that a DIFFERENT bridge (`peer`, in the payload — NOT
 * `reportingBridgeId`, the topic's own wildcard) has gone dark, ~45-60s
 * after last heard rather than waiting out MQTT's own LWT+timeout chain
 * (typically ~2 minutes). Deliberately just a thin wrapper around the
 * SAME `handleBridgePresence()` an LWT-driven offline already calls — zero
 * new state machine, reuses every downstream effect that function already
 * has (writes `bridges_presence`, re-verdicts every lock behind that
 * bridge) for free. This is the whole point of Phase 2's fast-path fix:
 * once `bridges_presence` reflects reality this early, the Phase-1 reject-
 * gate (`handleLockPresence()`'s `via` check) naturally sees an accurate
 * `lock.presence` by the time the Thread-triggered takeover's own claim
 * arrives, instead of incorrectly refusing it as "current bridge still
 * healthy" against stale data.
 *
 * A false positive (Thread mesh partition, not an actual dead bridge) is
 * self-limiting, not a lasting corruption — `bridges_presence` gets
 * overwritten by any subsequent REAL report from that bridge, same
 * self-correcting shape every other presence signal in this system
 * already has.
 */
async function handlePeerStatus(reportingBridgeId, payloadStr) {
  let obj;
  try {
    obj = JSON.parse(payloadStr);
  } catch (_) {
    logEvent('warn', `Non-JSON payload on bridges/${reportingBridgeId}/peer_status: "${payloadStr.slice(0, 60)}"`);
    return;
  }
  if (obj.event !== 'peer_down' || !obj.peer) return;
  logEvent(
    'key',
    `XF-144 peer_down: bridge ${reportingBridgeId} reports peer ${obj.peer} down — fast-tracking bridges_presence`
  );
  await handleBridgePresence(obj.peer, JSON.stringify({ state: 'offline', reason: 'peer_down' }));
}

/**
 * Lock's first broker contact (XF-42 §13.2 step 3). Token-free: the device_id
 * announced here must match a pairing the app already registered (§13.2 step 2).
 * Knowing the random device_id IS the bearer proof — no server credential. The
 * server binds the lock's MAC + broker creds to the pre-registered pairing.
 */
async function handleEnroll(siteId, topicDeviceId, obj) {
  const mac = normalizeMac(obj.mac);
  const deviceId = String(obj.device_id || topicDeviceId);

  if (!mac) {
    logEvent('warn', `Enroll from ${deviceId} missing mac — ignored`);
    return;
  }

  const [[row]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
  const fail = async (reason) => {
    logEvent('error', `ENROLL REJECTED for ${deviceId} (${mac}) — ${reason}`);
    mqttPublish(CONFIG.topicCommand(siteId, deviceId), {
      op: 'enrollment_nack',
      device_id: deviceId,
      error: reason,
    });
  };

  // No pre-registered pairing → the app must POST /pairings first (§13.2).
  if (!row) return fail('no pairing registered for this device_id — app must register it first');

  const brokerUsername = deviceId;
  const brokerSecret = makeSecret(16, 'ozl_');
  const label = row.label && row.label !== 'New Doorlock' ? row.label : `Doorlock ${mac.slice(-5)}`;
  const appId = row.app_id || (obj.app_id ? String(obj.app_id).slice(0, 80) : null);

  // bridge_id = NULL here is deliberate (2026-08-03). Reaching handleEnroll means
  // the LOCK ITSELF spoke MQTT — which only a direct Wi-Fi lock can do; a Thread
  // lock behind a bridge has no uplink and never enrolls. So a self-enroll is proof
  // the lock is NOT bridged. Found by converting a Thread lock to Wi-Fi (T2-on-WiFi,
  // ozprov.py): the row kept its old bridge_id, and BOTH the caps inference
  // (effectiveCaps) and the app's transport routing (XF-55 §9.1) key off bridge_id,
  // so the now-WiFi lock was still classified Thread-behind-bridge and its unlock was
  // routed to the bridge topic — which no longer reaches it. A device that transitions
  // transports must not carry the old transport's binding.
  // XF-57 (AN): record what the lock says it IS, alongside the binding reset
  // above. Same COALESCE rule as the heartbeat — a pre-XF-57 firmware omits both
  // fields and must not blank them.
  const ident = deviceIdentity(obj);
  // XF-122 §5 ask 3. COALESCE, not a bare overwrite: a pre-2.14 firmware
  // enrolling (or re-enrolling) sends neither field, and that must leave
  // whatever this row already knows untouched rather than blanking it —
  // same discipline as transport/caps above, for the same reason.
  const tuyaPid = typeof obj.tuya_pid === 'string' && obj.tuya_pid.length <= 32 ? obj.tuya_pid : null;
  const profile = typeof obj.profile === 'string' && obj.profile.length <= 64 ? obj.profile : null;
  await pool.query(
    `UPDATE locks SET app_id = ?, mac = ?, label = ?, fw = ?, status = 'enrolled',
       bridge_id = NULL,
       transport = COALESCE(?, transport), caps = COALESCE(?, caps),
       tuya_pid = COALESCE(?, tuya_pid), profile = COALESCE(?, profile),
       heartbeat_s = COALESCE(heartbeat_s, ?), broker_username = ?, broker_secret = ?, last_seen_at = NOW()
     WHERE id = ?`,
    [appId, mac, label, obj.fw || null, ident.transport, ident.caps, tuyaPid, profile,
     CONFIG.DEFAULT_HEARTBEAT_S, brokerUsername, brokerSecret, deviceId]
  );

  // v1 plaintext ack — bench only; production wraps this in the ozkey-04 §8
  // ECDH session and the broker enforces the credential.
  mqttPublish(CONFIG.topicCommand(siteId, deviceId), {
    op: 'enrollment_ack',
    device_id: deviceId,
    site_id: siteId,
    app_id: appId,
    label,
    broker_username: brokerUsername,
    broker_secret: brokerSecret,
    heartbeat_s: CONFIG.DEFAULT_HEARTBEAT_S,
    issued_by: 'OZLOCK/',
  });

  logEvent('pair', `ENROLLED ${deviceId} (${mac}) -> site '${siteId}' as "${label}", paired to app ${appId || '(anon)'}`);
}

/* -- Locks (the owner's fleet) ----------------------------------------------- */
/** XF-144 (2026-09-02, PM-approved): "does this lock have a confirmed-online
 * fallback bridge on record" — deterministic and cheap (`lock_bridges` +
 * `bridges_presence` only), not scan-confirmed (PM's own call: a live scan
 * per `GET /locks` call is too heavy for a list endpoint). Lets the app show
 * a "no backup bridge" warning without guessing reachability itself.
 */
async function backupAvailableSet(lockIds = null) {
  const [rows] = await pool.query(
    `SELECT DISTINCT lb.lock_id
       FROM lock_bridges lb
       JOIN bridges_presence bp ON bp.bridge_id = lb.bridge_id AND bp.presence = 'online'
      WHERE lb.priority > 0` + (lockIds ? ' AND lb.lock_id IN (?)' : ''),
    lockIds ? [lockIds] : []
  );
  return new Set(rows.map((r) => r.lock_id));
}

// XF-66 (BH): "delivered" is not derivable from MQTT (QoS 1 only confirms the
// broker got it, never the lock), so this is a best-effort heuristic, not a
// guarantee — labeled honestly as `likely_delivered`, matching UnlockResult's
// own "we know it left, not that the door moved" property. 2.5x heartbeat_s
// grace: a lock naps between heartbeats by design, so "no heartbeat since
// last interval" alone would call a perfectly healthy lock offline.
const RESET_DELIVERY_GRACE = 2.5;
function likelyOnline(lastSeenAt, heartbeatS) {
  if (!lastSeenAt) return false;
  const graceMs = (heartbeatS || CONFIG.DEFAULT_HEARTBEAT_S) * 1000 * RESET_DELIVERY_GRACE;
  return Date.now() - new Date(lastSeenAt).getTime() < graceMs;
}

/**
 * XF-57 (AN). Pull the lock's self-reported identity out of an enroll/heartbeat
 * payload, for storage. Returns `{transport, caps}` where each is either a value
 * to write or NULL meaning "the device did not say" — callers COALESCE, so a
 * pre-XF-57 firmware (doorlock <= 1.5) never blanks a column it simply omits.
 *
 * Validated, not trusted: `transport` must be one of the two we know, and `caps`
 * must be an array of known strings. A device is authoritative about itself, not
 * about our schema — and this arrives over an MQTT broker that is anonymous-open
 * on the bench, so anything unrecognised is dropped rather than stored.
 */
// XF-138 P5 (2026-08-31): CH584M locks are BLE-native and self-report
// transport:"ble" (firmware bridgeBLE-0.20). Thread support is fully
// retired — no lock uses it any more — so `'thread'` is no longer a known
// value here; a stray legacy report of it is treated the same as any other
// unrecognised value (dropped to NULL, not stored) rather than accepted.
const KNOWN_TRANSPORTS = ['wifi', 'ble'];
// XF-138/139 P5 (2026-08-31): `cred_pin` deliberately NOT here — firmware
// confirmed it never sends that literal string, only `pin_sync`
// (`if (canPin) caps.add("pin_sync")`, the actual source). `cred_pin` is
// the app's own name for the same fact (`directory_client.dart:268/273`),
// translated at the `effectiveCaps()` response layer, not accepted here as
// if the device might say it — this list is meant to reflect what firmware
// can actually report on the wire. `rfid_sync` alongside `pin_sync` for the
// same reason — `if (canRfid) caps.add("rfid_sync")`, same source. Missing
// it caused a real live bug: `deviceIdentity()`'s filter requires EVERY
// reported value to be recognised or the whole array is dropped as "nothing
// said" (its own documented all-or-nothing behavior) — a `pin_sync`-only
// whitelist silently rejected a real, otherwise-valid report the moment
// firmware's actual payload included `rfid_sync` too. Caught live before
// firmware's own flash-and-verify pass, not after.
const KNOWN_CAPS = ['remote_unlock', 'assisted_unlock', 'pin_sync', 'rfid_sync', 'audit'];

function deviceIdentity(obj) {
  const t = typeof obj.transport === 'string' ? obj.transport.toLowerCase() : null;
  const transport = KNOWN_TRANSPORTS.includes(t) ? t : null;

  let caps = null;
  if (Array.isArray(obj.caps)) {
    const clean = obj.caps.filter((c) => KNOWN_CAPS.includes(c));
    // An empty array after filtering is still a real statement ("I can do none
    // of these"), but an all-junk report is not — treat it as nothing said.
    if (clean.length === obj.caps.length) caps = JSON.stringify(clean);
  }
  return { transport, caps };
}

function effectiveCaps(lock) {
  // XF-58 (AS) — `bridge_id` is CONFIGURATION: is a bridge bound. Liveness must
  // NEVER enter this function. Two reasons, and the second is the stronger:
  //   1. remote_unlock would blink out during a bridge reboot, and the app would
  //      send the user to walk to a door that would have opened in a second.
  //   2. `caps` is now partly device-reported and CACHED in the app's local row
  //      (XF-57). A field worth caching must be stable; mixing a by-the-second
  //      signal in would make that cache wrong by design — XF-57's staleness bug
  //      again, but unfixable, because the truth would change faster than sync.
  // "Bound but currently unreachable" belongs in a SEPARATE `bridge_state` field
  // (live / stale / unknown), deferred to the (AC) bridges table. Do not reach
  // for a liveness signal here to "improve" the answer.
  const bridged = !!lock.bridge_id;

  // What we can deduce with no word from the device. Note `assisted_unlock` is
  // NOT here, and cannot be — see the rule below.
  //
  // XF-128 Ask 4 (firmware, 2026-08-22): `pin_sync` used to be asserted here
  // too, for ANY enrolled lock, device caps or not. Firmware found the real
  // failure mode this enables: on `tuya-luona-ds013-t3` the credential DP
  // (16, `bulk_password_add`) is `reserved` — the supplier never gave us its
  // payload layout — so PIN provisioning is not implementable on this
  // product AT ALL today, by any layer. Asserting `pin_sync` here is exactly
  // "with no word from the device," which is what let the app offer PIN
  // entry, seal a legacy DP-21 frame, and silently corrupt
  // `navigation_volume` while reporting success (XF-128 §1-4). Dropped —
  // now the same rule §2990 already states for `assisted_unlock`: a
  // capability whose enforcement/support lives in firmware is only ever
  // device-reported, never inferred. No shipped profile currently
  // advertises `pin_sync` (ozkey session-state, 2026-08-21: "every
  // credential DP is reserved"), so this now correctly reads `false`
  // everywhere until a real DP layout exists to back it.
  const inferred = bridged ? ['remote_unlock', 'audit'] : ['audit'];

  let device = null;
  if (lock.caps) {
    try {
      const c = JSON.parse(lock.caps);
      if (Array.isArray(c)) device = c;
    } catch (_) {
      // Malformed device report: fall through to inference rather than trust it.
    }
  }
  if (!device) return { caps: inferred, source: 'inferred' };

  const caps = [];

  // XF-57 — remote_unlock needs BOTH sides to agree, because each knows half:
  //   • the device knows its transport — a Wi-Fi lock sleeps and can never do
  //     remote unlock, whatever the deployment looks like;
  //   • the server knows whether a bridge is actually bound — a Thread lock is
  //     only remotely reachable THROUGH one, and the lock cannot tell whether
  //     its bridge is alive or was removed yesterday.
  // Trusting the device alone would mirror the bug XF-57 fixed: a Thread lock
  // whose bridge is gone would keep claiming remote_unlock and every attempt
  // would queue and expire unheard.
  if (device.includes('remote_unlock') && bridged) caps.push('remote_unlock');

  // XF-58 — assisted_unlock is DEVICE-ONLY and must NEVER be inferred.
  //
  // This is a safety interlock, not a modelling nicety. The whole guarantee of an
  // assisted unlock — "the door opens only if somebody is standing at it" — is
  // enforced in FIRMWARE (doorlock-1.5 refuses the command without a recent
  // keypad touch). A lock running anything older ignores `action` entirely and
  // forwards the frame straight to the MCU, so granting the capability by
  // inference would hand an unenforced unlock to exactly the firmware that cannot
  // enforce it. Only a device new enough to report caps at all is new enough to
  // honour them, so the report IS the version check.
  //
  // General rule worth keeping: a capability whose enforcement lives in firmware
  // may only ever be device-reported. Never infer one.
  if (device.includes('assisted_unlock')) caps.push('assisted_unlock');

  for (const cap of ['pin_sync', 'audit']) if (device.includes(cap)) caps.push(cap);

  // XF-138/139 P5 (2026-08-31, firmware's correction): `cred_pin` is NOT a
  // name firmware has ever sent or ever will — the device only ever reports
  // `pin_sync` (`if (canPin) caps.add("pin_sync")`, confirmed against the
  // actual source, not assumed). `cred_pin` exists ONLY in the app's own
  // gate (`directory_client.dart:268/273`, `banoi_doorlock.dart:5700`) —
  // the two names were never the same field, just two different teams'
  // words for the same fact, and nothing before this relay ever put them
  // in the same room to notice. Translating here, not in storage: `caps`
  // above stays the raw, unmolested truth of what the device said (`caps`
  // storage is meant to be auditable device fact, not a translation), and
  // this is the one place — building the API response — where the two
  // vocabularies actually need to agree. No RFID-side translation yet:
  // firmware confirmed `cred_pin`'s existence but explicitly could not
  // confirm an app-facing name for RFID's equivalent — guessing one here
  // would just be a third instance of this exact bug, not a fix.
  if (device.includes('pin_sync')) caps.push('cred_pin');

  return { caps, source: 'device' };
}

module.exports = {
  getOrMintBridgeCredentials,
  eventsAtRisk,
  eventsPending,
  eventsAtRiskLogged,
  BATTERY_LOW_PCT,
  computeFaultAttribution,
  recomputeAndStorePresence,
  handleLockPresence,
  pendingBridgeResets,
  lastUtcPushAt,
  pushUtcToBridge,
  refreshOnlineBridgeTimePushes,
  FAILOVER_GRACE_MS,
  FAILOVER_SWEEP_MS,
  runBridgeFailoverSweep,
  waitForBridgeResetVerdict,
  pendingLockScans,
  handleBridgeScanResult,
  SCAN_RESULT_GRACE_MS,
  scanForLockAcrossBridges,
  notifyBridgeResetWaiters,
  pendingLockResets,
  waitForLockResetVerdict,
  notifyLockResetWaiters,
  bridgePresenceQueues,
  handleBridgePresence,
  handleBridgePresenceImpl,
  handlePeerStatus,
  handleEnroll,
  backupAvailableSet,
  RESET_DELIVERY_GRACE,
  likelyOnline,
  KNOWN_TRANSPORTS,
  KNOWN_CAPS,
  deviceIdentity,
  effectiveCaps,
};
