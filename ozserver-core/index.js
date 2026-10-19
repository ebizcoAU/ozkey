'use strict';
/*
 * ozserver-core — the lock side of the OZKIE servers, written once
 * (ozkey-50 §2): transport, registry, relay, queue, audit, DB bootstrap.
 * Each product (ozlock now; ozpms and ozlodge in S1/S2) is a profile that
 * creates one core and adds its own routes and tables.
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

/**
 * @param {object} deps
 * @param {object} deps.CONFIG    the profile's CONFIG (DB, MQTT_URL, SITE_ID, topics, timeouts)
 * @param {Function} deps.logEvent (level, message) => event
 * @param {object} deps.mysql     require('mysql2/promise')
 * @param {object} deps.mqtt      require('mqtt')
 */
function createCore({ CONFIG, logEvent, mysql, mqtt }) {
  if (ctx.CONFIG) throw new Error('ozserver-core: createCore() called twice; one core per process');
  Object.assign(ctx, { CONFIG, logEvent, mysql, mqtt, pool: null });
  return {
    ...audit,
    ...relay,
    ...queue,
    ...registry,
    ...transport,
    ...db,
    /** The mysql2 pool, once initDatabase() has created it. */
    get pool() {
      return ctx.pool;
    },
  };
}

module.exports = { createCore, util };
