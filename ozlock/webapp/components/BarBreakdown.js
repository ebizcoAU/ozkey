import { C } from '../lib/theme';

/** Horizontal bar list — one row per {label, value, color}, count + share. */
export default function BarBreakdown({ rows = [] }) {
  const total = rows.reduce((s, r) => s + r.value, 0) || 1;
  const max = Math.max(...rows.map((r) => r.value), 1);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
      {rows.map((r) => (
        <div key={r.label}>
          <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 10, color: C.dim, marginBottom: 3 }}>
            <span style={{ textTransform: 'uppercase', letterSpacing: 0.5 }}>{r.label}</span>
            <span>
              {r.value.toLocaleString()} · {((r.value / total) * 100).toFixed(0)}%
            </span>
          </div>
          <div style={{ background: '#050B14', border: `1px solid ${C.panelEdge}`, borderRadius: 4, height: 8, overflow: 'hidden' }}>
            <div
              style={{
                width: `${(r.value / max) * 100}%`,
                height: '100%',
                background: r.color,
                boxShadow: `0 0 6px ${r.color}`,
              }}
            />
          </div>
        </div>
      ))}
    </div>
  );
}
