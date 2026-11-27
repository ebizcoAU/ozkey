'use strict';
/*
 * ozkey-57 R3: every audit row has a KIND, 'security' or 'use'. A profile
 * that sets CONFIG.AUDIT_KEEP_KINDS keeps only those kinds (ozlock: security
 * only; an untagged call counts as 'use' there). Without the key, every row
 * is kept, as before (ozpms). No DB: the pool is stubbed.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { ctx } = require('../ctx');

const inserts = [];
Object.assign(ctx, {
  CONFIG: { SITE_ID: 'lab' },
  logEvent: () => {},
  pool: { query: async (sql, args) => { if (/INSERT INTO audit_log/.test(sql)) inserts.push(args); return [[]]; } },
});
const { recordAudit } = require('../audit');

async function rows(fn) {
  inserts.length = 0;
  await fn();
  return inserts.map((a) => a[3]);
}

test('R3: no AUDIT_KEEP_KINDS (ozpms): every kind is kept, untagged too', async () => {
  delete ctx.CONFIG.AUDIT_KEEP_KINDS;
  const got = await rows(async () => {
    await recordAudit('app', 'L', 'unlock', 'remote unlock: sent_unconfirmed', 'use');
    await recordAudit('app', 'L', 'pair', 'x', 'security');
    await recordAudit('app', 'L', 'legacy', 'x');
  });
  assert.deepEqual(got, ['unlock', 'pair', 'legacy']);
});

test('R3: AUDIT_KEEP_KINDS = [security] (ozlock): use and untagged rows are not written', async () => {
  ctx.CONFIG.AUDIT_KEEP_KINDS = ['security'];
  const got = await rows(async () => {
    await recordAudit('app', 'L', 'unlock', 'remote unlock: sent_unconfirmed', 'use');
    await recordAudit('app', 'L', 'unlock', 'remote unlock: refused COUNTER_REPLAY', 'security');
    await recordAudit('app', 'L', 'legacy', 'x');
  });
  assert.deepEqual(got, ['unlock']);
});

test('R3 static: every recordAudit( call in the core names its kind', () => {
  const dir = path.join(__dirname, '..');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.js'));
  const untagged = [];
  for (const f of files) {
    const src = fs.readFileSync(path.join(dir, f), 'utf8');
    // Every call (not the definition, not the late-bound forwarder).
    const re = /(?<!function |\.)recordAudit\(/g;
    let m;
    while ((m = re.exec(src))) {
      if (src.slice(m.index - 30, m.index).includes('=> require(')) continue;
      if (src.slice(m.index, m.index + 20).startsWith('recordAudit(...args')) continue;
      if (src.slice(m.index, m.index + 13) === 'recordAudit()') continue; // prose: "recordAudit()"
      const lineStart = src.lastIndexOf('\n', m.index) + 1;
      const lead = src.slice(lineStart, m.index).trim();
      if (lead.startsWith('*') || lead.startsWith('//')) continue; // a comment
      // Find the matching close paren, skipping string and template contents.
      let depth = 0, i = m.index + 'recordAudit'.length, end = -1, q = null;
      for (; i < src.length; i++) {
        const c = src[i];
        if (q) { if (c === '\\') i++; else if (c === q) q = null; continue; }
        if (c === "'" || c === '"' || c === '`') q = c;
        else if (c === '(') depth++;
        else if (c === ')') { depth--; if (depth === 0) { end = i; break; } }
      }
      const call = src.slice(m.index, end + 1);
      if (!/,\s*'(security|use)'\s*\)$/.test(call)) {
        untagged.push(`${f}:${src.slice(0, m.index).split('\n').length}`);
      }
    }
  }
  assert.deepEqual(untagged, [], 'calls without a kind');
});
