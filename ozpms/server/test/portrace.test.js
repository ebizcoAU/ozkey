'use strict';
/*
 * OZPMS-PORT-RACE-1 (PM nexus-46, 2026-11-18): the shared testkit picks a free
 * port, closes the probe, then spawns the server. Under a concurrent gate
 * another process could take the port in between, and the whole ozpms suite
 * went red on EADDRINUSE (17 fails, 2026-11-18 dev run). The harness now
 * retries on a fresh port. This forces the race: take the server's port, then
 * restart, and the server must come up on another port and still answer.
 */
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const net = require('net');
const { Fixture } = require('./harness');

const fx = new Fixture();
let squatter = null;

before(async () => {
  await fx.start();
});
after(async () => {
  if (squatter) await new Promise((r) => squatter.close(r));
  await fx.stop();
});

test('a taken HTTP port is retried on a fresh one, not a red suite', async () => {
  await fx.stopServer();
  const taken = fx.httpPort;
  squatter = net.createServer();
  // Same wildcard bind as the server (express listens on ::), so it collides.
  await new Promise((resolve, reject) => squatter.once('error', reject).listen(taken, resolve));
  await fx.restartServer();
  assert.notEqual(fx.httpPort, taken, 'moved to another port');
  const r = await fx.http('GET', '/health');
  assert.equal(r.status, 200);
  assert.match(fx.log(), /EADDRINUSE/, 'the collision really happened');
});
