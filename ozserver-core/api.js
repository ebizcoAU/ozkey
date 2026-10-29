'use strict';
/*
 * ozserver-core/api.js — the lock-side REST API: pairing, the lock/bridge
 * registry reads, settings, sealed grants/unlock/bond verbs, bridge reset,
 * lock removal, the write queue and the lab wake hook.
 *
 * Moved verbatim from ozlock/server/server.js in S1b (ozkey-50 §7); comments
 * keep their original references. The route bodies below register on a
 * collector, not on a router: a profile calls mountLockApi(router, hooks) to
 * put them on its own express Router, with its auth in front of every one.
 * The profile keeps its own /health, /events and anything product-specific.
 */

const { ctx, lazy } = require('./ctx');
const CONFIG = lazy(() => ctx.CONFIG);
const pool = lazy(() => ctx.pool);
const logEvent = (...args) => ctx.logEvent(...args);

// From other core modules.
const makeSecret = (...args) => require('./util').makeSecret(...args);
const recordAudit = (...args) => require('./audit').recordAudit(...args);
const refuseIfOzkieV2 = (...args) => require('./relay').refuseIfOzkieV2(...args);
const flushQueueForDevice = (...args) => require('./queue').flushQueueForDevice(...args);
const eventsAtRisk = (...args) => require('./registry').eventsAtRisk(...args);
const eventsPending = (...args) => require('./registry').eventsPending(...args);
const waitForBridgeResetVerdict = (...args) => require('./registry').waitForBridgeResetVerdict(...args);
const scanForLockAcrossBridges = (...args) => require('./registry').scanForLockAcrossBridges(...args);
const waitForLockResetVerdict = (...args) => require('./registry').waitForLockResetVerdict(...args);
const backupAvailableSet = (...args) => require('./registry').backupAvailableSet(...args);
const likelyOnline = (...args) => require('./registry').likelyOnline(...args);
const effectiveCaps = (...args) => require('./registry').effectiveCaps(...args);
const mqttPublish = (...args) => require('./transport').mqttPublish(...args);
const { OZKIE_V2 } = require('./relay');
const { eventsAtRiskLogged } = require('./registry');

const SUPPORTED_CRED_TYPES = ['pin', 'rfid'];

/** The profile's hooks, set by mountLockApi(). */
let hooks = {};

/** Routes register here at load; mountLockApi() replays them onto a router. */
const routes = [];
const api = {};
for (const method of ['get', 'post', 'patch', 'delete']) {
  api[method] = (path, ...handlers) => routes.push({ method, path, handlers });
}

function guardDb(res) {
  if (!ctx.pool) {
    res.status(503).json({ ok: false, error: 'Database not ready' });
    return false;
  }
  return true;
}

/** ozkey-04 §5 provision payload — the app writes it over BLE; lab pastes it
 *  into LockSim (transport equivalence, ozkey-04 §2.2). No enrollment_token:
 *  under trust-model v2 the device_id IS the bearer rendezvous handle. */
function buildProvisionPayload(appId, deviceId) {
  return {
    v: 1,
    mode: 'ozkey-cloud',
    ssid: CONFIG.PROVISION_WIFI_SSID,
    password: CONFIG.PROVISION_WIFI_PASSWORD,
    broker_host: CONFIG.BROKER.host,
    broker_tcp_port: CONFIG.BROKER.tcp_port,
    broker_ws_port: CONFIG.BROKER.ws_port,
    broker_ws_path: CONFIG.BROKER.ws_path,
    server_ip: CONFIG.SERVER_IP,
    server_port: CONFIG.HTTP_PORT,
    site_id: CONFIG.SITE_ID,
    app_id: appId,
    device_id: deviceId,
    heartbeat_s: CONFIG.DEFAULT_HEARTBEAT_S,
  };
}

/**
 * Register an app⇄device pairing (XF-42 §13.2). OZLOCK authenticates neither
 * party: the app self-generated app_id and granted the lock its device_id at
 * the BLE ceremony, and now records the bond here. First-writer-wins on the
 * random device_id — re-registering by the SAME app is idempotent; a different
 * app claiming a live device_id is refused (squatting guard). The device_id's
 * unguessability + the (future) e2e envelope are the security, not a server
 * credential. Returns {status:number}|null via `err` for the caller to map.
 */
async function registerPairing(appId, deviceId, label, bridgeId, ozkieVersion) {
  const [[existing]] = await pool.query('SELECT app_id, status FROM locks WHERE id = ?', [
    deviceId,
  ]);
  if (existing && existing.app_id && appId && existing.app_id !== appId) {
    const e = new Error(`device_id already paired to a different app`);
    e.httpStatus = 409;
    // XF-48 ask (I). NOT `e.code` — mysql2 and Node system errors already use
    // `.code` (ER_DUP_ENTRY, ECONNREFUSED), so writing there would sometimes
    // ship a driver's internal code to the app as if it were our contract.
    e.ozCode = 'device_paired_elsewhere';
    throw e;
  }
  // Fresh registration lands as 'registered' (awaiting the lock's first
  // contact); an already-enrolled lock keeps its status on re-register.
  await pool.query(
    `INSERT INTO locks (id, site_id, app_id, bridge_id, label, status, heartbeat_s)
       VALUES (?, ?, ?, ?, ?, 'registered', ?)
     ON DUPLICATE KEY UPDATE app_id = VALUES(app_id),
       bridge_id = COALESCE(VALUES(bridge_id), bridge_id),
       label = COALESCE(VALUES(label), label)`,
    [deviceId, CONFIG.SITE_ID, appId, bridgeId || null, label || 'New Doorlock', CONFIG.DEFAULT_HEARTBEAT_S]
  );
  // Only written when the app says so — an older app build that never sends
  // it must not flip an existing row back to the default.
  if (ozkieVersion) await pool.query('UPDATE locks SET ozkie_version = ? WHERE id = ?', [ozkieVersion, deviceId]);
  logEvent(
    'pair',
    `Pairing registered: app ${appId || '(anon)'} ⇄ device ${deviceId}` +
      (bridgeId
        ? ` via bridge ${bridgeId} (bridged — no self-enroll, see ozkey-11 §3)`
        : ' — awaiting doorlock contact')
  );
  await recordAudit(appId, deviceId, 'pair', 'registered pairing');
}

/* -- Pairing registration (trust-model v2, XF-42 §13.2) ---------------------- */
api.post('/pairings', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { app_id, device_id, label, bridge_id, ozkie_version } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    // ozkey-48 §7: optional, absent = leave as is (default 1 for a new row).
    if (ozkie_version !== undefined && ozkie_version !== 1 && ozkie_version !== 2) {
      return res.status(400).json({
        ok: false,
        code: 'invalid_ozkie_version',
        error: 'ozkie_version must be 1 or 2 (ozkey-48 §7)',
      });
    }
    const deviceId = device_id ? String(device_id).slice(0, 64) : null;
    // Optional: present only for a Thread lock reached through a bridge32
    // gateway (ozkey-11 §3). Absent = direct Wi-Fi lock, routed as before.
    const bridgeId = bridge_id ? String(bridge_id).slice(0, 64) : null;
    if (!appId || !deviceId) {
      return res
        .status(400)
        .json({
          ok: false,
          code: 'missing_fields',
          error: 'app_id and device_id are required (the app grants both)',
        });
    }
    await registerPairing(appId, deviceId, label, bridgeId, ozkie_version);
    // ozkey-23 §10.2a, XF-96 §4/§6: app broker auth is NOT minted here —
    // it's a separate identity/auth flow (ozkey-24: POST /apps/register +
    // /auth/challenge + /auth/token, below), independent of pairing a
    // particular lock.
    res.json({
      ok: true,
      device_id: deviceId,
      app_id: appId,
      bridge_id: bridgeId,
      ozkie_version: ozkie_version || null,
      provision_payload: buildProvisionPayload(appId, deviceId),
    });
  } catch (err) {
    // ozCode is OUR contract; err.httpStatus present means a deliberate throw.
    // Anything else is an unhandled fault and must not masquerade as a known code.
    res.status(err.httpStatus || 500).json({
      ok: false,
      code: err.ozCode || (err.httpStatus ? 'request_failed' : 'internal_error'),
      error: err.message,
    });
  }
});

api.get('/pairings/status', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = String(req.query.device_id || '');
    const [[row]] = await pool.query(
      'SELECT id, app_id, bridge_id, status, mac FROM locks WHERE id = ?',
      [deviceId]
    );
    if (!row)
      return res.status(404).json({ ok: false, code: 'pairing_not_found', error: 'no such pairing' });
    res.json({
      ok: true,
      device_id: row.id,
      app_id: row.app_id,
      bridge_id: row.bridge_id,
      // 'registered' = awaiting lock; 'enrolled' = lock made first contact.
      status: row.status,
      mac: row.mac,
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- DEPRECATED: /enroll/begin — token-variant shim over registerPairing ----- */
api.post('/enroll/begin', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { label, app_id, device_id } = req.body || {};
    const appId = app_id ? String(app_id).slice(0, 80) : null;
    const deviceId = device_id ? String(device_id).slice(0, 64) : makeSecret(16, 'ozl-');
    await registerPairing(appId, deviceId, label);
    logEvent('warn', `/enroll/begin is deprecated — use POST /pairings (device ${deviceId})`);
    res.json({
      ok: true,
      // `token` retained only as a correlation handle for legacy callers.
      token: deviceId,
      device_id: deviceId,
      app_id: appId,
      provision_payload: buildProvisionPayload(appId, deviceId),
    });
  } catch (err) {
    // ozCode is OUR contract; err.httpStatus present means a deliberate throw.
    // Anything else is an unhandled fault and must not masquerade as a known code.
    res.status(err.httpStatus || 500).json({
      ok: false,
      code: err.ozCode || (err.httpStatus ? 'request_failed' : 'internal_error'),
      error: err.message,
    });
  }
});

api.get('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    // bridge_id + caps, same as GET /locks/:id (2026-08-02). Patching only the
    // single-lock read was not enough: the app builds its lock LIST from here, so
    // a lock could report remote_unlock when opened individually and still render
    // as BLE-only in the list the user actually taps. Both reads must agree.
    // presence/presence_reason (ozkey-20 R6 verdict) exposed here so the app can
    // decide Thread-vs-BLE revoke without re-deriving liveness itself — this is
    // the field the app team asked about (2026-08-10): whether a lock is still
    // reachable via Thread/bridge before falling back to asking the user for BLE.
    const [rows] = await pool.query(
      `SELECT id, site_id, app_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
         FROM locks ORDER BY enrolled_at DESC`
    );
    const backupSet = await backupAvailableSet();
    const locks = rows.map((l) => {
      const { caps, source } = effectiveCaps(l);
      return {
        ...l,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(l),
        events_pending: eventsPending(l),
        backup_available: backupSet.has(l.id),
      };
    });
    res.json({ ok: true, locks });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** XF-144 Phase 3 (2026-09-02, App's own ask): a bridge with zero currently-
 * assigned locks (e.g. every lock behind it already failed over elsewhere)
 * is invisible to `GET /locks` — nothing there mentions a `bridge_id` no
 * lock currently points at. App derives all its bridge-health UI by joining
 * its own locally-known bridge list against this, by `bridge_id` — so this
 * exists purely to expose `bridges_presence` directly, not to be a bridge
 * registry (deliberately not what the `bridges` table is either — see its
 * own header comment). No new columns, no new derivation — same raw rows
 * `computeFaultAttribution()` already reads.
 */
api.get('/bridges', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query('SELECT bridge_id, presence, presence_at FROM bridges_presence ORDER BY bridge_id');
    res.json({ ok: true, bridges: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* ===========================================================================
 * Registry / observability lookups (OZLOCK console — no actions, ozkey-05)
 * ========================================================================= */

/** Enumerate the apps (users) OZLOCK knows, with how many locks each holds. */
api.get('/apps', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT app_id,
              COUNT(*) AS lock_count,
              SUM(status = 'enrolled') AS enrolled_count,
              MAX(last_seen_at) AS last_seen_at
         FROM locks
        WHERE app_id IS NOT NULL
        GROUP BY app_id
        ORDER BY lock_count DESC`
    );
    res.json({ ok: true, apps: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given an app (user id) → all its doorlocks. */
api.get('/apps/:appId/locks', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      `SELECT id, app_id, mac, label, status, power_profile, heartbeat_s, last_seen_at, enrolled_at
         FROM locks WHERE app_id = ? ORDER BY enrolled_at DESC`,
      [req.params.appId]
    );
    res.json({ ok: true, app_id: req.params.appId, locks: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given an app (user id) → its control-plane activity (grant/revoke/unlock…). */
api.get('/apps/:appId/activity', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { where, params } = rangeWhere('app_id = ?', [req.params.appId], req.query);
    const { limit, offset } = pageParams(req.query);
    const [[{ total }]] = await pool.query(
      `SELECT COUNT(*) AS total FROM audit_log WHERE ${where}`,
      params
    );
    const [rows] = await pool.query(
      `SELECT * FROM audit_log WHERE ${where} ORDER BY id DESC LIMIT ? OFFSET ?`,
      [...params, limit, offset]
    );
    res.json({ ok: true, app_id: req.params.appId, activity: rows, total, limit, offset });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** Given a doorlock (device id) → the app it's bound to + its record. */
api.get('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    // bridge_id + caps added 2026-08-02. They were being SELECTed nowhere, so the
    // app had no way to learn a Thread lock is remotely reachable and fell back to
    // BLE — which a commissioned lock never advertises for. Symptom was "Do not
    // see doorlock" on a lock the server could have unlocked: effectiveCaps()
    // already inferred remote_unlock from bridge_id, we simply never published it.
    // This is XF-48 ask 1 (the capability field) landing on the read path.
    const [[lock]] = await pool.query(
      `SELECT id, app_id, site_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
         FROM locks WHERE id = ?`,
      [req.params.id]
    );
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${req.params.id} not found` });
    const { caps, source } = effectiveCaps(lock);
    // caps_source lets the app distinguish "the device told us" from "we inferred
    // it from a bound bridge" — XF-48 §9.5 wants the device's own report to win
    // once the state uplink exists, and silently swapping one for the other would
    // be undiagnosable from the app side.
    const backupSet = await backupAvailableSet([lock.id]);
    res.json({
      ok: true,
      lock: {
        ...lock,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(lock),
        events_pending: eventsPending(lock),
        backup_available: backupSet.has(lock.id),
      },
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/** XF-144 (2026-09-01): "scan advisor," not a full pairer — deliberately
 * does NOT trigger pairing itself. Today's only pairing mechanism
 * (`pairctl`) is a BLE GATT write the PHONE performs directly against a
 * bridge; there is no MQTT-triggered pairing command on a bridge today
 * (flagged, not assumed, in this endpoint's own XF-144 thread — building
 * that is a separate, not-yet-agreed piece). This endpoint answers "which
 * bridge should you pair with," ranked by the bridge's own measured
 * reachability instead of the phone's BLE radio — the app still performs
 * the actual `pairctl` write itself, same mechanism as its already-shipped
 * RSSI-ranked flow, just fed a better signal.
 */
api.post('/locks/:id/scan-bridges', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT id FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

    const { requested, results } = await scanForLockAcrossBridges(deviceId);
    const candidates = results
      .filter((r) => r.found)
      .sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
    logEvent(
      'info',
      `Scan-bridges for ${deviceId}: asked ${requested} online bridge(s), ` +
        `${results.length} responded, ${candidates.length} report reachability`
    );
    res.json({ ok: true, device_id: deviceId, bridges_asked: requested, candidates });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.patch('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const { label, power_profile, heartbeat_s, bridge_id } = req.body || {};
    const sets = [];
    const params = [];
    if (label !== undefined) {
      sets.push('label = ?');
      params.push(String(label).slice(0, 255));
    }
    if (power_profile !== undefined) {
      if (!['eco', 'responsive', 'scheduled'].includes(power_profile))
        return res
          .status(400)
          .json({ ok: false, code: 'invalid_power_profile', error: 'invalid power_profile' });
      sets.push('power_profile = ?');
      params.push(power_profile);
    }
    // XF-138 P5, S6/S7: `bridge_id` used to be write-once (POST /pairings'
    // `COALESCE(VALUES(bridge_id), bridge_id)` — first value wins, never
    // clearable). That left no way to re-pair an already-registered lock to a
    // different bridge or unpair it, which the P5 allowlist flow needs.
    // `undefined` = field omitted, leave untouched; explicit `null` = unpair;
    // a non-empty string = (re)pair. No COALESCE here — this route is the one
    // place allowed to overwrite, including back to NULL.
    let bridgeIdChanged = false;
    let newBridgeId;
    if (bridge_id !== undefined) {
      if (bridge_id !== null && (typeof bridge_id !== 'string' || !bridge_id.trim()))
        return res.status(400).json({
          ok: false,
          code: 'invalid_bridge_id',
          error: 'bridge_id must be a non-empty string (to pair) or null (to unpair)',
        });
      newBridgeId = bridge_id === null ? null : bridge_id.trim().slice(0, 64);
      sets.push('bridge_id = ?');
      params.push(newBridgeId);
      bridgeIdChanged = true;
      // XF-138 (2026-08-31): `mcu_link_up`/`mcu_last_frame_s` are only ever
      // meaningful relative to the specific bridge relationship that
      // produced them — clear both on ANY bridge_id change (pair or
      // unpair), so stale telemetry from a previous bridge relationship can
      // never be misread as current under a new one.
      //
      // XF-146 (2026-09-01): `direct_presence`/`presence` reset to
      // 'unknown' for the same reason — a bridge_id change is exactly the
      // moment any prior reachability signal stops being trustworthy
      // (found live: LockA got permanently stuck `lock_unreachable` after
      // an unrelated bridge MQTT blip, with no way back short of a brand-
      // new physical reconnect; the underlying derivation bug is fixed,
      // but this endpoint is also a legitimate, reviewed way to force a
      // fresh signal for a row already stuck from before the fix existed).
      // Safe direction to err in either way: under-claiming reachability
      // until a real signal arrives beats trusting a stale one.
      sets.push(
        "mcu_link_up = NULL, mcu_last_frame_s = NULL, direct_presence = 'unknown', presence = 'unknown', presence_reason = NULL"
      );
    }
    if (heartbeat_s !== undefined) {
      // Clamp to the SAME 60–600 the firmware enforces (clampHeartbeatS,
      // doorlock.ino:285). The old Math.max(5, …) had no ceiling, so a PATCH of 5
      // or 86400 was stored and the row then disagreed with anything the lock would
      // actually run — the "row says X, device runs Y" class ftpos flagged in
      // XF-55 §11.2. One authority, one range.
      const hb = Number(heartbeat_s) || CONFIG.DEFAULT_HEARTBEAT_S;
      sets.push('heartbeat_s = ?');
      params.push(hb < 60 ? 60 : hb > 600 ? 600 : hb);
    }
    // XF-125 P0/G4: app-confirmed read progress on the lock's event log.
    // `query_events`/`events_response` is BLE-only (ozdoorlock_core.h:6103-
    // 6161, `ozNotifySealedTo()`) — invisible to this server by construction,
    // same as the door events it carries — so the app must say how far it's
    // gotten, there is no wire signal to observe instead. PATCH here rather
    // than a dedicated route: app (2026-08-21) already shipped this shape
    // best-effort against `PATCH /locks/:id`, and firmware endorsed it
    // ("PATCH on the existing endpoint is sensible, I would not invent a new
    // one") — converging on their existing contract beats introducing a
    // second, competing one for the same fact.
    const lastPulledSeq = req.body ? req.body.last_pulled_seq : undefined;
    if (lastPulledSeq !== undefined) {
      if (!Number.isFinite(Number(lastPulledSeq)) || Number(lastPulledSeq) < 0)
        return res
          .status(400)
          .json({ ok: false, code: 'invalid_last_pulled_seq', error: 'last_pulled_seq must be an integer >= 0' });
      // GREATEST, not a bare overwrite: idempotent and monotonic, so a
      // stale/out-of-order retry can never un-confirm progress a fresher
      // report already made.
      sets.push('last_pulled_seq = GREATEST(COALESCE(last_pulled_seq, 0), ?)');
      params.push(Number(lastPulledSeq));
    }
    if (!sets.length)
      return res.status(400).json({ ok: false, code: 'nothing_to_update', error: 'nothing to update' });
    params.push(req.params.id);
    const [r] = await pool.query(`UPDATE locks SET ${sets.join(', ')} WHERE id = ?`, params);
    if (r.affectedRows === 0)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${req.params.id} not found` });
    logEvent('info', `Lock ${req.params.id} settings updated (${sets.join(', ')})`);
    if (bridgeIdChanged) {
      logEvent(
        'pair',
        newBridgeId
          ? `Lock ${req.params.id} paired to bridge ${newBridgeId} via PATCH (XF-138 P5)`
          : `Lock ${req.params.id} unpaired (bridge_id cleared) via PATCH (XF-138 P5)`
      );
      await recordAudit(
        null,
        req.params.id,
        newBridgeId ? 'pair' : 'unpair',
        newBridgeId ? `bridge_id set to ${newBridgeId}` : 'bridge_id cleared'
      );
      // XF-144 (2026-09-01): keep `lock_bridges` in sync with every real
      // pair/re-pair, not just at migration time — otherwise the candidate
      // list is a frozen snapshot from whenever the table was created,
      // useless for any lock paired after that moment. A newly-active
      // bridge always becomes priority 0; every other known candidate for
      // this lock shifts down one, so the most recently proven-working
      // bridge is always tried first on failover, and older ones remain
      // available as fallback rather than being discarded. Only runs when
      // actually PAIRING (newBridgeId truthy) — unpairing removes the
      // active pointer but deliberately leaves history in this table, so a
      // later re-pair to a previously-known bridge doesn't lose its place
      // in the ranking.
      if (newBridgeId) {
        await pool.query('UPDATE lock_bridges SET priority = priority + 1 WHERE lock_id = ? AND bridge_id != ?', [
          req.params.id,
          newBridgeId,
        ]);
        await pool.query(
          `INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
             VALUES (?, ?, 0, NOW())
           ON DUPLICATE KEY UPDATE priority = 0, last_confirmed_reachable_at = NOW()`,
          [req.params.id, newBridgeId]
        );
      }
    }
    if (lastPulledSeq !== undefined) {
      const [[lock]] = await pool.query(
        'SELECT dropped_before_seq, last_pulled_seq FROM locks WHERE id = ?',
        [req.params.id]
      );
      if (!eventsAtRisk(lock) && eventsAtRiskLogged.has(req.params.id)) {
        eventsAtRiskLogged.delete(req.params.id);
        logEvent('info', `XF-125: ${req.params.id} events risk cleared — PATCH last_pulled_seq=${lock.last_pulled_seq}`);
      }
    }
    // Return the updated row (with recomputed caps) so a bridge_id change is
    // immediately visible without a second GET — the whole point of S6/S7 is
    // that remote_unlock flips the instant bridge_id does (effectiveCaps()).
    const [[updatedLock]] = await pool.query(
      `SELECT id, app_id, site_id, mac, label, fw, status, power_profile, heartbeat_s,
              last_seen_at, enrolled_at, bridge_id, caps, transport, tuya_pid, profile, ozkie_version,
              presence, direct_presence, presence_reason, presence_at, battery_pct, mcu_link_up, mcu_last_frame_s,
              seq_highwater, dropped_before_seq, last_pulled_seq, has_doorbell, profile_mismatch, bonds
         FROM locks WHERE id = ?`,
      [req.params.id]
    );
    const { caps, source } = effectiveCaps(updatedLock);
    res.json({
      ok: true,
      id: req.params.id,
      lock: {
        ...updatedLock,
        caps,
        caps_source: source,
        events_at_risk: eventsAtRisk(updatedLock),
        events_pending: eventsPending(updatedLock),
      },
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Clear the fleet (start fresh) ------------------------------------------- *
 * No FK cascade from locks → grants/pending_queue (they only carry a device_id
 * column), so wipe the dependents explicitly. DELETE /locks clears every lock
 * for the site; DELETE /locks/:id removes one. Lab/dev convenience.
 *
 * `lock_logs` is intentionally absent — this server no longer creates it, and
 * DELETEing a table that does not exist would throw on a fresh schema.
 */
async function purgeLockRows(conn, where, args) {
  await conn.query(`DELETE FROM grants WHERE ${where}`, args);
  await conn.query(`DELETE FROM pending_queue WHERE ${where}`, args);
  await conn.query(`DELETE FROM audit_log WHERE ${where}`, args);
  if (hooks.purgeExtraRows) await hooks.purgeExtraRows(conn, where, args);
}

/** Parse pagination query (?limit=&offset=), clamped; default 12 rows/page. */
function pageParams(query) {
  const limit = Math.min(200, Math.max(1, Number(query.limit) || 12));
  const offset = Math.max(0, Number(query.offset) || 0);
  return { limit, offset };
}

/**
 * Append an optional inclusive date range (?from=YYYY-MM-DD&to=YYYY-MM-DD) on
 * created_at to a base WHERE. `to` is inclusive (matches through end-of-day).
 * Returns { where, params } for interpolation (base clause is caller-literal).
 */
function rangeWhere(baseClause, baseParams, query) {
  const clauses = [baseClause];
  const params = [...baseParams];
  // Compare whole calendar dates (both ends inclusive) so a from=to=today
  // range matches regardless of time-of-day / UTC-vs-local boundary skew.
  if (query.from) {
    clauses.push('DATE(created_at) >= ?');
    params.push(String(query.from));
  }
  if (query.to) {
    clauses.push('DATE(created_at) <= ?');
    params.push(String(query.to));
  }
  return { where: clauses.join(' AND '), params };
}

// ozkey-23 §5(c)'s scrubExpiredGrantNames() lived here — removed 2026-08-13
// (ozkey-29, operator instruction). It nulled `grants.user_name` post hoc,
// on revoke/expiry. There is no longer a `user_name` column to null (see
// the ozkey-29 cutover near the `grants` CREATE TABLE above) and
// `recordAudit()` no longer writes identity into `audit_log.detail` in the
// first place (§5.2/§12 below) — nothing left to redact after the fact.
// A scrub-on-schedule was already the wrong shape per ozkey-29 §1 ("policy
// sitting on top of retained access"); not writing it is the actual fix.

api.delete('/locks', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    await conn.beginTransaction();
    await purgeLockRows(conn, 'site_id = ?', [CONFIG.SITE_ID]);
    const [d] = await conn.query('DELETE FROM locks WHERE site_id = ?', [CONFIG.SITE_ID]);
    await conn.commit();
    logEvent('warn', `Fleet cleared — ${d.affectedRows} doorlock(s) removed (start fresh)`);
    res.json({ ok: true, removed: d.affectedRows });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});


api.delete('/locks/:id', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const id = req.params.id;
    const { envelope_hex } = req.body || {};
    // Unpair the physical lock too (BANOI "Gỡ khoá" must reset the device,
    // not just the record): tell it to wipe NVS and return to ADVERTISING.
    // Best-effort — an offline lock misses it and needs the on-device reset.
    const [[lock]] = await conn.query(
      'SELECT site_id, bridge_id, last_seen_at, heartbeat_s, presence, presence_reason, mac, last_reset_reason, ozkie_version FROM locks WHERE id = ?',
      [id]
    );
    let attempted = false;
    let transportOk = false;
    let likelyDelivered = false;
    let resetVerdict = null; // ozkey-41 §5.3: only the sealed branch below gets one
    // XF-119 §6/§9.3 ask 2: the lock may have ALREADY told us it reset —
    // BOOT hold, the DL-MCU's own button, or a prior request this server
    // never issued or already forgot about. That message carries no
    // msg_id to wait on and never will (ozpresence.h, deliberately — a
    // reset nobody requested has no request to name), so opening a wait
    // here would time out on a lock that has already reached the goal
    // state. Skip publishing anything and resolve immediately instead.
    const alreadyReset = !!lock && lock.last_reset_reason === 'factory_reset';
    if (lock) {
      if (alreadyReset) {
        logEvent(
          'key',
          `Factory reset SKIPPED for lock ${id} — already reset per retained presence (XF-119)`
        );
      } else if (lock.ozkie_version === OZKIE_V2) {
        // ozkey-48: neither branch below can reach a v2 lock — both send v1
        // bytes — and `dev/reset` over a relay is owner-only actuation with an
        // Echo round trip (§4.2), not built yet. Remove the record only and
        // report the reset as not attempted; the owner resets at the door.
        logEvent('warn', `Factory reset NOT sent for lock ${id} — OZKIE v2 relay not built yet (ozkey-48 §7)`);
      } else if (envelope_hex) {
        // XF-91 §5 — sealed unpair, required for a Thread lock: bridge32 has
        // no `locks/+/command` subscription at all, so the plain path below
        // reaches nothing on that transport (confirmed on hardware, XF-91
        // §2/§7). Same shape as bond-revoke — queue + flushQueueForDevice
        // already route a sealed_envelope job through the bridge with
        // `target` when bridge_id is set (ozkey-11 §3).
        const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
        const [queueResult] = await pool.query(
          `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
           VALUES (?, ?, NULL, 'factory-reset', ?, 'sealed_envelope', 'queued', ?)`,
          [id, lock.site_id, envelope_hex, expiresAt]
        );
        attempted = true;
        const sent = await flushQueueForDevice(lock.site_id, id, (job, msgId) => {
          // Open the waiter here, not after the DB delete below: a lock that
          // answers before then would have its verdict dropped and the
          // request would sit out LOCK_RESET_TIMEOUT_MS as 'unknown'.
          if (job.id === queueResult.insertId) {
            resetVerdict = waitForLockResetVerdict(id, msgId, CONFIG.LOCK_RESET_TIMEOUT_MS);
          }
        });
        transportOk = sent > 0;
        logEvent(
          'key',
          `Factory reset (sealed) queued for lock ${id} (queue #${queueResult.insertId})`
        );
      } else {
        // Legacy plain path — XF-91 §5: "can stay for now" for a Wi-Fi-direct
        // lock, reachable only because it holds its own MQTT session. Same
        // dead end as above on Thread; kept for back-compat with app builds
        // that haven't added sealing for this verb yet.
        attempted = mqttPublish(CONFIG.topicCommand(lock.site_id || CONFIG.SITE_ID, id), {
          op: 'factory_reset',
          ts: new Date().toISOString(),
        });
        transportOk = attempted;
      }
      // XF-91 (AW) / XF-92 §7 / ozkey-25 §5.5 — CORRECTED 2026-08-12.
      // The `lock.presence === 'online'` boolean collapsed presence's own
      // 'unknown' state into `false`, which this comment used to concede as
      // a known limitation ("this field alone still has to"). Firmware then
      // caught it producing a real FALSE NEGATIVE on hardware: two locks
      // that had genuinely wiped (confirmed by direct BLE INFO read —
      // name:"", transport reverted to the wifi NVS default) both reported
      // `likely_delivered:false`, because Thread liveness happened to read
      // "0 reported, 0 updated" — i.e. "we don't know" — at request time,
      // and `false` said "we know it didn't." Now three states instead of
      // two: `null` for genuinely unknown, distinct from a real negative.
      likelyDelivered = lock.bridge_id
        ? lock.presence === 'unknown'
          ? null
          : lock.presence === 'online'
        : lock.last_seen_at == null
          ? null // never contacted the server at all — no data, not a negative
          : likelyOnline(lock.last_seen_at, lock.heartbeat_s);
    }
    await conn.beginTransaction();
    await purgeLockRows(conn, 'device_id = ?', [id]);
    const [d] = await conn.query('DELETE FROM locks WHERE id = ?', [id]);
    await conn.commit();
    if (d.affectedRows === 0)
      return res.status(404).json({ ok: false, code: 'lock_not_found', error: `Lock ${id} not found` });
    if (lock && hooks.onLockRemoved) hooks.onLockRemoved(lock); // fire-and-forget (ozlock: nexusDecommission())

    // XF-147: tell the currently-assigned bridge to drop this lock from its
    // own BLE allowlist too — without this, a decommissioned lock silently
    // reconnects on the bridge's next scan and keeps occupying a connection
    // slot as if still owned. Fire-and-forget, same as nexusDecommission()
    // above: the row is already gone server-side regardless of whether the
    // bridge is currently reachable to receive this. Deliberately
    // `unpair_lock`, NOT `unpair` — firmware confirmed bare `op:"unpair"` on
    // this topic already means "wipe the whole bridge" (bridgeOwnershipCheck
    // + factoryReset()), a real naming collision caught before this shipped.
    // Only known limitation, not solved here: if this lock was ever paired
    // to an earlier bridge that `bridge_id` no longer points to, that older
    // bridge never hears about this and keeps a stale allowlist entry — the
    // same single-column limitation XF-144's `lock_bridges` table exists
    // for elsewhere, not re-solved as a one-off here (see XF-147 Q3).
    if (lock && lock.bridge_id) {
      mqttPublish(CONFIG.topicBridgeCommand(lock.site_id || CONFIG.SITE_ID, lock.bridge_id), {
        op: 'unpair_lock',
        id,
      });
      logEvent('key', `XF-147: sent unpair_lock for ${id} to bridge ${lock.bridge_id}`);
    }

    // ozkey-41 §5/§6: wait for the real wire-level outcome — done after the
    // commit above (this server's own bookkeeping is unconditional, same as
    // before) rather than gating the DB delete on it, since how the app
    // itself should react to a denial/no_bond/timeout is still open (XF-114
    // §7.6 ask 3). `no_bond` resolves as a CONFIRMED reset (§5.2 — the lock
    // couldn't even decrypt the request because it's already unowned, which
    // for a removal is the desired end state, not a refusal).
    let verdict = null;
    let cause = null;
    if (alreadyReset) {
      verdict = 'reset_confirmed';
      cause = 'already_reset';
    } else if (transportOk && resetVerdict) {
      ({ verdict, cause } = await resetVerdict);
    }

    logEvent(
      'info',
      `Doorlock ${id} removed + factory_reset sent (attempted=${attempted} likely_delivered=${likelyDelivered} transport_ok=${transportOk}` +
        (verdict ? ` verdict=${verdict}${cause ? `/${cause}` : ''}` : '') +
        ')'
    );
    res.json({
      ok: true,
      id,
      reset: {
        attempted,
        likely_delivered: likelyDelivered,
        transport_ok: transportOk,
        presence: lock ? lock.presence : 'unknown',
        presence_reason: lock ? lock.presence_reason : null,
        verdict,
        cause,
      },
    });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});

/* -- Grants: issue / list / revoke user keys --------------------------------- */
api.post('/locks/:id/grants', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const deviceId = req.params.id;
    // ozkey-29 cutover, 2026-08-13: `user_name`/`date_from`/`date_to` are no
    // longer accepted into server storage — see the `grants` schema note
    // above. A caller may still send them (older app builds do); they are
    // silently ignored rather than rejected, same accept-both posture S3/S4
    // used during their own cutover window. `date_from`/`date_to` were
    // structurally redundant regardless (XF-95 §2) — the lock already
    // receives and enforces its own copy inside the sealed credential
    // envelope.
    const { type = 'pin', envelope_hex, slot_number = 1 } = req.body || {};

    // S4 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08: the app
    // seals the DP frame client-side (A1-A5, ftpos shipped) — `raw_value` is
    // no longer accepted at all, not just deprioritized. The server relays
    // envelope_hex opaque, never builds or sees the PIN/RFID.
    if (!envelope_hex) {
      return res.status(400).json({
        ok: false,
        code: 'missing_fields',
        error: 'envelope_hex is required',
      });
    }
    if (type === 'fingerprint') {
      return res.status(422).json({
        ok: false,
        code: 'credential_type_unsupported',
        error: 'fingerprint credentials are on hold — DP codec supports pin/rfid only',
      });
    }
    if (!SUPPORTED_CRED_TYPES.includes(type)) {
      return res
        .status(400)
        .json({
          ok: false,
          code: 'credential_type_unsupported',
          error: `type must be one of: ${SUPPORTED_CRED_TYPES.join(', ')}`,
        });
    }

    const [[lock]] = await conn.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });
    if (refuseIfOzkieV2(res, lock, deviceId, 'Grant')) return;

    await conn.beginTransaction();
    // S3 cutover: grants has no raw_value column anymore — the server
    // genuinely cannot know the PIN/RFID for a row it stores, structurally.
    const [grantResult] = await conn.query(
      `INSERT INTO grants (device_id, site_id, type, slot_number, sync_status)
       VALUES (?, ?, ?, ?, 'pending')`,
      [deviceId, lock.site_id, type, slot_number]
    );
    const grantId = grantResult.insertId;
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status)
       VALUES (?, ?, ?, 'grant-key', ?, 'sealed_envelope', 'queued')`,
      [deviceId, lock.site_id, grantId, envelope_hex]
    );
    await conn.commit();

    logEvent(
      'key',
      `Granted ${type.toUpperCase()} slot ${slot_number} on ${deviceId} ` +
        `(grant #${grantId}, queue #${queueResult.insertId}, sealed) — awaiting wake`
    );
    await recordAudit(lock.app_id, deviceId, 'grant', `grant ${type.toUpperCase()} slot ${slot_number}`);

    flushQueueForDevice(lock.site_id, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
      envelope_hex,
      sync_status: 'pending',
    });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});

api.get('/locks/:id/grants', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query(
      'SELECT * FROM grants WHERE device_id = ? ORDER BY id DESC LIMIT 100',
      [req.params.id]
    );
    res.json({ ok: true, grants: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Remote unlock (the away-path "Mở cửa", ozkey-05 §6.3) -------------------- */
/* -- XF-48 ask (E): capability enforcement, server-side ---------------------- *
 * ftpos raised this and they were right: removing the remote-unlock affordance
 * from BANOI fixes one build of one client. It does not close the path for older
 * app versions, other clients, or a direct API call — and after their fix the
 * exposure would be INVISIBLE to them, because the only thing that knew the rule
 * was the UI they had just changed.
 *
 * Authority order, and the order matters:
 *   1. `locks.caps` — what the LOCK reported about itself (firmware, M3+).
 *      A device's own statement, per XF-48 Ask 1. Never override it by
 *      inference; inference is what rots.
 *   2. Interim rule until caps ships: remote unlock requires a bound bridge.
 *
 * Why a bridge is the right proxy: a direct Wi-Fi lock wakes every 2-10 minutes,
 * and the unlock below carries a 60 s expiry precisely so a command can never
 * fire stale. So for an ECO lock the command already almost always expired
 * unheard — the door simply never opened and nothing said why. Enforcement does
 * not remove a capability; it converts a silent, undiagnosable expiry into a
 * refusal the app can render.
 */
// S1: the profile decides (ozlock: OZLOCK_ENFORCE_CAPS !== '0'); the core reads no env.


// XF-58. The "visitor at the door" unlock: the owner authorises remotely while on
// the phone, and the visitor's keypad touch completes it. 60 s — operator's call,
// and it is safe ONLY because doorlock-1.5 refuses the command without a recent
// touch. Without that lock-side check the window would be a probability rather
// than a requirement: a sleeping lock also wakes on its heartbeat timer, so a
// longer window would make an UNATTENDED open more likely, not less.
const ASSISTED_UNLOCK_MS = 60_000;


api.post('/locks/:id/unlock', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });

    // ozkey-48 §6 (operator, 2026-10-18): on a v2 lock, remote unlock is OWNER
    // ONLY — members are on-premises only and never get a relay context, so
    // their relayed unlock could never execute anyway. The lock is what
    // enforces this; the server has no way to see whose context sealed an
    // OSCORE request, and this API takes the caller's word for `app_id`. This
    // check is fail-fast UX, so a member's app gets a clear answer instead of
    // a timeout, not a security boundary.
    if (lock.ozkie_version === OZKIE_V2) {
      const callerAppId = req.body && req.body.app_id ? String(req.body.app_id).trim().toLowerCase() : null;
      if (!callerAppId || callerAppId !== String(lock.app_id || '').toLowerCase()) {
        logEvent('warn', `Remote UNLOCK refused for "${lock.label}" — not the owner (OZKIE v2, ozkey-48 §6)`);
        return res.status(403).json({
          ok: false,
          code: 'remote_unlock_owner_only',
          error: 'remote_unlock_owner_only',
          detail:
            'Only the lock owner can unlock this lock remotely. Members unlock with ' +
            'Bluetooth at the door.',
          device_id: deviceId,
          ref: 'ozkey-48 §6',
        });
      }
      // §4.2: actuation is never store-and-forward — so nothing below (which
      // queues) may run for a v2 lock, even once the relay exists.
      if (refuseIfOzkieV2(res, lock, deviceId, 'Remote UNLOCK')) return;
    }

    // A bridged (Thread) lock has no MQTT uplink of its own, so handleEnroll can
    // never run for it and its status cannot advance past 'registered' — that is
    // the ozkey-10 uplink gap, not a half-finished pairing. Commands still reach
    // it, because the BRIDGE is the MQTT client. Direct locks keep the stricter
    // 'enrolled' bar, which for them still means "the lock has spoken to us".
    const reachable = lock.status === 'enrolled' || (lock.bridge_id && lock.status === 'registered');
    if (!reachable) {
      return res
        .status(409)
        .json({
          ok: false,
          code: 'lock_not_enrolled',
          error: `Lock ${deviceId} is not enrolled yet (status: ${lock.status})`,
          status_now: lock.status,
        });
    }

    // XF-113 §5.1 — do not report success for an undeliverable command.
    // `lock.presence`/`presence_reason` are already kept live by LWT +
    // recomputeAndStorePresence() (bridge offline, lock off-mesh behind a
    // healthy bridge, or a Wi-Fi lock's own LWT) — this was previously never
    // read here, so a publish to an absent subscriber (fire-and-forget MQTT,
    // no error) still returned ok:true/delivered. Checked BEFORE anything is
    // queued, same reasoning as the capability gate below: a refused unlock
    // leaves no row to expire later and no audit entry implying it was
    // attempted at the door.
    if (lock.presence === 'offline') {
      logEvent(
        'warn',
        `Remote UNLOCK refused for "${lock.label}" — unreachable (${lock.presence_reason || 'offline'})`
      );
      return res.status(409).json({
        ok: false,
        code: lock.presence_reason || 'lock_unreachable',
        error: 'lock_unreachable',
        detail:
          'This doorlock is not currently reachable, so the command would never be ' +
          'delivered. ' +
          (lock.bridge_id
            ? 'Its bridge (or the lock behind it) is offline — nothing is subscribed ' +
              'to receive the command.'
            : 'It has no live network connection.') +
          ' Falling back to BLE-at-the-door is the only path that can work right now.',
        device_id: deviceId,
        presence_reason: lock.presence_reason,
        ref: 'XF-113 §5.1',
      });
    }

    // XF-48 ask (E) — capability gate. Checked BEFORE anything is queued, so a
    // refused unlock leaves no row to expire later and no audit entry implying
    // it was attempted at the door.
    const { caps, source } = effectiveCaps(lock);
    const assisted = !caps.includes('remote_unlock') && caps.includes('assisted_unlock');
    if (CONFIG.ENFORCE_CAPS && !caps.includes('remote_unlock') && !assisted) {
      logEvent(
        'warn',
        `Remote UNLOCK refused for "${lock.label}" — no remote_unlock capability (${source})`
      );
      return res.status(409).json({
        ok: false,
        code: 'remote_unlock_unsupported',
        error: 'remote_unlock_unsupported', // kept: ftpos shipped against error= for this one
        detail:
          'This lock cannot be unlocked remotely. Unlock is Bluetooth-at-the-door; ' +
          'its network link carries key management, PIN sync and audit on a periodic ' +
          'wake, not live commands. Add a bridge to enable remote unlock.',
        caps,
        caps_source: source, // 'device' = the lock said so; 'inferred' = no bridge bound
        device_id: deviceId,
        ref: 'XF-48 §3',
      });
    }

    // §6.3: an unlock MUST NOT fire stale. Queue with an expiry; the flush drops
    // it if the lock doesn't wake in time.
    //
    // XF-58: an ASSISTED unlock additionally carries `action_type =
    // 'assisted-unlock'`, which doorlock-1.5 refuses unless the keypad was
    // touched in the last 30 s. The expiry alone would not be enough — it bounds
    // WHEN the command may run, never WHETHER anybody was at the door, and a
    // sleeping lock wakes on its heartbeat timer too. The device-side check is
    // what makes "someone must be there" a requirement instead of a probability.
    //
    // XF-120 §2 step 2, 2026-08-21: `envelope_hex` is now REQUIRED. The
    // accept-both rollout shim (S10) is gone — both REST-unlock callers
    // (`unlock()`, `assistedUnlock()`) ship sealing as of `ftpos 34bc1e6`,
    // so a request with no envelope is no longer a legacy client to shim
    // for, it's exactly the unauthenticated-DP1 hole XF-120 §1.2b describes.
    // Reject it outright rather than silently building a frame for it.
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res.status(400).json({
        ok: false,
        code: 'envelope_hex_required',
        error: 'envelope_hex is required',
        detail:
          'Unsealed unlock is no longer accepted — seal {"kind":"unlock"} client-side. ' +
          'This closes the unauthenticated raw-DP path XF-120 §1.2b described.',
        ref: 'XF-120 §2',
      });
    }
    const actionType = assisted ? 'assisted-unlock' : 'unlock';
    const windowMs = assisted ? ASSISTED_UNLOCK_MS : 60_000;
    const expiresAt = new Date(Date.now() + windowMs);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, NULL, ?, ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, lock.site_id, actionType, envelope_hex, expiresAt]
    );

    logEvent(
      'key',
      `${assisted ? 'ASSISTED' : 'Remote'} UNLOCK queued for "${lock.label}" ` +
        `(queue #${queueResult.insertId}, expires ${windowMs / 1000}s` +
        `${assisted ? ', needs a keypad touch' : ''}, sealed)`
    );
    await recordAudit(lock.app_id, deviceId, 'unlock', 'remote unlock');
    const sent = await flushQueueForDevice(lock.site_id, deviceId);

    res.json({
      ok: true,
      device_id: deviceId,
      queue_id: queueResult.insertId,
      envelope_hex,
      // In the lab LockSim keeps its MQTT link open, so delivery is immediate;
      // a real eco lock would report 'queued' until its next wake.
      // KEPT for compat — the app parses this today, still live-testing.
      delivery: sent > 0 ? 'delivered' : 'queued',
      // ozkey-20 R5: "delivered must stop meaning likelyOnline... report
      // accepted/transport_ok, and let the lock's own ACK be the only thing
      // that produces executed." `sent` only means "handed to the MQTT
      // broker" — it says nothing about the lock. Added alongside `delivery`
      // rather than replacing it (same alias discipline as S12/S16); this is
      // the field to actually trust, `delivery` is the deprecated one.
      transport_ok: sent > 0,
      expires_at: expiresAt.toISOString(),
      // XF-58: everything the app needs to run the countdown, so it never has to
      // hardcode our window. `mode: 'assisted'` is the app's cue to prompt
      // "bảo khách chạm vào bàn phím" and show the seconds ticking — on this path
      // `delivered` means the command is ARMED, not that the door opened, since
      // the lock will still refuse it if nobody touches.
      mode: assisted ? 'assisted' : 'remote',
      window_s: windowMs / 1000,
      requires_touch: assisted,
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Bond-verb routes (S11, ozkey-14.md 2026-08-10): bond-revoke / invite-cancel --
 * Firmware has dispatched sealed DP 101 (bond-revoke)/102 (invite-cancel) over
 * MQTT identically to BLE since ozkey-13's F1 refactor — this was always
 * deliverable, just unrouted. ftpos found the gap reading server.js before
 * building: grants is grant_id-scoped, unlock is unlock-shaped, and bond verbs
 * fit neither.
 *
 * Three deliberate differences from /unlock — do not copy its shape blindly:
 *   1. Sealed only, no accept-both, no frame builder, now or ever. A bond verb
 *      was never legal in plaintext — the lock's own plaintext path already
 *      refuses DP 101/102 (broker is anon-open). This endpoint is born at the
 *      S3/S4 end-state, not migrating to it.
 *   2. Expiry is LONG (7 days), not unlock's 60s. Access removed late is still
 *      correct; access never removed because the queue row expired first is a
 *      security failure.
 *   3. No remote_unlock capability gate. A revoke is key management — even an
 *      eco/WiFi-direct lock with no bridge still syncs it on its own wake
 *      cycle. Gate on reachability only, same check /unlock uses before its
 *      (skipped-here) capability gate.
 */
const BOND_VERB_EXPIRY_MS = 7 * 24 * 3600 * 1000;

async function handleBondVerb(req, res, actionType, label) {
  if (!guardDb(res)) return;
  try {
    const deviceId = req.params.id;
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'envelope_hex is required' });
    }

    const [[lock]] = await pool.query('SELECT * FROM locks WHERE id = ?', [deviceId]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${deviceId} not found` });
    if (refuseIfOzkieV2(res, lock, deviceId, label)) return;

    const reachable = lock.status === 'enrolled' || (lock.bridge_id && lock.status === 'registered');
    if (!reachable) {
      return res.status(409).json({
        ok: false,
        code: 'lock_not_enrolled',
        error: `Lock ${deviceId} is not enrolled yet (status: ${lock.status})`,
        status_now: lock.status,
      });
    }

    const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
    const [queueResult] = await pool.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, NULL, ?, ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, lock.site_id, actionType, envelope_hex, expiresAt]
    );

    logEvent(
      'key',
      `${label} queued for "${lock.label}" (queue #${queueResult.insertId}, expires 7d)`
    );
    await recordAudit(lock.app_id, deviceId, actionType, label);
    const sent = await flushQueueForDevice(lock.site_id, deviceId);

    res.json({
      ok: true,
      device_id: deviceId,
      queue_id: queueResult.insertId,
      envelope_hex,
      delivery: sent > 0 ? 'delivered' : 'queued', // KEPT for compat, see /unlock
      transport_ok: sent > 0, // ozkey-20 R5 — the field to trust
      expires_at: expiresAt.toISOString(),
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
}

api.post('/locks/:id/bond-revoke', (req, res) => handleBondVerb(req, res, 'bond-revoke', 'Bond revoke'));
api.post('/locks/:id/invite-cancel', (req, res) =>
  handleBondVerb(req, res, 'invite-cancel', 'Invite cancel')
);
// ozkey-32 §4: a generic sealed-settings route (set_name is the first verb
// to need it, more of ozkey-28's settings verbs will follow) — server
// relays envelope_hex opaquely and never needs to know which settings verb
// is inside, so this is handleBondVerb with a new action_type, not a new
// mechanism.
api.post('/locks/:id/settings', (req, res) => handleBondVerb(req, res, 'settings', 'Settings'));

/** XF-93 (AZ) — remote bridge factory reset. `bridges` (ozkey-23 §10.2a)
 * exists now, but only for broker credentials — this route still reads and
 * writes nothing there; the audit trail is the only DB write. Deliberately
 * UNSEALED, matching firmware's own design (`bridge32.ino`, XF-93 §5): the
 * bridge holds no per-bond keys (a relay, not a crypto authority), so
 * `app_id` is checked bridge-side by `bridgeOwnershipCheck()`, not
 * cryptographically authenticated here. Built server-side rather than
 * letting the app publish directly, per XF-93 §7.3's reasoning: the handset
 * must never hold broker publish credentials, full stop — that outweighs
 * the convenience of a direct publish for an unsealed command.
 *
 * ozkey-P2, CORRECTED 2026-08-12 (courier rule, XF-84 — "the server knows
 * it sent the message; it does not know the bridge executed it"): this used
 * to return `ok:true` the moment the MQTT publish succeeded, which reports
 * the SERVER's success (the publish left) as if it were the BRIDGE's
 * (the reset happened) — exactly the class of bug XF-84 and this session's
 * `likely_delivered` fix (AW) both exist to stop. Now waits on the one real
 * signal that exists (`waitForBridgeResetVerdict()`, wired through
 * `handleBridgePresence()` above) instead of trusting the publish alone.
 *
 * `BRIDGE_DENIED` cannot be surfaced here — not a shortcut, a firmware gap,
 * confirmed by reading `bridge32.ino` (~718-730): a denied reset only
 * `Serial.printf()`s and `return`s; nothing is published over MQTT. A
 * denied reset and a reset the bridge never received are, on the wire,
 * IDENTICAL — both silence. Both correctly resolve to `unknown` here; they
 * cannot currently be told apart without a firmware change (bridge32
 * publishing something on refusal). Flagging rather than faking a
 * `BRIDGE_DENIED` verdict this server cannot actually detect.
 */
api.post('/bridges/:id/reset', async (req, res) => {
  try {
    const bridgeId = req.params.id;
    const { app_id } = req.body || {};
    if (!app_id) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'app_id is required' });
    }

    const transportOk = mqttPublish(CONFIG.topicBridgeCommand(CONFIG.SITE_ID, bridgeId), {
      op: 'factory_reset',
      app_id,
    });
    await recordAudit(app_id, bridgeId, 'bridge-reset', 'factory reset requested');

    if (!transportOk) {
      // Never reached the broker — genuinely nothing happened, not "unknown
      // whether something happened". Distinct from the timeout case below.
      logEvent('warn', `Bridge reset for "${bridgeId}" — MQTT publish failed (broker offline)`);
      return res.json({
        ok: true,
        bridge_id: bridgeId,
        verdict: 'unknown',
        cause: 'mqtt_publish_failed',
        transport_ok: false,
      });
    }

    const { verdict, cause } = await waitForBridgeResetVerdict(bridgeId, CONFIG.BRIDGE_RESET_TIMEOUT_MS);
    const causeNote =
      cause === 'timeout'
        ? ` (no factory_reset presence signal within ${CONFIG.BRIDGE_RESET_TIMEOUT_MS}ms)`
        : cause === 'offline_unrelated_reason'
          ? ' (bridge went offline for an unrelated/ambiguous reason before confirming)'
          : cause === 'presence_denied'
            ? ' (bridge32-1.34: bridgeOwnershipCheck() refused the app_id)'
            : '';
    logEvent('key', `Bridge reset for "${bridgeId}" -> ${verdict}${causeNote}`);
    res.json({ ok: true, bridge_id: bridgeId, verdict, cause, transport_ok: true });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

api.delete('/locks/:id/grants/:gid', async (req, res) => {
  if (!guardDb(res)) return;
  const conn = await pool.getConnection();
  try {
    const deviceId = req.params.id;
    const grantId = Number(req.params.gid);
    // S4 cutover, executed 2026-08-08: envelope_hex is required — ftpos
    // shipped sealed DPID 22/24 delete frames (revokeGrant() ->
    // sealDeletePin/sealDeleteRfid), buildDeleteFrame() is gone.
    const { envelope_hex } = req.body || {};
    if (!envelope_hex) {
      return res
        .status(400)
        .json({ ok: false, code: 'missing_fields', error: 'envelope_hex is required' });
    }

    const [[grant]] = await conn.query('SELECT * FROM grants WHERE id = ? AND device_id = ?', [
      grantId,
      deviceId,
    ]);
    // XF-155 (2026-09-03): a `grants` row can legitimately be gone (purged
    // on a lock delete/re-pair cycle, or never confirmed server-side to
    // begin with — the app's own local-history fallback assigns those a
    // synthetic id) while the LOCK still holds a live credential in that
    // slot. The server's bookkeeping row isn't what protects anything here
    // — the sealed envelope is, same trust relationship the S4 cutover
    // already established for grant creation (server relays opaque, never
    // gates on its own copy). Relay through regardless of whether `grants`
    // still has a matching row; only refuse when the LOCK itself doesn't
    // exist — there's genuinely nowhere to route the frame.
    let siteId;
    if (grant) {
      if (grant.sync_status === 'revoked')
        return res.status(409).json({
          ok: false,
          code: 'grant_already_revoked',
          error: `Grant #${grantId} is already revoked`,
        });
      siteId = grant.site_id;
    } else {
      const [[lock]] = await conn.query('SELECT site_id FROM locks WHERE id = ?', [deviceId]);
      if (!lock)
        return res.status(404).json({
          ok: false,
          code: 'device_not_found',
          error: `Lock ${deviceId} not found`,
        });
      siteId = lock.site_id;
    }
    const [[verRow]] = await conn.query('SELECT ozkie_version FROM locks WHERE id = ?', [deviceId]);
    if (refuseIfOzkieV2(res, verRow, deviceId, 'Grant revoke')) return;
    const [[dupe]] = await conn.query(
      `SELECT id FROM pending_queue
        WHERE grant_id = ? AND action_type = 'revoke-key' AND status = 'queued'`,
      [grantId]
    );
    if (dupe)
      return res.status(409).json({
        ok: false,
        code: 'revoke_already_queued',
        error: `Grant #${grantId} already has revoke queue #${dupe.id} pending`,
        queue_id: dupe.id,
      });

    // XF-155 (2026-09-03): revoke-key jobs previously had no expires_at at
    // all — a job that could never be delivered (server's MQTT connection
    // down at request time, or the lock permanently gone) sat 'queued'
    // forever with nothing surfacing it as a failure. Give it the same
    // expiry shape the bond verbs already use.
    const expiresAt = new Date(Date.now() + BOND_VERB_EXPIRY_MS);
    await conn.beginTransaction();
    const [queueResult] = await conn.query(
      `INSERT INTO pending_queue (device_id, site_id, grant_id, action_type, envelope_hex, msg_type, status, expires_at)
       VALUES (?, ?, ?, 'revoke-key', ?, 'sealed_envelope', 'queued', ?)`,
      [deviceId, siteId, grantId, envelope_hex, expiresAt]
    );
    if (grant) {
      await conn.query("UPDATE grants SET sync_status = 'revoking' WHERE id = ?", [grantId]);
    }
    await conn.commit();

    logEvent(
      'key',
      grant
        ? `Revoking ${grant.type.toUpperCase()} on ${deviceId} slot ${grant.slot_number} ` +
            `(grant #${grantId}, queue #${queueResult.insertId}, sealed) — awaiting wake`
        : `Revoking grant #${grantId} on ${deviceId} (no matching grants row — envelope ` +
            `authoritative, queue #${queueResult.insertId}, sealed) — awaiting wake`
    );
    await recordAudit(
      null,
      deviceId,
      'revoke',
      grant
        ? `revoke ${grant.type.toUpperCase()} slot ${grant.slot_number}`
        : `revoke grant #${grantId} (no server record)`
    );

    flushQueueForDevice(siteId, deviceId).catch(() => {});

    res.json({
      ok: true,
      grant_id: grantId,
      queue_id: queueResult.insertId,
      device_id: deviceId,
      envelope_hex,
      sync_status: 'revoking',
      expires_at: expiresAt.toISOString(),
    });
  } catch (err) {
    try {
      await conn.rollback();
    } catch (_) {
      /* connection already dead */
    }
    res.status(500).json({ ok: false, error: err.message });
  } finally {
    conn.release();
  }
});

/* -- Introspection + terminal feed --------------------------------------------- */
api.get('/queue', async (req, res) => {
  if (!guardDb(res)) return;
  try {
    const [rows] = await pool.query('SELECT * FROM pending_queue ORDER BY id DESC LIMIT 100');
    res.json({ ok: true, queue: rows });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

/* -- Lab simulator hook: fake a wake without the broker ------------------------ */
api.post('/sim/heartbeat', async (req, res) => {
  if (!guardDb(res)) return;
  const { device_id } = req.body || {};
  if (!device_id)
    return res.status(400).json({ ok: false, code: 'missing_fields', error: 'device_id required' });
  try {
    const [[lock]] = await pool.query('SELECT site_id FROM locks WHERE id = ?', [device_id]);
    if (!lock)
      return res
        .status(404)
        .json({ ok: false, code: 'lock_not_found', error: `Lock ${device_id} not found` });
    await pool.query('UPDATE locks SET last_seen_at = NOW() WHERE id = ?', [device_id]);
    const sent = await flushQueueForDevice(lock.site_id, String(device_id));
    res.json({ ok: true, device_id, flushed: sent });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});
/**
 * Put the lock API on the profile's express Router.
 *
 * @param {object} router            express.Router()
 * @param {object} opts
 * @param {Function} opts.auth       (req, res, next) middleware run before every
 *                                   route here. REQUIRED: there is no default,
 *                                   so a profile can't get the lock API without
 *                                   saying how it is protected (fail closed, as
 *                                   S−1).
 * @param {Function} [opts.onLockRemoved] (lock) after DELETE /locks/:id removed
 *                                   the row; fire-and-forget (ozlock: Nexus).
 * @param {Function} [opts.purgeExtraRows] (conn, where, args) inside the removal
 *                                   transaction, for the profile's own per-lock
 *                                   tables (ozpms: lock_logs).
 */
function mountLockApi(router, opts = {}) {
  if (typeof opts.auth !== 'function') {
    throw new Error('ozserver-core: mountLockApi() needs an auth middleware (opts.auth); refusing to mount');
  }
  if (!ctx.CONFIG) throw new Error('ozserver-core: mountLockApi() before createCore()');
  hooks = { onLockRemoved: opts.onLockRemoved || null, purgeExtraRows: opts.purgeExtraRows || null };
  for (const { method, path, handlers } of routes) router[method](path, opts.auth, ...handlers);
}

module.exports = {
  mountLockApi,
  registerPairing,
  buildProvisionPayload,
  pageParams,
  rangeWhere,
};
