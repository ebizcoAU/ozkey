'use strict';
/*
 * S1c/S1d (ozkey-50 §7): ozpms has no REST auth yet (ozkey-51), so it must
 * refuse to start unless the lab opts in with OZPMS_LAB_OPEN_API=1, and
 * always in production. The refusal happens while server.js loads, before
 * any DB, MQTT or HTTP, so these runs touch nothing. The child gets a clean
 * env with every required secret set; only the opt-in varies. A .env beside
 * server.js never overrides a var that is already set, and an empty value
 * counts as set, so the lab .env can't mask the case under test.
 */

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('child_process');
const path = require('path');

const SERVER_DIR = path.join(__dirname, '..');

function boot(extra) {
  const r = spawnSync(process.execPath, ['server.js'], {
    cwd: SERVER_DIR,
    timeout: 15000,
    encoding: 'utf8',
    env: {
      PATH: process.env.PATH,
      HOME: process.env.HOME,
      OZPMS_DB_PASSWORD: 'unused-refusal-happens-before-the-db',
      OZPMS_DB_NAME: 's1_ozpms_never_created',
      OZPMS_MQTT_URL: 'mqtt://127.0.0.1:1',
      OZPMS_HTTP_PORT: '1',
      PROVISION_WIFI_SSID: 'test-ssid',
      PROVISION_WIFI_PASSWORD: 'test-pass',
      ...extra,
    },
  });
  return { code: r.status, out: `${r.stdout}${r.stderr}` };
}

for (const [name, extra] of [
  ['opt-in empty', { OZPMS_LAB_OPEN_API: '' }],
  ['opt-in 0', { OZPMS_LAB_OPEN_API: '0' }],
  ['opt-in "true" (only the exact value 1 counts)', { OZPMS_LAB_OPEN_API: 'true' }],
  ['production even with the opt-in', { OZPMS_LAB_OPEN_API: '1', NODE_ENV: 'production' }],
]) {
  test(`refuses to start: ${name}`, () => {
    const { code, out } = boot(extra);
    assert.equal(code, 1, out);
    assert.match(out, /OZPMS has no REST auth yet \(ozkey-51\); refusing to mount the lock API/);
    assert.doesNotMatch(out, /MySQL online|HTTP directory listening/);
  });
}
