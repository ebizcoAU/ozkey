'use strict';
/*
 * ozserver-core — the lock side of the OZKIE servers, written once
 * (ozkey-50 §2): transport, registry, relay, queue, audit, DB bootstrap and
 * the lock-side REST API (api.js, S1b). Each product (ozlock now; ozpms and
 * ozlodge in S1/S2) is a profile that creates one core, mounts the lock API
 * with its own auth, and adds its own routes and tables.
 *
 * Everything the core needs from outside is the argument to createCore().
 * The core has no node_modules of its own: the profile passes in its mysql2
 * and mqtt so each app keeps owning its versions.
 *
 * S0b (ozkey-50 §7) moved this code out of ozlock/server/server.js verbatim;
 * behaviour is pinned by ozlock/server/test (golden transcripts).
 */

const { ctx } = require('./ctx');
const util = require('./util');
const audit = require('./audit');
const relay = require('./relay');
const queue = require('./queue');
const registry = require('./registry');
const transport = require('./transport');
const db = require('./db');
const api = require('./api');

/**
 * @param {object} deps
 * @param {object} deps.CONFIG    the profile's CONFIG (REQUIRED_CONFIG plus topics; see README.md)
 * @param {Function} deps.logEvent (level, message) => event
 * @param {object} deps.mysql     require('mysql2/promise')
 * @param {object} deps.mqtt      require('mqtt')
 */
// CONFIG keys the core reads that have no sensible shared default; a profile
// missing one fails at createCore(), not on the first message that needs it.
const REQUIRED_CONFIG = [
  'SITE_ID',
  'DB',
  'MQTT_URL',
  'SERVICE_NAME',
  'ENROLL_ISSUED_BY',
  'DEFAULT_HEARTBEAT_S',
  'FAILOVER_GRACE_MS',
  'FAILOVER_SWEEP_MS',
  'UTC_PUSH_REFRESH_MS',
  'AUDIT_LOG_RETENTION_MS',
  'ENFORCE_CAPS',
  'HASH_LOCK_BROKER_SECRETS',
  'PROVISION_WIFI_SSID',
  'PROVISION_WIFI_PASSWORD',
];

function createCore({ CONFIG, logEvent, mysql, mqtt }) {
  if (ctx.CONFIG) throw new Error('ozserver-core: createCore() called twice; one core per process');
  const missing = REQUIRED_CONFIG.filter((k) => CONFIG[k] === undefined);
  if (missing.length) throw new Error(`ozserver-core: CONFIG is missing ${missing.join(', ')}`);
  Object.assign(ctx, { CONFIG, logEvent, mysql, mqtt, pool: null });
  return {
    ...audit,
    ...relay,
    ...queue,
    ...registry,
    ...transport,
    ...db,
    ...api,
    /** The mysql2 pool, once initDatabase() has created it. */
    get pool() {
      return ctx.pool;
    },
  };
}

module.exports = { createCore, util };
