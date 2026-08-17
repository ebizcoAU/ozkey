import { C, agoLabel, shortId } from '../lib/theme';

/* Real data only (no mock): ranks locks by what a fleet engineer actually
 * wants surfaced first, using fields /locks already returns —
 * battery_pct, mcu_link_up, mcu_last_frame_s, last_seen_at. Answers "who's
 * about to go dark" before someone has to search for a lost doorlock. */

const DAY_S = 86400;

function issuesFor(l) {
  const issues = [];
  const lastSeenS = l.last_seen_at ? (Date.now() - new Date(l.last_seen_at).getTime()) / 1000 : null;

  if (lastSeenS === null) {
    issues.push({ label: 'never seen', color: C.red, weight: 100 });
  } else if (lastSeenS > DAY_S) {
    issues.push({ label: `silent ${agoLabel(l.last_seen_at)}`, color: C.amber, weight: 60 + Math.min(30, lastSeenS / DAY_S) });
  }

  if (l.mcu_link_up === false || l.mcu_link_up === 0) {
    issues.push({ label: 'MCU link down', color: C.red, weight: 90 });
  }

  if (typeof l.battery_pct === 'number' && l.battery_pct < 20) {
    issues.push({ label: `battery ${l.battery_pct}%`, color: l.battery_pct < 10 ? C.red : C.amber, weight: 80 - l.battery_pct });
  }

  return issues;
}

export default function AttentionTable({ locks = [], onJumpToLock, limit = 8 }) {
  const ranked = locks
    .map((l) => ({ lock: l, issues: issuesFor(l) }))
    .filter((r) => r.issues.length > 0)
    .sort((a, b) => Math.max(...b.issues.map((i) => i.weight)) - Math.max(...a.issues.map((i) => i.weight)))
    .slice(0, limit);

  if (ranked.length === 0) {
    return <div style={{ color: C.dim, fontSize: 12 }}>— no doorlock currently needs attention —</div>;
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
      {ranked.map(({ lock: l, issues }) => (
        <div
          key={l.id}
          onClick={() => onJumpToLock(l.id)}
          style={{
            cursor: 'pointer',
            border: `1px solid ${C.panelEdge}`,
            borderRadius: 6,
            padding: '7px 10px',
            display: 'flex',
            alignItems: 'center',
            gap: 8,
            background: C.bg,
            flexWrap: 'wrap',
          }}
        >
          <span style={{ fontSize: 12, fontWeight: 700, minWidth: 90 }}>{l.label}</span>
          <span style={{ fontSize: 10, color: C.dim, wordBreak: 'break-all' }}>{shortId(l.id, 12)}</span>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            {issues.map((iss) => (
              <span
                key={iss.label}
                style={{
                  fontSize: 9,
                  fontWeight: 700,
                  color: iss.color,
                  border: `1px solid ${iss.color}`,
                  borderRadius: 4,
                  padding: '2px 6px',
                  whiteSpace: 'nowrap',
                }}
              >
                {iss.label}
              </span>
            ))}
          </div>
        </div>
      ))}
    </div>
  );
}
