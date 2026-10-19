'use strict';
/*
 * ozserver-core/ctx.js — the one place core dependencies live. createCore()
 * fills it once per process; every core module reads it lazily, so a module
 * can be required before createCore() runs (ozlock's CONFIG needs util at
 * load time) and pool/mqtt clients created later are seen everywhere.
 */

const ctx = {
  CONFIG: null, // the profile's CONFIG object
  logEvent: null, // (level, message) => event
  mysql: null, // mysql2/promise
  mqtt: null, // mqtt
  pool: null, // set by db.initDatabase()
};

/** A stand-in that forwards every property access to `get()` at use time. */
function lazy(get) {
  return new Proxy(
    {},
    {
      get(_, key) {
        const target = get();
        if (!target) throw new Error(`ozserver-core: '${String(key)}' used before it was set up`);
        const value = target[key];
        return typeof value === 'function' ? value.bind(target) : value;
      },
    }
  );
}

module.exports = { ctx, lazy };
