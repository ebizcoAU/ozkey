import { C } from '../lib/theme';

/** Radial arc meter. Caller computes `pct` (0-1) and `centerText` — this
 *  component only draws, so it works equally for a percentage (fleet
 *  online %) or a raw count against a soft max (queue backlog). */
export default function Gauge({ pct, centerText, label, sublabel, color = C.teal, size = 96 }) {
  const clamped = Math.max(0, Math.min(1, pct || 0));
  const r = (size - 12) / 2;
  const c = 2 * Math.PI * r;
  const offset = c * (1 - clamped);
  const cx = size / 2;
  const cy = size / 2;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
        <circle cx={cx} cy={cy} r={r} fill="none" stroke={C.panelEdge} strokeWidth={8} />
        <circle
          cx={cx}
          cy={cy}
          r={r}
          fill="none"
          stroke={color}
          strokeWidth={8}
          strokeDasharray={c}
          strokeDashoffset={offset}
          strokeLinecap="round"
          transform={`rotate(-90 ${cx} ${cy})`}
          style={{ transition: 'stroke-dashoffset 0.6s ease' }}
        />
        <text x="50%" y="47%" textAnchor="middle" fontSize={size * 0.2} fontWeight={800} fill={C.text} fontFamily="inherit">
          {centerText}
        </text>
        {sublabel && (
          <text x="50%" y="64%" textAnchor="middle" fontSize={size * 0.09} fill={C.dim} fontFamily="inherit">
            {sublabel}
          </text>
        )}
      </svg>
      <div style={{ fontSize: 10, fontWeight: 700, letterSpacing: 1, textTransform: 'uppercase', color: C.dim }}>
        {label}
      </div>
    </div>
  );
}
