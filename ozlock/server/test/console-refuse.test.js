'use strict';
/*
 * C0a: the console config is fail-closed. ozlock must refuse to boot,
 * before any DB, MQTT or HTTP, when the console's TOTP key or public URL is
 * missing or weak. The pattern is ozpms refuse.test.js. Every var is set
 * explicitly (an empty value counts as set), so the lab .env can't mask a
 * case.
 */

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('child_process');
const os = require('os');
const path = require('path');

const SERVER_DIR = path.join(__dirname, '..');
const GOOD_KEY = 'ab'.repeat(32);

function boot(extra) {
  const r = spawnSync(process.execPath, ['server.js'], {
    cwd: SERVER_DIR,
    timeout: 60000,
    encoding: 'utf8',
    env: {
      PATH: process.env.PATH,
      HOME: process.env.HOME,
      OZLOCK_DB_PASSWORD: 'unused-refusal-happens-before-the-db',
      OZLOCK_DB_NAME: 'c0_ozlock_never_created',
      OZLOCK_MQTT_URL: 'mqtt://127.0.0.1:1',
      OZLOCK_HTTP_PORT: '1',
      OZLOCK_JWT_SECRET: 'c0-refuse-test-jwt-secret-not-real-000000',
      PROVISION_WIFI_SSID: 'test-ssid',
      PROVISION_WIFI_PASSWORD: 'test-pass',
      OZLOCK_CONSOLE_TOTP_KEY: GOOD_KEY,
      OZLOCK_PUBLIC_URL: 'https://console.example.com',
      OZ_LAB_INSECURE: '',
      ...extra,
    },
  });
  const diag = `status=${r.status} signal=${r.signal} loadavg ${os.loadavg().map((l) => l.toFixed(1)).join('/')}`;
  return { code: r.status, out: `${diag}\n${r.stdout}${r.stderr}` };
}

for (const [name, extra, why] of [
  ['no TOTP key', { OZLOCK_CONSOLE_TOTP_KEY: '' }, /OZLOCK_CONSOLE_TOTP_KEY is missing or a placeholder/],
  ['a placeholder TOTP key', { OZLOCK_CONSOLE_TOTP_KEY: 'changeme' }, /OZLOCK_CONSOLE_TOTP_KEY is missing or a placeholder/],
  ['a short TOTP key', { OZLOCK_CONSOLE_TOTP_KEY: 'ab'.repeat(16) }, /OZLOCK_CONSOLE_TOTP_KEY must be 64 hex characters/],
  ['a non-hex TOTP key', { OZLOCK_CONSOLE_TOTP_KEY: 'zz'.repeat(32) }, /OZLOCK_CONSOLE_TOTP_KEY must be 64 hex characters/],
  ['no public URL', { OZLOCK_PUBLIC_URL: '' }, /OZLOCK_PUBLIC_URL is missing/],
  ['an http public URL', { OZLOCK_PUBLIC_URL: 'http://console.example.com' }, /OZLOCK_PUBLIC_URL must be https:\/\//],
  ['the http lab opt-in in production', { OZLOCK_PUBLIC_URL: 'http://127.0.0.1:4400', OZ_LAB_INSECURE: '1', NODE_ENV: 'production' }, /OZ_LAB_INSECURE=1 is refused in production/],
]) {
  test(`refuses to start: ${name}`, () => {
    const { code, out } = boot(extra);
    assert.equal(code, 1, out);
    assert.match(out, why, out);
    assert.match(out, /FATAL S−1: refusing to start/);
    assert.doesNotMatch(out, /MySQL online|HTTP directory listening/);
  });
}
