/*
 * ============================================================================
 *  OZLOCK REGISTRY CONSOLE — the rendezvous server's operator view (Port 4300)
 *  ---------------------------------------------------------------------------
 *  OZLOCK is the STUN/rendezvous server: it matches apps to doorlocks and
 *  relays. It does NOT grant keys or add doorlocks — those are the BANOI app's
 *  job. This console is READ-ONLY observability over what OZLOCK knows:
 *    • given an app (user id)  → all its doorlocks + its activity log
 *    • given a doorlock (device id) → the bound app + its door-transaction log
 *  Two record streams, kept distinct:
 *    • Activity log  = control-plane actions an app performed (pair/grant/
 *                      revoke/unlock) — from audit_log
 *    • Door log      = physical access events at the lock (granted/denied/
 *                      expired) — from lock_logs
 *  Against OZLOCKSERV (:4200). Registry admin (prune stale records) is kept;
 *  originating pairings/keys is not.
 * ============================================================================
 */

import { useCallback, useEffect, useRef, useState } from 'react';

const API = 'http://localhost:4200/ozlockserv/api';

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */
const C = {
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

const panelStyle = {
  background: C.panel,
  border: `1px solid ${C.panelEdge}`,
  borderRadius: 8,
  padding: 12,
};

const inputStyle = {
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

function PanelTitle({ dot, children, right }) {
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

function agoLabel(iso) {
  if (!iso) return 'never';
  const s = Math.max(0, Math.floor((Date.now() - new Date(iso).getTime()) / 1000));
  if (s < 90) return `${s}s ago`;
  if (s < 5400) return `${Math.round(s / 60)}m ago`;
  if (s < 129600) return `${Math.round(s / 3600)}h ago`;
  return `${Math.round(s / 86400)}d ago`;
}

function shortId(id, head = 10) {
  if (!id) return '—';
  return id.length > head + 6 ? `${id.slice(0, head)}…${id.slice(-4)}` : id;
}

const statusColor = (s) =>
  ({ enrolled: C.green, registered: C.amber, revoked: C.gray }[s] || C.dim);

const actionColor = (a) =>
  ({ pair: C.blue, grant: C.violet, revoke: C.red, unlock: C.teal, settings: C.amber }[a] || C.dim);

const resultColor = (r) =>
  ({ granted: C.green, denied: C.red, expired: C.amber }[r] || C.dim);

const levelColor = (l) =>
  ({ error: C.red, warn: C.amber, pair: C.blue, key: C.violet, sync: C.teal, lock: C.blue }[l] ||
  C.termGreen);

/** Pull a device id (ozl-/ozk-) and/or app id (app_) out of a free-text event. */
function extractIds(message) {
  const m = String(message);
  const dev = m.match(/\b(oz[lk]-[0-9a-f]{6,})\b/i);
  const app = m.match(/\b(app_[0-9a-zA-Z]{6,})\b/);
  return { deviceId: dev ? dev[1] : null, appId: app ? app[1] : null };
}

function presence(lock) {
  if (!lock || !lock.last_seen_at) return { col: C.gray, label: 'never seen' };
  const s = (Date.now() - new Date(lock.last_seen_at).getTime()) / 1000;
  if (s < Math.max(90, (lock.heartbeat_s || 60) * 2)) return { col: C.green, label: 'online' };
  return { col: C.amber, label: `asleep · ${agoLabel(lock.last_seen_at)}` };
}

/* ===========================================================================
 * Registry console
 * ========================================================================= */
export default function OzlockConsole() {
  const [gatewayUp, setGatewayUp] = useState(false);
  const [mqttUp, setMqttUp] = useState(false);

  const [apps, setApps] = useState([]);
  const [locks, setLocks] = useState([]);
  const [browseTab, setBrowseTab] = useState('apps'); // 'apps' | 'locks'
  const [search, setSearch] = useState('');

  // selection: an app OR a lock (mutually exclusive inspector)
  const [sel, setSel] = useState(null); // {kind:'app'|'lock', id}
  const selRef = useRef(null);
  useEffect(() => {
    selRef.current = sel;
  }, [sel]);

  const [appLocks, setAppLocks] = useState([]);
  const [lockDetail, setLockDetail] = useState(null);
  const [inspectTab, setInspectTab] = useState('primary'); // primary|secondary

  // Paginated + date-ranged logs (16 rows/page, fetched on demand — not polled,
  // so page boundaries don't jitter while you browse history).
  const PAGE = 16;
  const [appActivity, setAppActivity] = useState([]);
  const [actTotal, setActTotal] = useState(0);
  const [actPage, setActPage] = useState(0);
  const [actFrom, setActFrom] = useState('');
  const [actTo, setActTo] = useState('');
  const [lockLog, setLockLog] = useState([]);
  const [logTotal, setLogTotal] = useState(0);
  const [logPage, setLogPage] = useState(0);
  const [logFrom, setLogFrom] = useState('');
  const [logTo, setLogTo] = useState('');

  // General activity firehose — every message OZLOCK receives, live.
  const [events, setEvents] = useState([]);
  const lastEventIdRef = useRef(0);
  const feedRef = useRef(null);

  /* -------------------------------------------------------------------------
   * Polling: registry (apps + locks) + the selected entity's detail
   * ----------------------------------------------------------------------- */
  // Live parts only (safe to poll): an app's lock list, a lock's detail row.
  const loadSelection = useCallback(async (s) => {
    if (!s) return;
    try {
      if (s.kind === 'app') {
        const l = await fetch(`${API}/apps/${encodeURIComponent(s.id)}/locks`).then((r) => r.json());
        if (l.ok) setAppLocks(l.locks);
      } else {
        const d = await fetch(`${API}/locks/${encodeURIComponent(s.id)}`).then((r) => r.json());
        if (d.ok) setLockDetail(d.lock);
      }
    } catch (_) {
      /* transient */
    }
  }, []);

  const rangeQs = (page, from, to) => {
    const qs = new URLSearchParams({ limit: String(PAGE), offset: String(page * PAGE) });
    if (from) qs.set('from', from);
    if (to) qs.set('to', to);
    return qs.toString();
  };

  // App activity — refetch when the app, its page, or its date range changes.
  useEffect(() => {
    if (!sel || sel.kind !== 'app') return;
    fetch(`${API}/apps/${encodeURIComponent(sel.id)}/activity?${rangeQs(actPage, actFrom, actTo)}`)
      .then((r) => r.json())
      .then((d) => {
        if (d.ok) {
          setAppActivity(d.activity);
          setActTotal(d.total);
        }
      })
      .catch(() => {});
  }, [sel, actPage, actFrom, actTo]);

  // Door transactions — refetch when the lock, its page, or its date range changes.
  useEffect(() => {
    if (!sel || sel.kind !== 'lock') return;
    fetch(`${API}/locks/${encodeURIComponent(sel.id)}/log?${rangeQs(logPage, logFrom, logTo)}`)
      .then((r) => r.json())
      .then((d) => {
        if (d.ok) {
          setLockLog(d.log);
          setLogTotal(d.total);
        }
      })
      .catch(() => {});
  }, [sel, logPage, logFrom, logTo]);

  useEffect(() => {
    let alive = true;
    async function poll() {
      try {
        const [h, appsRes, locksRes, evRes] = await Promise.all([
          fetch(`${API}/health`).then((r) => r.json()),
          fetch(`${API}/apps`).then((r) => r.json()),
          fetch(`${API}/locks`).then((r) => r.json()),
          fetch(`${API}/events?after=${lastEventIdRef.current}`).then((r) => r.json()),
        ]);
        if (!alive) return;
        setGatewayUp(true);
        setMqttUp(!!h.mqtt);
        if (appsRes.ok) setApps(appsRes.apps);
        if (locksRes.ok) setLocks(locksRes.locks);
        if (evRes.ok && evRes.events.length) {
          lastEventIdRef.current = evRes.events[evRes.events.length - 1].id;
          setEvents((prev) => {
            const merged = [...prev, ...evRes.events];
            return merged.length > 500 ? merged.slice(merged.length - 500) : merged;
          });
        }
        if (selRef.current) await loadSelection(selRef.current);
      } catch (_) {
        if (!alive) return;
        setGatewayUp(false);
        setMqttUp(false);
      }
    }
    poll();
    const t = setInterval(poll, 2500);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, [loadSelection]);

  useEffect(() => {
    if (feedRef.current) feedRef.current.scrollTop = feedRef.current.scrollHeight;
  }, [events]);

  const selectApp = (id) => {
    setSel({ kind: 'app', id });
    setInspectTab('primary');
    setAppLocks([]);
    setAppActivity([]);
    setActTotal(0);
    setActPage(0);
    setActFrom('');
    setActTo('');
    loadSelection({ kind: 'app', id });
  };
  const selectLock = (id) => {
    setSel({ kind: 'lock', id });
    setInspectTab('primary');
    setLockDetail(null);
    setLockLog([]);
    setLogTotal(0);
    setLogPage(0);
    setLogFrom('');
    setLogTo('');
    loadSelection({ kind: 'lock', id });
  };

  /* -------------------------------------------------------------------------
   * Registry admin (prune stale records — NOT an app action)
   * ----------------------------------------------------------------------- */
  const removeLock = async (id, e) => {
    if (e) e.stopPropagation();
    if (!confirm(`Remove pairing record ${id}?\nThis prunes the lock and its keys/queue/logs from OZLOCK's registry.`))
      return;
    try {
      await fetch(`${API}/locks/${encodeURIComponent(id)}`, { method: 'DELETE' });
      if (sel && sel.kind === 'lock' && sel.id === id) setSel(null);
    } catch (_) {}
  };

  /* -------------------------------------------------------------------------
   * Derived / filtered
   * ----------------------------------------------------------------------- */
  const q = search.trim().toLowerCase();
  const appsF = q ? apps.filter((a) => String(a.app_id).toLowerCase().includes(q)) : apps;
  const locksF = q
    ? locks.filter(
        (l) =>
          String(l.id).toLowerCase().includes(q) ||
          String(l.app_id).toLowerCase().includes(q) ||
          String(l.label).toLowerCase().includes(q)
      )
    : locks;

  const fmtTime = (iso) =>
    iso ? new Date(iso).toLocaleString('en-AU', { hour12: false }) : '—';

  const dateInputStyle = { ...inputStyle, width: 'auto', padding: '4px 8px', fontSize: 11, colorScheme: 'dark' };
  const pagerBtn = (on, accent) => ({
    cursor: on ? 'pointer' : 'not-allowed',
    fontFamily: 'inherit',
    fontSize: 10,
    fontWeight: 700,
    padding: '3px 10px',
    borderRadius: 5,
    border: `1px solid ${on ? accent : C.panelEdge}`,
    background: 'transparent',
    color: on ? accent : C.gray,
    opacity: on ? 1 : 0.5,
  });

  const renderDateRange = (from, to, setFrom, setTo, setPage) => (
    <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginBottom: 8, flexWrap: 'wrap' }}>
      <span style={{ fontSize: 9, color: C.dim, textTransform: 'uppercase', letterSpacing: 1 }}>From</span>
      <input type="date" value={from} onChange={(e) => { setFrom(e.target.value); setPage(0); }} style={dateInputStyle} />
      <span style={{ fontSize: 9, color: C.dim, textTransform: 'uppercase', letterSpacing: 1 }}>To</span>
      <input type="date" value={to} onChange={(e) => { setTo(e.target.value); setPage(0); }} style={dateInputStyle} />
      {(from || to) && (
        <button
          onClick={() => { setFrom(''); setTo(''); setPage(0); }}
          style={{ cursor: 'pointer', fontFamily: 'inherit', fontSize: 10, color: C.dim, background: 'transparent', border: `1px solid ${C.panelEdge}`, borderRadius: 5, padding: '3px 8px' }}
        >
          clear
        </button>
      )}
    </div>
  );

  const renderPager = (page, total, count, setPage, accent, onExport) => {
    const start = total === 0 ? 0 : page * PAGE + 1;
    const end = page * PAGE + count;
    const hasPrev = page > 0;
    const hasNext = (page + 1) * PAGE < total;
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginTop: 8, fontSize: 10, color: C.dim }}>
        <span>{total === 0 ? 'no records in range' : `showing ${start}–${end} of ${total}`}</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          {onExport && (
            <button disabled={total === 0} onClick={onExport} style={pagerBtn(total > 0, accent)} title="Download every record in the current date range as CSV">
              ⤓ CSV
            </button>
          )}
          <button disabled={!hasPrev} onClick={() => setPage((p) => p - 1)} style={pagerBtn(hasPrev, accent)}>‹ prev</button>
          <button disabled={!hasNext} onClick={() => setPage((p) => p + 1)} style={pagerBtn(hasNext, accent)}>next ›</button>
        </div>
      </div>
    );
  };

  /* -------------------------------------------------------------------------
   * CSV export — whole date range, not just the visible page. The API clamps
   * limit at 200/request, so page through until `total` rows are collected.
   * ----------------------------------------------------------------------- */
  const csvCell = (v) => {
    const s = v == null ? '' : String(v);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };

  const downloadCsv = (filename, header, rows) => {
    const csv = [header, ...rows].map((r) => r.map(csvCell).join(',')).join('\n');
    const url = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  };

  const fetchAllInRange = async (base, from, to, key) => {
    const rows = [];
    for (let offset = 0; ; offset += 200) {
      const qs = new URLSearchParams({ limit: '200', offset: String(offset) });
      if (from) qs.set('from', from);
      if (to) qs.set('to', to);
      const d = await fetch(`${base}?${qs}`).then((r) => r.json());
      if (!d.ok || !d[key].length) break;
      rows.push(...d[key]);
      if (rows.length >= d.total) break;
    }
    return rows;
  };

  const stamp = () => new Date().toISOString().slice(0, 10);

  const exportActivityCsv = async () => {
    const rows = await fetchAllInRange(
      `${API}/apps/${encodeURIComponent(sel.id)}/activity`, actFrom, actTo, 'activity'
    );
    downloadCsv(
      `ozlock-activity-${sel.id.slice(0, 12)}-${stamp()}.csv`,
      ['id', 'time_utc', 'action', 'detail', 'device_id'],
      rows.map((a) => [a.id, a.created_at, a.action, a.detail, a.device_id || ''])
    );
  };

  const exportDoorLogCsv = async () => {
    const rows = await fetchAllInRange(
      `${API}/locks/${encodeURIComponent(sel.id)}/log`, logFrom, logTo, 'log'
    );
    downloadCsv(
      `ozlock-doorlog-${sel.id}-${stamp()}.csv`,
      ['id', 'lock_time_utc', 'received_utc', 'result', 'detail'],
      rows.map((t) => [t.id, t.lock_ts || '', t.created_at, t.result, t.detail || ''])
    );
  };

  /* -------------------------------------------------------------------------
   * Render
   * ----------------------------------------------------------------------- */
  return (
    <div
      style={{
        minHeight: '100vh',
        background: C.bg,
        color: C.text,
        fontFamily: "'SF Mono', 'JetBrains Mono', Menlo, Consolas, 'Liberation Mono', monospace",
        padding: 12,
      }}
    >
      {/* Header */}
      <div
        style={{
          ...panelStyle,
          padding: '8px 14px',
          display: 'flex',
          alignItems: 'center',
          gap: 14,
          marginBottom: 10,
          flexWrap: 'wrap',
        }}
      >
        <div style={{ fontSize: 14, fontWeight: 800, letterSpacing: 1.5, whiteSpace: 'nowrap' }}>
          OZLOCK <span style={{ color: C.teal }}>//</span> REGISTRY CONSOLE
        </div>
        <span style={{ fontSize: 10, color: C.dim }}>
          rendezvous directory · read-only · does not grant keys or add locks
        </span>
        <input
          style={{ ...inputStyle, flex: '1 1 220px', maxWidth: 340, marginLeft: 'auto', padding: '6px 10px' }}
          placeholder="search app id / device id / label…"
          value={search}
          onChange={(e) => setSearch(e.target.value)}
        />
        <div style={{ display: 'flex', gap: 12, fontSize: 10 }}>
          {[
            ['OZLOCK', gatewayUp],
            ['MQTT', mqttUp],
          ].map(([label, up]) => (
            <div key={label} style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
              <span
                style={{
                  width: 7,
                  height: 7,
                  borderRadius: '50%',
                  background: up ? C.green : C.red,
                  boxShadow: `0 0 6px ${up ? C.green : C.red}`,
                }}
              />
              <span style={{ color: up ? C.text : C.dim }}>{label}</span>
            </div>
          ))}
        </div>
      </div>

      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', alignItems: 'stretch' }}>
        {/* ---- Left: registry browser ---------------------------------- */}
        <div style={{ ...panelStyle, flex: '1 1 340px', minWidth: 300 }}>
          <div style={{ display: 'flex', gap: 6, marginBottom: 10 }}>
            {[
              ['apps', `Apps (${apps.length})`, C.violet],
              ['locks', `Doorlocks (${locks.length})`, C.teal],
            ].map(([id, label, col]) => (
              <button
                key={id}
                onClick={() => setBrowseTab(id)}
                style={{
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                  fontSize: 10,
                  fontWeight: 700,
                  letterSpacing: 1,
                  textTransform: 'uppercase',
                  padding: '6px 12px',
                  borderRadius: 5,
                  border: `1px solid ${browseTab === id ? col : C.panelEdge}`,
                  background: browseTab === id ? C.bg : 'transparent',
                  color: browseTab === id ? col : C.dim,
                }}
              >
                {label}
              </button>
            ))}
          </div>

          {browseTab === 'apps' && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
              {appsF.length === 0 && <div style={{ color: C.dim, fontSize: 12 }}>— no apps —</div>}
              {appsF.map((a) => {
                const on = sel && sel.kind === 'app' && sel.id === a.app_id;
                return (
                  <div
                    key={a.app_id}
                    onClick={() => selectApp(a.app_id)}
                    style={{
                      cursor: 'pointer',
                      border: `1px solid ${on ? C.violet : C.panelEdge}`,
                      background: on ? '#241833' : C.bg,
                      borderRadius: 7,
                      padding: '9px 11px',
                    }}
                  >
                    <div style={{ fontSize: 12, fontWeight: 700, color: C.violet, wordBreak: 'break-all' }}>
                      {shortId(a.app_id, 22)}
                    </div>
                    <div style={{ fontSize: 10, color: C.dim, marginTop: 2 }}>
                      {a.lock_count} lock{a.lock_count === 1 ? '' : 's'} · {a.enrolled_count || 0} enrolled ·
                      seen {agoLabel(a.last_seen_at)}
                    </div>
                  </div>
                );
              })}
            </div>
          )}

          {browseTab === 'locks' && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
              {locksF.length === 0 && <div style={{ color: C.dim, fontSize: 12 }}>— no doorlocks —</div>}
              {locksF.map((l) => {
                const on = sel && sel.kind === 'lock' && sel.id === l.id;
                const p = presence(l);
                return (
                  <div
                    key={l.id}
                    onClick={() => selectLock(l.id)}
                    style={{
                      cursor: 'pointer',
                      border: `1px solid ${on ? C.teal : C.panelEdge}`,
                      background: on ? '#0F2A26' : C.bg,
                      borderRadius: 7,
                      padding: '9px 11px',
                      display: 'flex',
                      alignItems: 'center',
                      gap: 10,
                    }}
                  >
                    <span
                      style={{ width: 8, height: 8, borderRadius: '50%', background: p.col, boxShadow: `0 0 6px ${p.col}`, flexShrink: 0 }}
                    />
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <div style={{ fontSize: 12, fontWeight: 700 }}>{l.label}</div>
                      <div style={{ fontSize: 10, color: C.dim, wordBreak: 'break-all' }}>
                        {shortId(l.id, 16)}{' '}
                        <span style={{ color: C.violet }}>→ {shortId(l.app_id, 12)}</span>
                      </div>
                    </div>
                    <span style={{ fontSize: 10, fontWeight: 700, color: statusColor(l.status), whiteSpace: 'nowrap' }}>
                      {String(l.status || '').toUpperCase()}
                    </span>
                    <button
                      onClick={(e) => removeLock(l.id, e)}
                      title="Prune this pairing record"
                      style={{
                        cursor: 'pointer',
                        fontFamily: 'inherit',
                        fontSize: 11,
                        color: C.dim,
                        background: 'transparent',
                        border: 'none',
                        padding: '0 2px',
                      }}
                    >
                      ✕
                    </button>
                  </div>
                );
              })}
            </div>
          )}
        </div>

        {/* ---- Right: inspector ---------------------------------------- */}
        <div style={{ ...panelStyle, flex: '2 1 480px', minWidth: 340, display: 'flex', flexDirection: 'column' }}>
          {!sel && (
            <div style={{ color: C.dim, fontSize: 12, padding: 24, textAlign: 'center' }}>
              Select an <span style={{ color: C.violet }}>app</span> to see its doorlocks + activity,
              <br />or a <span style={{ color: C.teal }}>doorlock</span> to see its bound app + door
              transactions.
            </div>
          )}

          {/* ----- App inspector ----- */}
          {sel && sel.kind === 'app' && (
            <>
              <PanelTitle dot={C.violet}>App · User</PanelTitle>
              <div style={{ fontSize: 12, color: C.violet, fontWeight: 700, wordBreak: 'break-all', marginBottom: 10 }}>
                {sel.id}
              </div>
              <div style={{ display: 'flex', gap: 6, marginBottom: 10 }}>
                {[
                  ['primary', `Doorlocks (${appLocks.length})`],
                  ['secondary', `Activity (${actTotal})`],
                ].map(([id, label]) => (
                  <button
                    key={id}
                    onClick={() => setInspectTab(id)}
                    style={{
                      cursor: 'pointer',
                      fontFamily: 'inherit',
                      fontSize: 10,
                      fontWeight: 700,
                      letterSpacing: 1,
                      textTransform: 'uppercase',
                      padding: '6px 12px',
                      borderRadius: 5,
                      border: `1px solid ${inspectTab === id ? C.violet : C.panelEdge}`,
                      background: inspectTab === id ? C.bg : 'transparent',
                      color: inspectTab === id ? C.violet : C.dim,
                    }}
                  >
                    {label}
                  </button>
                ))}
              </div>

              {inspectTab === 'primary' && (
                <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                  {appLocks.length === 0 && <div style={{ color: C.dim, fontSize: 12 }}>— no doorlocks —</div>}
                  {appLocks.map((l) => {
                    const p = presence(l);
                    return (
                      <div
                        key={l.id}
                        onClick={() => selectLock(l.id)}
                        style={{ cursor: 'pointer', border: `1px solid ${C.panelEdge}`, borderRadius: 7, padding: '8px 11px', background: C.bg, display: 'flex', alignItems: 'center', gap: 10 }}
                      >
                        <span style={{ width: 8, height: 8, borderRadius: '50%', background: p.col }} />
                        <div style={{ flex: 1, minWidth: 0 }}>
                          <div style={{ fontSize: 12, fontWeight: 700 }}>{l.label}</div>
                          <div style={{ fontSize: 10, color: C.dim, wordBreak: 'break-all' }}>{l.id}</div>
                        </div>
                        <span style={{ fontSize: 10, color: statusColor(l.status), fontWeight: 700 }}>
                          {String(l.status || '').toUpperCase()}
                        </span>
                      </div>
                    );
                  })}
                </div>
              )}

              {inspectTab === 'secondary' && (
                <>
                  {renderDateRange(actFrom, actTo, setActFrom, setActTo, setActPage)}
                  <div style={{ background: '#050B14', border: `1px solid ${C.panelEdge}`, borderRadius: 6, padding: '10px 12px', fontSize: 12, lineHeight: 1.7, height: 464, overflowY: 'auto' }}>
                    {appActivity.length === 0 && <div style={{ color: C.dim }}>— no activity in range —</div>}
                    {appActivity.map((a) => (
                      <div key={a.id} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                        <span style={{ color: '#33445C' }}>{fmtTime(a.created_at)} </span>
                        <span style={{ color: actionColor(a.action), fontWeight: 700 }}>
                          [{String(a.action).toUpperCase().padEnd(6, ' ')}]
                        </span>{' '}
                        <span style={{ color: C.termGreen }}>{a.detail}</span>
                        {a.device_id && <span style={{ color: C.dim }}> · {shortId(a.device_id, 14)}</span>}
                      </div>
                    ))}
                  </div>
                  {renderPager(actPage, actTotal, appActivity.length, setActPage, C.violet, exportActivityCsv)}
                </>
              )}
            </>
          )}

          {/* ----- Doorlock inspector ----- */}
          {sel && sel.kind === 'lock' && (
            <>
              <PanelTitle dot={C.teal}>Doorlock · Device</PanelTitle>
              <div style={{ fontSize: 12, marginBottom: 10, wordBreak: 'break-all' }}>
                <span style={{ color: C.teal, fontWeight: 700 }}>{sel.id}</span>
                <span style={{ color: C.dim }}> → bound app </span>
                {lockDetail && lockDetail.app_id ? (
                  <span
                    style={{ color: C.violet, fontWeight: 700, cursor: 'pointer' }}
                    onClick={() => selectApp(lockDetail.app_id)}
                    title="Inspect this app"
                  >
                    {shortId(lockDetail.app_id, 18)}
                  </span>
                ) : (
                  <span style={{ color: C.dim }}>—</span>
                )}
              </div>
              <div style={{ display: 'flex', gap: 6, marginBottom: 10 }}>
                {[
                  ['primary', `Door Transactions (${logTotal})`],
                  ['secondary', 'Details'],
                ].map(([id, label]) => (
                  <button
                    key={id}
                    onClick={() => setInspectTab(id)}
                    style={{
                      cursor: 'pointer',
                      fontFamily: 'inherit',
                      fontSize: 10,
                      fontWeight: 700,
                      letterSpacing: 1,
                      textTransform: 'uppercase',
                      padding: '6px 12px',
                      borderRadius: 5,
                      border: `1px solid ${inspectTab === id ? C.teal : C.panelEdge}`,
                      background: inspectTab === id ? C.bg : 'transparent',
                      color: inspectTab === id ? C.teal : C.dim,
                    }}
                  >
                    {label}
                  </button>
                ))}
              </div>

              {inspectTab === 'primary' && (
                <>
                  <div style={{ color: C.dim, fontSize: 10, marginBottom: 6 }}>
                    physical access events at the lock (granted / denied / expired), newest first
                  </div>
                  {renderDateRange(logFrom, logTo, setLogFrom, setLogTo, setLogPage)}
                  <div style={{ background: '#050B14', border: `1px solid ${C.panelEdge}`, borderRadius: 6, padding: '10px 12px', fontSize: 12, lineHeight: 1.7, height: 464, overflowY: 'auto' }}>
                    {lockLog.length === 0 && <div style={{ color: C.dim }}>— no door transactions in range —</div>}
                    {lockLog.map((t) => (
                      <div key={t.id} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                        <span style={{ color: '#33445C' }}>{fmtTime(t.lock_ts || t.created_at)} </span>
                        <span style={{ color: resultColor(t.result), fontWeight: 700 }}>
                          [{String(t.result).toUpperCase().padEnd(7, ' ')}]
                        </span>{' '}
                        <span style={{ color: C.termGreen }}>{t.detail || '—'}</span>
                      </div>
                    ))}
                  </div>
                  {renderPager(logPage, logTotal, lockLog.length, setLogPage, C.teal, exportDoorLogCsv)}
                </>
              )}

              {inspectTab === 'secondary' && lockDetail && (
                <div style={{ fontSize: 12, lineHeight: 2 }}>
                  {[
                    ['Device ID', lockDetail.id],
                    ['Bound app', lockDetail.app_id || '—'],
                    ['MAC', lockDetail.mac || '—'],
                    ['Label', lockDetail.label],
                    ['Status', String(lockDetail.status || '').toUpperCase()],
                    ['Presence', presence(lockDetail).label],
                    ['Power profile', lockDetail.power_profile],
                    ['Heartbeat (s)', lockDetail.heartbeat_s],
                    ['Enrolled', fmtTime(lockDetail.enrolled_at)],
                    ['Last seen', fmtTime(lockDetail.last_seen_at)],
                  ].map(([k, v]) => (
                    <div key={k} style={{ display: 'flex', gap: 10 }}>
                      <span style={{ color: C.dim, minWidth: 120 }}>{k}</span>
                      <span style={{ wordBreak: 'break-all' }}>{v}</span>
                    </div>
                  ))}
                </div>
              )}
            </>
          )}
        </div>
      </div>

      {/* ---- General activity firehose (all incoming messages) --------- */}
      <div style={{ ...panelStyle, marginTop: 10 }}>
        <PanelTitle
          dot={C.termGreen}
          right={
            <span style={{ fontSize: 9, color: C.dim, textTransform: 'none', letterSpacing: 0 }}>
              live · click a line with an id to inspect it
            </span>
          }
        >
          All Activity
        </PanelTitle>
        <div
          ref={feedRef}
          style={{
            background: '#050B14',
            border: `1px solid ${C.panelEdge}`,
            borderRadius: 6,
            height: 240,
            overflowY: 'auto',
            padding: '10px 12px',
            fontSize: 12,
            lineHeight: 1.7,
          }}
        >
          {events.length === 0 && (
            <div style={{ color: C.dim }}>ozlock@cloud:~$ waiting for OZLOCK event stream…</div>
          )}
          {events.map((e) => {
            const ids = extractIds(e.message);
            const target = ids.deviceId
              ? () => selectLock(ids.deviceId)
              : ids.appId
                ? () => selectApp(ids.appId)
                : null;
            return (
              <div
                key={e.id}
                onClick={target || undefined}
                title={target ? 'Inspect the referenced lock/app' : undefined}
                style={{
                  whiteSpace: 'pre-wrap',
                  wordBreak: 'break-all',
                  cursor: target ? 'pointer' : 'default',
                }}
              >
                <span style={{ color: '#33445C' }}>
                  {new Date(e.ts).toLocaleTimeString('en-AU', { hour12: false })}{' '}
                </span>
                <span style={{ color: levelColor(e.level), fontWeight: 700 }}>
                  [{String(e.level).toUpperCase().padEnd(5, ' ')}]
                </span>{' '}
                <span style={{ color: target ? C.text : C.dim }}>{e.message}</span>
                {target && <span style={{ color: C.teal }}> ↩</span>}
              </div>
            );
          })}
        </div>
      </div>

      <style jsx global>{`
        html,
        body {
          margin: 0;
          background: ${C.bg};
        }
        ::-webkit-scrollbar {
          width: 8px;
          height: 8px;
        }
        ::-webkit-scrollbar-track {
          background: #0a1322;
        }
        ::-webkit-scrollbar-thumb {
          background: #2b3b52;
          border-radius: 4px;
        }
      `}</style>
    </div>
  );
}
