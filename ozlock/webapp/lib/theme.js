/*
 * Shared palette + small pure helpers, extracted from pages/index.js so the
 * new Overview dashboard components and the existing Registry console read
 * from one source instead of two copies drifting apart.
 */

export const C = {
  bg: '#0C1220',
  panel: '#16202F',
  panelEdge: '#2B3B52',
  text: '#E2E8F0',
  dim: '#8CA3BD',
  teal: '#2DD4BF',
  green: '#22C55E',
  blue: '#38BDF8',
  red: '#EF4444',
  gray: '#475569',
  amber: '#F59E0B',
  violet: '#C084FC',
  termGreen: '#4ADE80',
};

export const panelStyle = {
  background: C.panel,
  border: `1px solid ${C.panelEdge}`,
  borderRadius: 8,
  padding: 12,
};

export const inputStyle = {
  width: '100%',
  boxSizing: 'border-box',
  background: C.bg,
  border: `1px solid ${C.panelEdge}`,
  borderRadius: 6,
  color: C.text,
  padding: '8px 10px',
  fontSize: 13,
  fontFamily: 'inherit',
  outline: 'none',
};

export function agoLabel(iso) {
  if (!iso) return 'never';
  const s = Math.max(0, Math.floor((Date.now() - new Date(iso).getTime()) / 1000));
  if (s < 90) return `${s}s ago`;
  if (s < 5400) return `${Math.round(s / 60)}m ago`;
  if (s < 129600) return `${Math.round(s / 3600)}h ago`;
  return `${Math.round(s / 86400)}d ago`;
}

export function shortId(id, head = 10) {
  if (!id) return '—';
  return id.length > head + 6 ? `${id.slice(0, head)}…${id.slice(-4)}` : id;
}

export const statusColor = (s) =>
  ({ enrolled: C.green, registered: C.amber, revoked: C.gray }[s] || C.dim);

export const actionColor = (a) =>
  ({ pair: C.blue, grant: C.violet, revoke: C.red, unlock: C.teal, settings: C.amber }[a] || C.dim);

export const levelColor = (l) =>
  ({ error: C.red, warn: C.amber, pair: C.blue, key: C.violet, sync: C.teal, lock: C.blue }[l] ||
  C.termGreen);

/** Pull a device id (ozl-/ozk-) and/or app id (app_) out of a free-text event. */
export function extractIds(message) {
  const m = String(message);
  const dev = m.match(/\b(oz[lk]-[0-9a-f]{6,})\b/i);
  const app = m.match(/\b(app_[0-9a-zA-Z]{6,})\b/);
  return { deviceId: dev ? dev[1] : null, appId: app ? app[1] : null };
}

export function presence(lock) {
  if (!lock || !lock.last_seen_at) return { col: C.gray, label: 'never seen' };
  const s = (Date.now() - new Date(lock.last_seen_at).getTime()) / 1000;
  if (s < Math.max(90, (lock.heartbeat_s || 60) * 2)) return { col: C.green, label: 'online' };
  return { col: C.amber, label: `asleep · ${agoLabel(lock.last_seen_at)}` };
}

/** Fleet-wide rollup of the same per-row `presence()` classification above —
 *  online/asleep/never-seen, for the Overview health strip and PresenceStack. */
export function presenceSummary(locks) {
  const out = { online: 0, asleep: 0, never: 0 };
  for (const l of locks || []) {
    if (!l || !l.last_seen_at) out.never += 1;
    else {
      const s = (Date.now() - new Date(l.last_seen_at).getTime()) / 1000;
      if (s < Math.max(90, (l.heartbeat_s || 60) * 2)) out.online += 1;
      else out.asleep += 1;
    }
  }
  return out;
}
