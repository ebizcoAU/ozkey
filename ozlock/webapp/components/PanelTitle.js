import { C } from '../lib/theme';

export default function PanelTitle({ dot, children, right }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
      <span style={{ width: 7, height: 7, borderRadius: '50%', background: dot, boxShadow: `0 0 6px ${dot}` }} />
      <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: 1.2, textTransform: 'uppercase', color: C.text }}>
        {children}
      </span>
      {right && <span style={{ marginLeft: 'auto' }}>{right}</span>}
    </div>
  );
}
