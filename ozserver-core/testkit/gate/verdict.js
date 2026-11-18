'use strict';
/*
 * testkit/gate/verdict.js — the verdict for a gated-rounds.sh rounds.txt.
 *
 * Counts RESULTS, not lines: a concurrent round prints two suite results on
 * one line ("conc1 ozlock # pass … # fail 0 … | ozpms # pass … # fail 0 …").
 * Counting lines (grep -c) saw 15 of 18 on 6d2af01 and refused a green gate.
 *
 * CLI: node verdict.js <rounds.txt> <expected results>
 *      exit 0 = exactly <expected> results, 0 fails, no rc≠0, HEAD unchanged.
 */
const fs = require('node:fs');

function verdict(text, expected) {
  const results = [...text.matchAll(/# pass (\d+) # fail (\d+)/g)].map((m) => ({ pass: +m[1], fail: +m[2] }));
  const fails = results.reduce((s, r) => s + r.fail, 0);
  const badRc = (text.match(/\brc=[1-9]\d*/g) || []).length;
  const heads = [...text.matchAll(/^HEAD (\S+) diff (\S+)/gm)].map((m) => `${m[1]} ${m[2]}`);
  const headStable = heads.length >= 2 && heads.every((h) => h === heads[0]);
  const reasons = [];
  if (results.length !== expected) reasons.push(`results ${results.length} != expected ${expected}`);
  if (fails) reasons.push(`${fails} failing test(s)`);
  if (badRc) reasons.push(`${badRc} run(s) with rc != 0`);
  if (!headStable) reasons.push('HEAD/diff not recorded identically at start and end');
  return { ok: reasons.length === 0, results: results.length, fails, badRc, headStable, reasons };
}

module.exports = { verdict };

if (require.main === module) {
  const [file, exp] = process.argv.slice(2);
  const v = verdict(fs.readFileSync(file, 'utf8'), Number(exp));
  console.log(`results=${v.results} fails=${v.fails} badrc=${v.badRc} head_stable=${v.headStable} ${v.ok ? 'GREEN' : 'RED: ' + v.reasons.join('; ')}`);
  process.exit(v.ok ? 0 : 1);
}
