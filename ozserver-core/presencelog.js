'use strict';
/**
 * OZLOCK-LOG-NOISE-1: presence log lines on CHANGE only, plus one summary line
 * per CONFIG.PRESENCE_SUMMARY_MS (default 1 h).
 *
 * The XF-152 resync redelivers every retained presence every 30 s; each one
 * used to log again (about 2,900 lines/day per bridge). Only the log line is
 * gated here: the DB writes and re-verdicts still run on every report.
 * State is in memory, so the first report after a restart is always logged.
 */
const { ctx } = require('./ctx');

const last = new Map(); // key -> last logged text
let quiet = { bridge: 0, lock: 0 };
let since = Date.now();
let timer = null;

/** True (and remembered) when `text` differs from the last one logged for
 *  `kind`/`id`; otherwise counts a repeat and returns false. */
function changed(kind, id, text) {
  const k = `${kind}|${id}`;
  if (last.get(k) === text) {
    quiet[kind] = (quiet[kind] || 0) + 1;
    return false;
  }
  last.set(k, text);
  return true;
}

function summary() {
  const n = quiet.bridge + quiet.lock;
  const secs = Math.round((Date.now() - since) / 1000);
  ctx.logEvent(
    'info',
    `Presence summary (last ${secs} s): ${n} repeat report(s) not logged (bridges ${quiet.bridge}, locks ${quiet.lock}); ` +
      `${last.size} bridge/lock state(s) tracked`
  );
  quiet = { bridge: 0, lock: 0 };
  since = Date.now();
}

/** Arms the summary timer (idempotent; unref'd so it never holds the process). */
function startPresenceSummary() {
  if (timer) return;
  const ms = Number(ctx.CONFIG.PRESENCE_SUMMARY_MS) || 3_600_000;
  since = Date.now();
  timer = setInterval(summary, ms);
  timer.unref?.();
}

function stopPresenceSummary() {
  if (timer) clearInterval(timer);
  timer = null;
}

module.exports = { changed, startPresenceSummary, stopPresenceSummary };
