'use strict';
/**
 * OZLODGE-BROKER-ENV-1: ozlodge takes its broker from OZLODGE_MQTT_URL.
 *
 * The real server.js is spawned against a throwaway local mosquitto and a
 * scratch DB, with OZLODGE_MDNS=0 so it never advertises the lab's service
 * name on the LAN. It must never reach the lab broker: the URL is always set
 * here, so the built-in default (10.1.1.42) is not exercised by this suite.
 */
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const mysql = require('mysql2/promise');

const SERVER_DIR = path.join(__dirname, '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function dbPassword() {
  if (process.env.OZLODGE_DB_PASSWORD) return process.env.OZLODGE_DB_PASSWORD;
  const file = path.join(SERVER_DIR, '.env');
  if (fs.existsSync(file)) {
    for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
      const m = line.match(/^\s*OZLODGE_DB_PASSWORD\s*=\s*(.*?)\s*$/);
      if (m) return m[1].replace(/^(['"])(.*)\1$/, '$2');
    }
  }
  throw new Error('OZLODGE_DB_PASSWORD not set (env or ozlodge/server/.env)');
}

const freePort = () =>
  new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once('error', reject);
    s.listen(0, '127.0.0.1', () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });

const mosquittoBin = () =>
  [process.env.OZ_TEST_MOSQUITTO, '/usr/local/sbin/mosquitto', '/opt/homebrew/sbin/mosquitto', '/usr/sbin/mosquitto']
    .filter(Boolean)
    .find((p) => fs.existsSync(p));

async function waitForLog(file, re, timeoutMs, proc) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const text = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
    if (re.test(text)) return text;
    if (proc.exitCode !== null) throw new Error(`server exited ${proc.exitCode} before ${re}\n${text}`);
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${re} (loadavg ${os.loadavg().map((l) => l.toFixed(1)).join('/')})\n${text}`);
    await sleep(100);
  }
}

const password = dbPassword();
const dbName = `zzz_ozlodge_brokerenv_${process.pid}`;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ozlodge-brokerenv-'));
const procs = [];
let mqttUrl;
let serverLog;
let server;

test.before(async () => {
  const bin = mosquittoBin();
  if (!bin) throw new Error('mosquitto not found; set OZ_TEST_MOSQUITTO');
  const mqttPort = await freePort();
  const conf = path.join(tmp, 'mosquitto.conf');
  fs.writeFileSync(conf, `listener ${mqttPort} 127.0.0.1\nallow_anonymous true\n`);
  procs.push(spawn(bin, ['-c', conf], { stdio: 'ignore' }));
  mqttUrl = `mqtt://127.0.0.1:${mqttPort}`;

  serverLog = path.join(tmp, 'server.log');
  const out = fs.openSync(serverLog, 'a');
  server = spawn(process.execPath, ['server.js'], {
    cwd: SERVER_DIR,
    stdio: ['ignore', out, out],
    env: {
      PATH: process.env.PATH,
      HOME: process.env.HOME,
      OZLODGE_DB_PASSWORD: password,
      OZLODGE_DB_NAME: dbName,
      OZLODGE_MQTT_URL: mqttUrl,
      OZLODGE_HTTP_PORT: String(await freePort()),
      OZLODGE_SERVER_IP: '127.0.0.1',
      OZLODGE_MDNS: '0',
    },
  });
  procs.push(server);
});

test.after(async () => {
  for (const p of procs.reverse()) {
    if (p.exitCode === null && p.signalCode === null) {
      p.kill('SIGTERM');
      await Promise.race([new Promise((r) => p.once('exit', r)), sleep(3000)]);
      if (p.exitCode === null && p.signalCode === null) p.kill('SIGKILL');
    }
  }
  const admin = await mysql.createConnection({ host: 'localhost', user: 'root', password });
  await admin.query(`DROP DATABASE IF EXISTS \`${dbName}\``);
  await admin.end();
  fs.rmSync(tmp, { recursive: true, force: true });
});

test('the broker comes from OZLODGE_MQTT_URL', async () => {
  const log = await waitForLog(serverLog, /MQTT online — TalkPOS broker /, 60000, server);
  assert.match(log, new RegExp(`MQTT online — TalkPOS broker ${mqttUrl.replace(/[.]/g, '\\.')}\\b`));
  assert.doesNotMatch(log, /10\.1\.1\.(20|42)/, 'a test server must not name a lab broker');
});

test('OZLODGE_MDNS=0: nothing is advertised', async () => {
  const log = await waitForLog(serverLog, /HTTP gateway listening/, 60000, server);
  await sleep(500); // startMdns() runs right after listen() is called
  const after = fs.readFileSync(serverLog, 'utf8');
  assert.match(after, /mDNS advertising disabled \(OZLODGE_MDNS=0, test only\)/);
  assert.doesNotMatch(log + after, /mDNS advertising "/);
});
