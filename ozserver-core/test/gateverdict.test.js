'use strict';
/*
 * GATE-VERDICT-1: the gate verdict counts results, not lines. The fixture is
 * the real 6d2af01 rounds.txt (2026-11-18): 9 quiet + 3 concurrent lines with
 * 2 results each + 3 ozlodge = 18 results on 15 result lines.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { verdict } = require('../testkit/gate/verdict');

const real = fs.readFileSync(path.join(__dirname, 'gate-rounds-6d2af01.fixture.txt'), 'utf8');

test('GATE-VERDICT-1: the real 6d2af01 gate is 18 results, green (a line count saw 15)', () => {
  const v = verdict(real, 18);
  assert.equal(v.results, 18);
  assert.deepEqual(v.reasons, []);
  assert.equal(v.ok, true);
});

test('GATE-VERDICT-1: a missing run, one failing test, a bad rc or a moved HEAD is red', () => {
  const dropOne = real.replace(/^ozlodge3 .*\n/m, '');
  assert.equal(verdict(dropOne, 18).ok, false, 'a missing run');
  const oneFail = real.replace('| ozpms # pass 33 # fail 0', '| ozpms # pass 32 # fail 1');
  assert.equal(verdict(oneFail, 18).ok, false, 'a fail hidden in the second half of a concurrent line');
  assert.equal(verdict(real.replace('ozlodge1 rc=0', 'ozlodge1 rc=1'), 18).ok, false, 'rc != 0');
  const moved = real.replace(/(\nHEAD )6d2af01/, '$1deadbee');
  assert.equal(verdict(moved, 18).ok, false, 'HEAD moved during the gate');
});
