import { C } from '../lib/theme';

/** Small stacked bar for the fleet-wide online/asleep/never-seen split —
 *  a rollup of the same per-lock classification `lib/theme`'s presence()
 *  already uses in the Registry inspector, not a new taxonomy. */
export default function PresenceStack({ online = 0, asleep = 0, never = 0 }) {
  const total = online + asleep + never || 1;
  const segs = [
    { n: online, color: C.green, label: 'online' },
    { n: asleep, color: C.amber, label: 'asleep' },
    { n: never, color: C.gray, label: 'never seen' },
  ];

  return (
    <div>
      <div style={{ display: 'flex', height: 8, borderRadius: 4, overflow: 'hidden', border: `1px solid ${C.panelEdge}` }}>
        {segs.map(
          (s) =>
            s.n > 0 && (
              <div
                key={s.label}
                style={{ width: `${(s.n / total) * 100}%`, background: s.color }}
                title={`${s.label}: ${s.n}`}
              />
            )
        )}
      </div>
      <div style={{ display: 'flex', gap: 10, marginTop: 6, fontSize: 10, color: C.dim, flexWrap: 'wrap' }}>
        {segs.map((s) => (
          <span key={s.label}>
            <span style={{ color: s.color }}>●</span> {s.label} {s.n}
          </span>
        ))}
      </div>
    </div>
  );
}
