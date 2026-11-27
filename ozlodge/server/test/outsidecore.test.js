'use strict';
/*
 * ozkey-57 P2: ozlodge is outside the shared core (finding 2026-11-19: a
 * standalone server with its own tables), so the ozlock transit-only changes
 * in ozserver-core cannot change ozlodge. If ozlodge ever starts using the
 * core, this row goes red and ozlodge's retention must be ruled on first.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

test('P2: ozlodge requires nothing from ozserver-core', () => {
  const dir = path.join(__dirname, '..');
  const hits = fs
    .readdirSync(dir)
    .filter((f) => f.endsWith('.js'))
    .filter((f) => /require\(\s*['"`][^'"`]*ozserver-core/.test(fs.readFileSync(path.join(dir, f), 'utf8')));
  assert.deepEqual(hits, []);
});
