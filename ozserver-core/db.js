'use strict';
/*
 * ozserver-core/db.js — MySQL bootstrap: creates the schema and the pool (ctx.pool).
 *
 * Moved verbatim from ozlock/server/server.js in S0b (ozkey-50 §7); comments
 * keep their original references. Dependencies come from createCore() via
 * ./ctx (see index.js); other core modules are reached through late-bound
 * forwarders so load order never matters.
 */

const { ctx, lazy } = require('./ctx');
const CONFIG = lazy(() => ctx.CONFIG);
const pool = lazy(() => ctx.pool);
const mysql = lazy(() => ctx.mysql);
const logEvent = (...args) => ctx.logEvent(...args);

async function initDatabase() {
  const admin = await mysql.createConnection({
    host: CONFIG.DB.host,
    user: CONFIG.DB.user,
    password: CONFIG.DB.password,
    multipleStatements: false,
  });
  await admin.query(
    `CREATE DATABASE IF NOT EXISTS \`${CONFIG.DB.database}\`
     CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`
  );
  await admin.end();

  ctx.pool = mysql.createPool(CONFIG.DB);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS owners (
      id INT AUTO_INCREMENT PRIMARY KEY,
      display_name VARCHAR(255),
      banoi_sub VARCHAR(255) NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS sites (
      id VARCHAR(50) PRIMARY KEY,
      owner_id INT,
      label VARCHAR(255),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (owner_id) REFERENCES owners(id)
    ) ENGINE=InnoDB`);

  await pool.query(`
    CREATE TABLE IF NOT EXISTS locks (
      id VARCHAR(64) PRIMARY KEY,
      site_id VARCHAR(50),
      app_id VARCHAR(80) NULL,
      bridge_id VARCHAR(64) NULL,
      mac VARCHAR(17),
      label VARCHAR(255) DEFAULT 'New Doorlock',
      fw VARCHAR(50) NULL,
      status VARCHAR(20) DEFAULT 'enrolled',
      power_profile VARCHAR(20) DEFAULT 'eco',
      -- XF-48 Ask 1: the lock's OWN statement of what it can do, as a JSON
      -- array e.g. ["remote_unlock","pin_sync","audit"]. NULL until firmware
      -- reports it (M3+); until then capability is inferred — see
      -- effectiveCaps(). Never infer once the device has spoken.
      caps VARCHAR(255) NULL,
      heartbeat_s INT DEFAULT 60,
      broker_username VARCHAR(64),
      broker_secret VARCHAR(64),
      last_seen_at DATETIME NULL,
      enrolled_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (site_id) REFERENCES sites(id)
    ) ENGINE=InnoDB`);

  // app_id = the paired app's self-generated identity (trust-model v2, ozkey-05
  // amendment / XF-42 §13). Additive migration for pre-v2 lab rows.
  const [[{ hasAppId }]] = await pool.query(
    `SELECT COUNT(*) AS hasAppId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'app_id'`,
    [CONFIG.DB.database]
  );
  if (!hasAppId) await pool.query('ALTER TABLE locks ADD COLUMN app_id VARCHAR(80) NULL AFTER site_id');

  // bridge_id = the bridge gateway a bridged (BLE-native) lock is reached
  // through (ozkey-11 §3). NULL = the lock is its own MQTT client (direct
  // Wi-Fi, modes 2b/3/4). Only the app learns this — it paired the lock to
  // the bridge over BLE at commissioning — so it must tell us at /pairings
  // or the lock is unreachable.
  const [[{ hasBridgeId }]] = await pool.query(
    `SELECT COUNT(*) AS hasBridgeId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'bridge_id'`,
    [CONFIG.DB.database]
  );
  if (!hasBridgeId)
    await pool.query('ALTER TABLE locks ADD COLUMN bridge_id VARCHAR(64) NULL AFTER app_id');

  // XF-48 Ask 1 / ask (E). Additive migration for existing lab rows.
  const [[{ hasCaps }]] = await pool.query(
    `SELECT COUNT(*) AS hasCaps FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'caps'`,
    [CONFIG.DB.database]
  );
  if (!hasCaps)
    await pool.query('ALTER TABLE locks ADD COLUMN caps VARCHAR(255) NULL AFTER power_profile');

  // XF-57 (AN), operator directive 2026-08-03. The lock's OWN statement of its
  // transport, reported on every enroll and heartbeat. Before this the server
  // never knew — it inferred capability from `bridge_id`, and the app kept a
  // private copy written at commissioning that nothing corrected, so a lock
  // whose real transport changed stayed misreported in the app permanently
  // and every remote unlock was refused. `wifi` | `ble`, NULL until the
  // device reports (XF-138 P5: `thread` retired along with Thread support).
  const [[{ hasTransport }]] = await pool.query(
    `SELECT COUNT(*) AS hasTransport FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'transport'`,
    [CONFIG.DB.database]
  );
  if (!hasTransport)
    await pool.query('ALTER TABLE locks ADD COLUMN transport VARCHAR(16) NULL AFTER power_profile');

  // ozkey-20 R5 (liveness/health/fault-attribution, 2026-08-10): observed
  // state, replacing the likelyOnline() inference. `presence`/
  // `presence_reason`/`presence_at` are R6's fault-attribution OUTPUT
  // (computeFaultAttribution() below writes them), not a raw signal —
  // `presence_reason` holds one of the R6 table's verdict strings
  // (bridge_offline, lock_unreachable, battery_low, ...). Inputs feeding it
  // (R1 presence topics, R4 heartbeat health fields) don't fully exist on
  // the wire yet — this is the receiving side built ahead of the sender,
  // same pattern as SUB_UPLINK before V1 shipped, so it lights up with zero
  // further server change once firmware catches up.
  const [[{ hasPresence }]] = await pool.query(
    `SELECT COUNT(*) AS hasPresence FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'presence'`,
    [CONFIG.DB.database]
  );
  if (!hasPresence)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN presence         ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
        ADD COLUMN presence_reason  VARCHAR(32) NULL,
        ADD COLUMN presence_at      DATETIME NULL,
        ADD COLUMN battery_pct      TINYINT NULL,
        ADD COLUMN pending_uplinks  SMALLINT NOT NULL DEFAULT 0,
        ADD COLUMN roster_epoch     INT UNSIGNED NOT NULL DEFAULT 0
    `);

  // XF-146 (2026-09-01, found live on LockA — real bug, not theoretical):
  // `presence` used to serve two purposes that turned out to conflict —
  // the RAW signal `handleLockPresence()` writes directly, and the DERIVED
  // verdict `recomputeAndStorePresence()` computes from it — sharing one
  // column. A bridge's own brief MQTT reconnect blip (its LWT firing and
  // clearing seconds later, nothing to do with the actual BLE link to the
  // lock) triggered two re-verdicts back to back: the first correctly read
  // `bridge_offline` and wrote `presence='offline'`; the second, once the
  // bridge recovered, re-read `lock.presence` for its "is there still a
  // direct signal" check — and got back its OWN prior write, misread as a
  // fresh direct report, permanently re-confirming `lock_unreachable` with
  // no path back except a brand-new physical reconnect (which, confirmed
  // on firmware's own serial log, never even happened — the real BLE link
  // was healthy throughout). `direct_presence` is the fix: the raw signal
  // now lives in its own column, written ONLY by `handleLockPresence()`,
  // never touched by the derivation step — so a transient bridge-level
  // blip can no longer permanently poison what the lock itself last
  // reported. Backfilled from `presence` so no lock reads as spuriously
  // unknown right after this migration runs.
  const [[{ hasDirectPresence }]] = await pool.query(
    `SELECT COUNT(*) AS hasDirectPresence FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'direct_presence'`,
    [CONFIG.DB.database]
  );
  if (!hasDirectPresence) {
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN direct_presence ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown'
    `);
    await pool.query(`UPDATE locks SET direct_presence = presence`);
  }

  // XF-144 (2026-09-01, PM directive): multi-bridge failover. Deliberately
  // NOT replacing `locks.bridge_id` — per this session's own recommendation
  // (agreed by PM): every reachability/queue/caps code path already keys
  // off that single column, tested and working live. `bridge_id` stays the
  // denormalized "currently active" pointer; this table is the additive
  // ranked-candidate list failover reads from. Composite PK (lock_id,
  // bridge_id) — a lock can appear once per bridge it's ever been paired
  // to, priority 0 = current/primary, higher = older/fallback.
  const [[{ hasLockBridges }]] = await pool.query(
    `SELECT COUNT(*) AS hasLockBridges FROM information_schema.tables
      WHERE table_schema = ? AND table_name = 'lock_bridges'`,
    [CONFIG.DB.database]
  );
  if (!hasLockBridges) {
    await pool.query(`
      CREATE TABLE lock_bridges (
        lock_id                      VARCHAR(64) NOT NULL,
        bridge_id                    VARCHAR(64) NOT NULL,
        priority                     SMALLINT NOT NULL DEFAULT 0,
        last_confirmed_reachable_at  DATETIME NULL,
        PRIMARY KEY (lock_id, bridge_id),
        INDEX idx_lock_priority (lock_id, priority)
      ) ENGINE=InnoDB
    `);
    // Non-destructive backfill (XF-144's own migration requirement):
    // every lock's CURRENT bridge_id becomes its priority-0 row — existing
    // single-bridge assignments are represented exactly as they are today,
    // nothing is inferred or guessed for locks that have never had a
    // second bridge.
    await pool.query(`
      INSERT INTO lock_bridges (lock_id, bridge_id, priority, last_confirmed_reachable_at)
      SELECT id, bridge_id, 0, presence_at FROM locks WHERE bridge_id IS NOT NULL
    `);
  }

  // last_mech_result/last_mech_at are NOT in ozkey-20's own R5 schema block,
  // even though R6's attribution table references last_mech_result — a real
  // gap between R5 and R6 as specified. Filling it rather than silently
  // working around it: these store R4's heartbeat field once it exists.
  // Guarded separately from the block above: an earlier restart could have
  // already added `presence` before this block existed, which would leave
  // this guard permanently false if it shared the `hasPresence` flag.
  const [[{ hasMechResult }]] = await pool.query(
    `SELECT COUNT(*) AS hasMechResult FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'last_mech_result'`,
    [CONFIG.DB.database]
  );
  if (!hasMechResult)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN last_mech_result VARCHAR(16) NULL,
        ADD COLUMN last_mech_at     DATETIME NULL
    `);

  // ozkey-20 §5a (added 2026-08-11 on firmware review) — the DL MCU link was
  // missing from the whole model. mcu_link_up is the R6 verdict input
  // (mcu_link_down ranks above battery_low); mcu_last_frame_s is kept for
  // observability only. Own guard, same reason as hasMechResult above — do
  // not fold into an existing flag.
  const [[{ hasMcuLink }]] = await pool.query(
    `SELECT COUNT(*) AS hasMcuLink FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'mcu_link_up'`,
    [CONFIG.DB.database]
  );
  if (!hasMcuLink)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN mcu_link_up      TINYINT(1) NULL,
        ADD COLUMN mcu_last_frame_s SMALLINT NULL
    `);

  // XF-119 §6/§9.3 ask 2: the RAW `reason` off the most recent
  // `locks/<id>/presence` message, distinct from `presence_reason` (which is
  // computeFaultAttribution()'s own derived R6 vocabulary and does not
  // include `factory_reset` at all — confirmed while building this, see
  // handleLockPresence()). Lets DELETE /locks/:id answer "has this lock
  // already told us it reset?" from state already held, instead of opening
  // a reset-wait window nothing can ever close for a lock that reset itself
  // (BOOT hold, DL-MCU button) with the app never involved. Cleared to NULL
  // by the same handler whenever a message carries no `reason` (the
  // canonical `online` message per ozpresence.h) — so a stale factory_reset
  // cannot outlive the lock coming back, same non-staleness property XF-119
  // §6 already relies on for the retained MQTT value itself.
  const [[{ hasLastResetReason }]] = await pool.query(
    `SELECT COUNT(*) AS hasLastResetReason FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'last_reset_reason'`,
    [CONFIG.DB.database]
  );
  if (!hasLastResetReason)
    await pool.query('ALTER TABLE locks ADD COLUMN last_reset_reason VARCHAR(32) NULL');

  // XF-122 §5 ask 3: there is no generic Tuya DP map — DP numbers mean
  // different things per product (DP 76 is `unlock_ble` on Luona,
  // `fill_light` on Tuya's own Wi-Fi-Lock-Pro standard). `profile` names
  // which pinned DP map the lock's firmware build resolves verbs against
  // (`tuya-luona-ds013-t3`, …); `tuya_pid` is the lock's own MCU-reported
  // product id, used to detect a build/PID mismatch. Both ride the enroll
  // payload (`handleEnroll()`) alongside the existing fw/caps/verbs fields.
  // The app needs them to survive its own restart without re-pairing —
  // it shows "Detected: <model>" from `tuya_pid` and must not have to talk
  // to the lock again just to redraw that screen (XF-122 §7).
  const [[{ hasTuyaPid }]] = await pool.query(
    `SELECT COUNT(*) AS hasTuyaPid FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'tuya_pid'`,
    [CONFIG.DB.database]
  );
  if (!hasTuyaPid)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN tuya_pid VARCHAR(32) NULL,
        ADD COLUMN profile  VARCHAR(64) NULL
    `);

  // XF-125 P0 (server half): the lock's audit ring is pull-only (F2/F9) —
  // it keeps every event and waits for `query_events`, which is a BLE-only
  // exchange the server never sees (confirmed by reading
  // ozdoorlock_core.h:6103-6161 — the response goes out over
  // ozNotifySealedTo(), not MQTT). Nothing anywhere checks that a reader
  // still exists, and an app's MQTT session can die silently while events
  // pile up unread. These three columns are what makes that detectable:
  //   seq_highwater      last seq the lock has WRITTEN (its hb["seq_highwater"])
  //   dropped_before_seq oldest seq rotation destroyed (its hb["dropped_before_seq"])
  //   last_pulled_seq     highest seq any app has CONFIRMED pulling (ours —
  //                       PATCH /locks/:id `last_pulled_seq`, since there's
  //                       no wire signal to observe: query_events never
  //                       touches MQTT at all)
  // A lock is at risk exactly when dropped_before_seq has passed
  // last_pulled_seq — records were destroyed that nobody ever confirmed
  // reading. See eventsAtRisk() below.
  const [[{ hasSeqHighwater }]] = await pool.query(
    `SELECT COUNT(*) AS hasSeqHighwater FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'seq_highwater'`,
    [CONFIG.DB.database]
  );
  if (!hasSeqHighwater)
    await pool.query(`
      ALTER TABLE locks
        ADD COLUMN seq_highwater      INT UNSIGNED NULL,
        ADD COLUMN dropped_before_seq INT UNSIGNED NULL,
        ADD COLUMN last_pulled_seq    INT UNSIGNED NULL
    `);

  // XF-127: the lock has reported `has_doorbell` on every heartbeat since
  // XF-107, derived from its own DP map (`tuya-luona-ds013-t3` selects DP
  // 53) — not a guess. Server never stored or served it, so the app (which
  // has no other source and defaults to "no doorbell" on purpose — a false
  // positive strands someone at the door, a false negative only costs a
  // step) could never offer a remote wake for a Wi-Fi lock that actually
  // has one. Blocked verifying XF-126's safety fix, which needs a wake path
  // to even reach the assisted-unlock gate under test.
  const [[{ hasDoorbellCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasDoorbellCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'has_doorbell'`,
    [CONFIG.DB.database]
  );
  if (!hasDoorbellCol)
    await pool.query('ALTER TABLE locks ADD COLUMN has_doorbell TINYINT(1) NULL');

  // XF-127 §8 ask 2 (the audit) turned this one up too, same shape as
  // has_doorbell: already on the wire for Thread locks (bridge32's verbatim
  // beacon relay), never ingested. `g_profileMismatch` is set when the Tuya
  // 0x08 DP census finds hardware that disagrees with the pinned profile —
  // exactly the "DP 76 means unlock on Luona, a lamp on Tuya-standard"
  // class of danger XF-122 exists to prevent, so it's worth closing
  // alongside has_doorbell rather than only reporting it.
  const [[{ hasProfileMismatchCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasProfileMismatchCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'profile_mismatch'`,
    [CONFIG.DB.database]
  );
  if (!hasProfileMismatchCol)
    await pool.query('ALTER TABLE locks ADD COLUMN profile_mismatch TINYINT(1) NULL');

  // XF-134: `bonds` is a device fact (count of entries in the lock's own
  // NVS bond table), already live on the wire for Thread locks today —
  // same verbatim-beacon-relay-onto-heartbeat mechanism as
  // seq_highwater/has_doorbell/profile_mismatch, just never ingested.
  // DIAGNOSTIC ONLY, per XF-133's own reply: a count has no per-member
  // identity, so it can never answer "does member X hold a bond" — that
  // still needs the membership roster XF-129 §10 established doesn't
  // exist. This is "how many keys exist on this door right now," useful
  // for an ops/support view, never a security decision input.
  const [[{ hasBondsCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasBondsCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'bonds'`,
    [CONFIG.DB.database]
  );
  if (!hasBondsCol) await pool.query('ALTER TABLE locks ADD COLUMN bonds TINYINT UNSIGNED NULL');

  // ozkey-48 §7 (OZKIE v2, operator-approved 2026-10-18): which protocol the
  // lock speaks. 1 = every lock that exists today (ESP32 / CH584M era, sealed
  // JSON envelopes); 2 = AK1037 and later (EDHOC + OSCORE). The app learns it
  // from the GATT service UUID at pairing and tells us at POST /pairings —
  // the server has no way to tell on its own, since it never opens either
  // kind of payload. Gates every relay route below via `isOzkieV2()`.
  const [[{ hasOzkieVersionCol }]] = await pool.query(
    `SELECT COUNT(*) AS hasOzkieVersionCol FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'locks' AND column_name = 'ozkie_version'`,
    [CONFIG.DB.database]
  );
  if (!hasOzkieVersionCol)
    await pool.query('ALTER TABLE locks ADD COLUMN ozkie_version TINYINT UNSIGNED NOT NULL DEFAULT 1');

  await pool.query(`
    CREATE TABLE IF NOT EXISTS bridges_presence (
      bridge_id   VARCHAR(64) PRIMARY KEY,
      presence    ENUM('online','offline','unknown') NOT NULL DEFAULT 'unknown',
      presence_at DATETIME NULL
    ) ENGINE=InnoDB`);

  // ozkey-23 §10.1a/10.2a: bridges were structurally unauthenticatable — no
  // column existed anywhere to hold broker credentials for one, so firmware
  // (bridge32.ino) had nothing to present even after the lock-side bug was
  // fixed. This table exists ONLY to close that gap (credential storage);
  // it is deliberately not a bridge "registry" — (AZ)'s factory-reset route
  // still needs no row, and bridge identity/liveness stay owned by
  // bridges_presence/the Thread-liveness table, not here. A row is created
  // the first time a bridge is seen online (handleBridgePresence()), the
  // same "device_id IS the bearer handle" trust model locks already use.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS bridges (
      id              VARCHAR(64) PRIMARY KEY,
      broker_username VARCHAR(64),
      broker_secret   VARCHAR(64),
      first_seen_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // ozkey-23 §10.1a/10.2a, XF-96 §4/§6: BANOI has the same gap bridges had —
  // ftpos confirmed the app connects with client-id only, no username/
  // password, because no server-side mint exists for it either.
  // ozkey-24, CORRECTED 2026-08-12 by firmware (§9.2, verified against
  // blelock/common/ozcrypto.h before accepting): `app_id` is not a bearer
  // string alongside a separate X25519 identity — it IS the identity,
  // hex-encoded. `ozBond0Evaluate()` (ozcrypto.h:414) hex-decodes `app_id`
  // directly into the 32 raw pubkey bytes it bonds against; the bond struct
  // says the same thing explicitly (`uint8_t pub[32]; // the member's
  // X25519 public key == its app_id`, ozcrypto.h:260). Every lock a member
  // has ever bonded to already trusts these exact bytes. So there is no
  // second key to register and no `apps` table to hold one — `locks.app_id`
  // (written by registerPairing() at `POST /pairings`, since before this
  // file could spell "X25519") has been an app public-key store from day
  // one, without anyone having named it that. The `apps` table and
  // `POST /apps/register` this comment used to describe are GONE — first
  // written, then reverted the same day, before either ran against
  // production. Only the challenge/token/JWT flow below survived, reading
  // `locks.app_id` directly instead of a dedicated table.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS enroll_tokens (
      token VARCHAR(64) PRIMARY KEY,
      site_id VARCHAR(50),
      owner_id INT,
      app_id VARCHAR(80) NULL,
      label VARCHAR(255) NULL,
      expires_at DATETIME,
      used_at DATETIME NULL,
      device_id VARCHAR(64) NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // MUST run AFTER the CREATE above. Ordered the other way round originally,
  // which worked only because this deployment already had the table: on a FRESH
  // schema the information_schema probe returns 0 (no table, so no column) and
  // the ALTER then fails with "Table 'x.enroll_tokens' doesn't exist" — an init
  // loop that never converges, so a brand-new install could never boot. Found
  // 2026-07-31 bootstrapping ozpms from this same code. The `locks` probe above
  // is safe only because `locks` is created earlier; keep every migration below
  // the CREATE it depends on.
  const [[{ hasTokenAppId }]] = await pool.query(
    `SELECT COUNT(*) AS hasTokenAppId FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'enroll_tokens' AND column_name = 'app_id'`,
    [CONFIG.DB.database]
  );
  if (!hasTokenAppId)
    await pool.query('ALTER TABLE enroll_tokens ADD COLUMN app_id VARCHAR(80) NULL AFTER owner_id');

  await pool.query(`
    CREATE TABLE IF NOT EXISTS grants (
      id INT AUTO_INCREMENT PRIMARY KEY,
      device_id VARCHAR(64),
      site_id VARCHAR(50),
      type ENUM('pin','rfid','fingerprint'),
      slot_number INT,
      sync_status VARCHAR(50) DEFAULT 'pending',
      issued_by VARCHAR(50) DEFAULT 'owner',
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // S3 cutover (ozkey-13 §10 phase 4, XF-69), executed 2026-08-08:
  // migrations/S3_drop_raw_value.sql, folded in here so every environment
  // running this code — not just the one it was run against by hand —
  // converges to the same schema. Irreversible on purpose: no plaintext
  // PIN/RFID should be recoverable from server-side storage after cutover.
  const [[{ hasRawValue }]] = await pool.query(
    `SELECT COUNT(*) AS hasRawValue FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'grants' AND column_name = 'raw_value'`,
    [CONFIG.DB.database]
  );
  if (hasRawValue) await pool.query('ALTER TABLE grants DROP COLUMN raw_value');

  // ozkey-29 cutover, executed 2026-08-13 (operator: "the residential server
  // retains nothing beyond the pairing relationship" — same irreversible-on-
  // purpose posture as S3 above, same table, next column over). `user_name`
  // was never the server's to hold — the lock never receives it either
  // (ozkey-29 §11.1, verified against ozdoorlock_core.h), so there was no
  // path where storing it server-side did anything but create the
  // transaction log the Sovereign Edge paper commits to never having.
  // `date_from`/`date_to` are dropped alongside it — confirmed redundant,
  // never read back by any server logic (XF-95 §2); the lock already
  // receives and enforces its own copy inside the sealed credential
  // envelope. Queue expiry (where it exists) uses `pending_queue.expires_at`,
  // untouched by this.
  for (const col of ['user_name', 'date_from', 'date_to']) {
    const [[{ hasCol }]] = await pool.query(
      `SELECT COUNT(*) AS hasCol FROM information_schema.columns
        WHERE table_schema = ? AND table_name = 'grants' AND column_name = ?`,
      [CONFIG.DB.database, col]
    );
    if (hasCol) await pool.query(`ALTER TABLE grants DROP COLUMN ${col}`);
  }

  await pool.query(`
    CREATE TABLE IF NOT EXISTS pending_queue (
      id INT AUTO_INCREMENT PRIMARY KEY,
      device_id VARCHAR(64),
      site_id VARCHAR(50),
      grant_id INT NULL,
      action_type VARCHAR(50),
      payload_hex TEXT,
      status VARCHAR(50) DEFAULT 'queued',
      expires_at DATETIME NULL,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // ozkey-13 §8 S2/S6 (relay-opaque migration, XF-69): a sealed-envelope job
  // (app built + AES-GCM sealed the DP frame client-side) carries an opaque
  // envelope instead of a server-built one. `envelope_hex` holds it;
  // `msg_type` says which of payload_hex/envelope_hex is live for this row so
  // flushQueueForDevice() doesn't have to guess from NULL-ness. Additive
  // migration, same pattern as the `locks` columns above — MUST run after the
  // CREATE it depends on (see the enroll_tokens comment for why).
  const [[{ hasEnvelopeHex }]] = await pool.query(
    `SELECT COUNT(*) AS hasEnvelopeHex FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'pending_queue' AND column_name = 'envelope_hex'`,
    [CONFIG.DB.database]
  );
  if (!hasEnvelopeHex)
    await pool.query('ALTER TABLE pending_queue ADD COLUMN envelope_hex TEXT NULL AFTER payload_hex');

  const [[{ hasMsgType }]] = await pool.query(
    `SELECT COUNT(*) AS hasMsgType FROM information_schema.columns
      WHERE table_schema = ? AND table_name = 'pending_queue' AND column_name = 'msg_type'`,
    [CONFIG.DB.database]
  );
  if (!hasMsgType)
    await pool.query(
      "ALTER TABLE pending_queue ADD COLUMN msg_type VARCHAR(20) NOT NULL DEFAULT 'legacy_payload' AFTER envelope_hex"
    );

  // `lock_logs` is NOT created here — removed 2026-07-31, see the file header.
  // Any rows an older build left in this schema are deliberately NOT dropped by
  // code: destroying an operator's existing data is their decision, not a
  // side effect of a deploy. Drop it by hand when you are satisfied it is
  // migrated:  DROP TABLE lock_logs;

  // App-attributed control-plane audit trail: every action an app performs
  // through OZLOCK (register pairing, grant/revoke a key, remote unlock,
  // settings). This is "what an app did", NOT "what a door did" — it is the
  // §4.1 "security events" class and is RETAINED here on purpose: without it
  // an abuse report is uninvestigable. Kept deliberately when `lock_logs` went.
  await pool.query(`
    CREATE TABLE IF NOT EXISTS audit_log (
      id INT AUTO_INCREMENT PRIMARY KEY,
      app_id VARCHAR(80) NULL,
      device_id VARCHAR(64) NULL,
      site_id VARCHAR(50),
      action VARCHAR(30),
      detail VARCHAR(255),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ) ENGINE=InnoDB`);

  // Seed the single lab owner + site (ozkey-05 lab simplification).
  const [[{ ownerCnt }]] = await pool.query('SELECT COUNT(*) AS ownerCnt FROM owners');
  if (ownerCnt === 0) {
    const [r] = await pool.query(
      "INSERT INTO owners (display_name, banoi_sub) VALUES ('BANOI Lab Owner', 'banoi:usr_lab')"
    );
    await pool.query('INSERT INTO sites (id, owner_id, label) VALUES (?, ?, ?)', [
      CONFIG.SITE_ID,
      r.insertId,
      'Lab Home',
    ]);
    logEvent('info', `Seeded owner "BANOI Lab Owner" + site '${CONFIG.SITE_ID}'`);
  }

  logEvent('info', `MySQL online — ${CONFIG.DB.host}/${CONFIG.DB.database}`);
}

module.exports = {
  initDatabase,
};
