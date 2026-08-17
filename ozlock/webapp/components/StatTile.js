import { C } from '../lib/theme';

/** One health-strip tile: dot + label, big value, optional sub line. */
export default function StatTile({ label, value, sub, dot, accent }) {
  return (
    <div
      style={{
        background: C.bg,
        border: `1px solid ${C.panelEdge}`,
        borderRadius: 7,
        padding: '10px 12px',
        minWidth: 120,
        flex: '1 1 140px',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 4 }}>
        {dot && (
          <span
            style={{ width: 7, height: 7, borderRadius: '50%', background: dot, boxShadow: `0 0 6px ${dot}` }}
          />
        )}
        <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1, textTransform: 'uppercase', color: C.dim }}>
          {label}
        </span>
      </div>
      <div style={{ fontSize: 20, fontWeight: 800, color: accent || C.text }}>{value}</div>
      {sub && <div style={{ fontSize: 10, color: C.dim, marginTop: 2 }}>{sub}</div>}
    </div>
  );
}
