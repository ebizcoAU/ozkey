/*
 * ============================================================================
 *  OZLOCK PERSONAL KEYRING — BANOI stand-in (Port 4300)
 *  ---------------------------------------------------------------------------
 *  Market-A front end against OZLOCKSERV (:4200), per ozkey-05:
 *  - ADD DOORLOCK: mints an enrollment token and shows the ozkey-04 §5
 *    provision payload (in production BANOI writes it over BLE; in the lab
 *    the operator pastes it into LockSim's SERVER PUSH input)
 *  - Lock cards: label (editable), presence, device id
 *  - Key grants per lock (PIN/RFID, slot, validity) + revoke
 *  - Multi-tab terminal: event feed | keys | door transaction log
 * ============================================================================
 */

import { useCallback, useEffect, useRef, useState } from 'react';

const API = 'http://localhost:4200/ozlockserv/api';

/* ---------------------------------------------------------------------------
 * Palette — OZLOCK personal identity (teal accent vs OZKEY's hotel blue)
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

function Label({ children }) {
  return (
    <div style={{ fontSize: 9, letterSpacing: 1, color: C.dim, textTransform: 'uppercase', margin: '8px 0 3px' }}>
      {children}
    </div>
  );
}

function PanelTitle({ dot, children }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
      <span style={{ width: 7, height: 7, borderRadius: '50%', background: dot, boxShadow: `0 0 6px ${dot}` }} />
      <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: 1.2, textTransform: 'uppercase', color: C.text }}>
        {children}
      </span>
    </div>
  );
}

function agoLabel(iso) {
  if (!iso) return 'never';
  const s = Math.max(0, Math.floor((Date.now() - new Date(iso).getTime()) / 1000));
  if (s < 90) return `${s}s ago`;
  if (s < 5400) return `${Math.round(s / 60)}m ago`;
  return `${Math.round(s / 3600)}h ago`;
}

function randHex(bytes) {
  const a = new Uint8Array(bytes);
  (window.crypto || {}).getRandomValues?.(a);
  return Array.from(a, (b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * This app's self-generated identity (trust-model v2, XF-42 §13). In production
 * this is a keypair in the secure enclave; in the lab a random id in
 * localStorage, one per browser profile. OZLOCK never authenticates it.
 */
function loadAppId() {
  try {
    let id = window.localStorage.getItem('ozlock.app_id.v1');
    if (!id) {
      id = `app_${randHex(12)}`;
      window.localStorage.setItem('ozlock.app_id.v1', id);
    }
    return id;
  } catch {
    return 'app_ephemeral';
  }
}

/* ===========================================================================
 * Main keyring
 * ========================================================================= */
export default function Keyring() {
  /* -- live server state -- */
  const [locks, setLocks] = useState([]);
  const [gatewayUp, setGatewayUp] = useState(false);
  const [mqttUp, setMqttUp] = useState(false);

  /* -- this app's identity (v2: self-generated, unauthenticated) -- */
  const [appId, setAppId] = useState('');
  useEffect(() => {
    setAppId(loadAppId());
  }, []);

  /* -- Add Doorlock flow -- */
  const [enroll, setEnroll] = useState(null); // {token, provision_payload, status}
  const [enrollLabel, setEnrollLabel] = useState('');
  const [enrollDeviceId, setEnrollDeviceId] = useState(''); // blank = generate
  const [enrollBusy, setEnrollBusy] = useState(false);
  const [copied, setCopied] = useState(false);

  /* -- selection + grants -- */
  const [selectedId, setSelectedId] = useState('');
  const selectedIdRef = useRef('');
  useEffect(() => {
    selectedIdRef.current = selectedId;
  }, [selectedId]);
  const [grants, setGrants] = useState([]);
  const [lockLog, setLockLog] = useState([]);
  const [grantForm, setGrantForm] = useState({
    user_name: '',
    type: 'pin',
    raw_value: '',
    slot_number: 1,
    date_from: '',
    date_to: '',
  });
  const [grantBusy, setGrantBusy] = useState(false);
  const [revokeBusyId, setRevokeBusyId] = useState(null);
  const [unlockBusy, setUnlockBusy] = useState(false);
  const [labelDraft, setLabelDraft] = useState(null); // {id, value} while editing

  /* -- terminal -- */
  const [termTab, setTermTab] = useState('terminal');
  const [logs, setLogs] = useState([]);
  const lastEventIdRef = useRef(0);
  const termRef = useRef(null);
  const logSeqRef = useRef(0);

  const appendLog = useCallback((level, message) => {
    setLogs((prev) => {
      const next = [
        ...prev,
        { key: `local-${++logSeqRef.current}`, ts: new Date().toISOString(), level, message },
      ];
      return next.length > 400 ? next.slice(next.length - 400) : next;
    });
  }, []);

  /* -------------------------------------------------------------------------
   * Polling loop: locks + events (+ selected lock's grants/log)
   * ----------------------------------------------------------------------- */
  useEffect(() => {
    let alive = true;

    async function poll() {
      try {
        const [healthRes, locksRes, eventsRes] = await Promise.all([
          fetch(`${API}/health`).then((r) => r.json()),
          fetch(`${API}/locks`).then((r) => r.json()),
          fetch(`${API}/events?after=${lastEventIdRef.current}`).then((r) => r.json()),
        ]);
        if (!alive) return;

        setGatewayUp(true);
        setMqttUp(!!healthRes.mqtt);
        if (locksRes.ok) setLocks(locksRes.locks);
        if (eventsRes.ok && eventsRes.events.length) {
          lastEventIdRef.current = eventsRes.events[eventsRes.events.length - 1].id;
          setLogs((prev) => {
            const merged = [
              ...prev,
              ...eventsRes.events.map((e) => ({
                key: `srv-${e.id}`,
                ts: e.ts,
                level: e.level,
                message: e.message,
              })),
            ];
            return merged.length > 400 ? merged.slice(merged.length - 400) : merged;
          });
        }

        if (selectedIdRef.current) {
          const id = encodeURIComponent(selectedIdRef.current);
          const [grantsRes, logRes] = await Promise.all([
            fetch(`${API}/locks/${id}/grants`).then((r) => r.json()),
            fetch(`${API}/locks/${id}/log`).then((r) => r.json()),
          ]);
          if (!alive) return;
          if (grantsRes.ok) setGrants(grantsRes.grants);
          if (logRes.ok) setLockLog(logRes.log);
        }
      } catch (err) {
        if (!alive) return;
        setGatewayUp(false);
        setMqttUp(false);
      }
    }

    poll();
    const timer = setInterval(poll, 2500);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, []);

  /* immediate refresh on selection change */
  useEffect(() => {
    if (!selectedId) {
      setGrants([]);
      setLockLog([]);
      return;
    }
    const id = encodeURIComponent(selectedId);
    fetch(`${API}/locks/${id}/grants`)
      .then((r) => r.json())
      .then((d) => d.ok && setGrants(d.grants))
      .catch(() => {});
    fetch(`${API}/locks/${id}/log`)
      .then((r) => r.json())
      .then((d) => d.ok && setLockLog(d.log))
      .catch(() => {});
  }, [selectedId]);

  /* pairing status poll until the doorlock makes first contact (enrolled) */
  useEffect(() => {
    if (!enroll || enroll.status === 'enrolled') return;
    const timer = setInterval(async () => {
      try {
        const d = await fetch(`${API}/pairings/status?device_id=${encodeURIComponent(enroll.device_id)}`).then((r) => r.json());
        if (d.ok && d.status === 'enrolled') {
          setEnroll((e) => (e ? { ...e, status: 'enrolled' } : e));
          appendLog('pair', `Doorlock ${d.device_id} made first contact — pairing live`);
          setSelectedId(d.device_id);
        }
      } catch (_) {}
    }, 2000);
    return () => clearInterval(timer);
  }, [enroll, appendLog]);

  /* autoscroll the terminal (log-style tabs only) */
  useEffect(() => {
    if (termTab !== 'keys' && termRef.current)
      termRef.current.scrollTop = termRef.current.scrollHeight;
  }, [logs, lockLog, termTab]);

  /* -------------------------------------------------------------------------
   * Actions
   * ----------------------------------------------------------------------- */
  const beginEnroll = async () => {
    setEnrollBusy(true);
    setCopied(false);
    try {
      // v2 pairing (XF-42 §13.2): the app registers an {app_id ⇄ device_id}
      // bond. Normally the app grants a fresh random device_id; on the LockSim
      // bench you paste the lock's own device_id (the ID-exchange model — no
      // BLE) so both sides rendezvous on the same id.
      const deviceId = enrollDeviceId.trim() || `ozl-${randHex(16)}`;
      const d = await fetch(`${API}/pairings`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ label: enrollLabel || undefined, app_id: appId, device_id: deviceId }),
      }).then((r) => r.json());
      if (d.ok) {
        setEnroll({ device_id: d.device_id, provision_payload: d.provision_payload, status: 'pending' });
        appendLog('pair', `Pairing registered for ${d.device_id} — write payload to the doorlock (LockSim: SERVER PUSH)`);
      } else {
        appendLog('error', `PAIRING failed — ${d.error}`);
      }
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    } finally {
      setEnrollBusy(false);
    }
  };

  const copyPayload = async () => {
    if (!enroll) return;
    try {
      await navigator.clipboard.writeText(JSON.stringify(enroll.provision_payload));
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch (_) {
      appendLog('warn', 'Clipboard unavailable — copy the payload manually');
    }
  };

  const saveLabel = async (id, value) => {
    setLabelDraft(null);
    try {
      const d = await fetch(`${API}/locks/${encodeURIComponent(id)}`, {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ label: value }),
      }).then((r) => r.json());
      if (d.ok) appendLog('info', `Lock ${id} renamed to "${value}"`);
      else appendLog('error', `RENAME failed — ${d.error}`);
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    }
  };

  const removeLock = async (id, e) => {
    if (e) e.stopPropagation();
    if (!confirm(`Remove doorlock ${id}?\nThis also deletes its keys, queue and logs.`)) return;
    try {
      const d = await fetch(`${API}/locks/${encodeURIComponent(id)}`, {
        method: 'DELETE',
      }).then((r) => r.json());
      if (d.ok) {
        appendLog('info', `Doorlock ${id} removed`);
        if (selectedId === id) setSelectedId(null);
      } else {
        appendLog('error', `DELETE failed — ${d.error}`);
      }
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    }
  };

  const clearAll = async () => {
    if (
      !confirm(
        'Clear ALL doorlocks and start fresh?\nThis wipes every lock, key, queue and log for the lab site.'
      )
    )
      return;
    try {
      const d = await fetch(`${API}/locks`, { method: 'DELETE' }).then((r) => r.json());
      if (d.ok) {
        appendLog('warn', `Fleet cleared — ${d.removed} doorlock(s) removed`);
        setSelectedId(null);
      } else {
        appendLog('error', `CLEAR failed — ${d.error}`);
      }
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    }
  };

  const doGrant = async () => {
    if (!selectedId || !grantForm.user_name || !grantForm.raw_value) {
      appendLog('warn', 'Grant needs a selected lock, a key holder name and a value');
      return;
    }
    setGrantBusy(true);
    try {
      const d = await fetch(`${API}/locks/${encodeURIComponent(selectedId)}/grants`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ...grantForm,
          slot_number: Number(grantForm.slot_number) || 1,
          date_from: grantForm.date_from || undefined,
          date_to: grantForm.date_to || undefined,
        }),
      }).then((r) => r.json());
      if (d.ok) {
        appendLog('key', `GRANTED ${grantForm.type.toUpperCase()} to "${grantForm.user_name}" (grant #${d.grant_id}) — frame ${d.payload_hex}`);
        setGrantForm((f) => ({ ...f, user_name: '', raw_value: '' }));
        setTermTab('keys');
      } else {
        appendLog('error', `GRANT REJECTED — ${d.error}`);
      }
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    } finally {
      setGrantBusy(false);
    }
  };

  const doUnlock = async () => {
    if (!selectedId) return;
    setUnlockBusy(true);
    appendLog('key', `Remote unlock → "${selected?.label || selectedId}" via OZLOCK …`);
    try {
      const d = await fetch(`${API}/locks/${encodeURIComponent(selectedId)}/unlock`, {
        method: 'POST',
      }).then((r) => r.json());
      if (d.ok) {
        appendLog('key', `UNLOCK ${d.delivery.toUpperCase()} — frame ${d.payload_hex} (expires ${new Date(d.expires_at).toLocaleTimeString('en-AU', { hour12: false })})`);
        setTermTab('lock');
      } else {
        appendLog('error', `UNLOCK REJECTED — ${d.error}`);
      }
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    } finally {
      setUnlockBusy(false);
    }
  };

  const doRevoke = async (g) => {
    setRevokeBusyId(g.id);
    try {
      const d = await fetch(`${API}/locks/${encodeURIComponent(g.device_id)}/grants/${g.id}`, {
        method: 'DELETE',
      }).then((r) => r.json());
      if (d.ok) appendLog('key', `REVOKE QUEUED — grant #${g.id} frame ${d.payload_hex}`);
      else appendLog('error', `REVOKE REJECTED — ${d.error}`);
    } catch (err) {
      appendLog('error', `OZLOCK unreachable: ${err.message}`);
    } finally {
      setRevokeBusyId(null);
    }
  };

  /* -------------------------------------------------------------------------
   * Derived data
   * ----------------------------------------------------------------------- */
  const selected = locks.find((l) => l.id === selectedId) || null;

  const levelColor = (level) =>
    ({
      error: C.red,
      warn: C.amber,
      pair: C.blue,
      key: C.violet,
      sync: C.teal,
      lock: C.blue,
    }[level] || C.termGreen);

  const grantStatusColor = (s) =>
    ({ pending: C.amber, synced: C.green, revoking: C.red, revoked: C.gray }[s] || C.dim);

  const accessResultColor = (r) =>
    ({ granted: C.green, denied: C.red, expired: C.amber }[r] || C.dim);

  const renderLogLine = (l) => (
    <div key={l.key} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
      <span style={{ color: '#33445C' }}>
        {new Date(l.ts).toLocaleTimeString('en-AU', { hour12: false })}{' '}
      </span>
      <span style={{ color: levelColor(l.level), fontWeight: 700 }}>
        [{String(l.level).toUpperCase().padEnd(6, ' ')}]
      </span>{' '}
      <span style={{ color: C.termGreen }}>{l.message}</span>
    </div>
  );

  const lockTransactions = [...lockLog].reverse();

  const presence = (l) => {
    if (!l.last_seen_at) return { col: C.gray, label: 'NEVER SEEN' };
    const s = (Date.now() - new Date(l.last_seen_at).getTime()) / 1000;
    if (s < Math.max(90, (l.heartbeat_s || 60) * 2)) return { col: C.green, label: 'ONLINE' };
    return { col: C.amber, label: `ASLEEP · ${agoLabel(l.last_seen_at)}` };
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
        fontFamily:
          "'SF Mono', 'JetBrains Mono', Menlo, Consolas, 'Liberation Mono', monospace",
        padding: 12,
      }}
    >
      {/* == Header ========================================================== */}
      <div
        style={{
          ...panelStyle,
          padding: '8px 14px',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          marginBottom: 10,
          flexWrap: 'wrap',
          gap: 10,
        }}
      >
        <div style={{ fontSize: 14, fontWeight: 800, letterSpacing: 1.5 }}>
          OZLOCK <span style={{ color: C.teal }}>//</span> PERSONAL KEYRING
          <span style={{ color: C.dim, fontSize: 10, marginLeft: 10, fontWeight: 400 }}>
            BANOI stand-in · app{' '}
            <span style={{ color: C.teal }}>{appId || '…'}</span>
          </span>
        </div>
        <div style={{ display: 'flex', gap: 14, fontSize: 10 }}>
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

      {/* == Main split: fleet | add + grant ================================== */}
      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', alignItems: 'stretch' }}>
        {/* -- My doorlocks ------------------------------------------------- */}
        <div style={{ ...panelStyle, flex: '3 1 480px' }}>
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
            <PanelTitle dot={C.teal}>My Doorlocks</PanelTitle>
            {locks.length > 0 && (
              <button
                onClick={clearAll}
                title="Delete every doorlock and start fresh"
                style={{
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                  fontSize: 9,
                  fontWeight: 800,
                  letterSpacing: 1,
                  padding: '4px 10px',
                  borderRadius: 4,
                  border: `1px solid ${C.red}`,
                  background: 'transparent',
                  color: C.red,
                }}
              >
                CLEAR ALL
              </button>
            )}
          </div>
          {locks.length === 0 && (
            <div style={{ color: C.dim, fontSize: 12, padding: 16 }}>
              No doorlocks yet — use ADD DOORLOCK to enroll one (LockSim on :3100 is your hardware).
            </div>
          )}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            {locks.map((l) => {
              const p = presence(l);
              const isSel = l.id === selectedId;
              const editing = labelDraft && labelDraft.id === l.id;
              return (
                <div
                  key={l.id}
                  onClick={() => setSelectedId(l.id)}
                  style={{
                    cursor: 'pointer',
                    border: `1px solid ${isSel ? C.teal : C.panelEdge}`,
                    background: isSel ? '#0F2A26' : C.bg,
                    borderRadius: 8,
                    padding: '10px 12px',
                    display: 'flex',
                    alignItems: 'center',
                    gap: 12,
                    flexWrap: 'wrap',
                  }}
                >
                  <span
                    style={{
                      width: 9,
                      height: 9,
                      borderRadius: '50%',
                      background: p.col,
                      boxShadow: `0 0 6px ${p.col}`,
                      flexShrink: 0,
                    }}
                  />
                  <div style={{ flex: '1 1 200px', minWidth: 0 }}>
                    {editing ? (
                      <input
                        autoFocus
                        style={{ ...inputStyle, padding: '4px 8px', fontSize: 13 }}
                        value={labelDraft.value}
                        onClick={(e) => e.stopPropagation()}
                        onChange={(e) => setLabelDraft({ id: l.id, value: e.target.value })}
                        onKeyDown={(e) => {
                          if (e.key === 'Enter') saveLabel(l.id, labelDraft.value);
                          if (e.key === 'Escape') setLabelDraft(null);
                        }}
                        onBlur={() => saveLabel(l.id, labelDraft.value)}
                      />
                    ) : (
                      <div
                        style={{ fontSize: 13, fontWeight: 700 }}
                        title="Click to rename"
                        onClick={(e) => {
                          e.stopPropagation();
                          setSelectedId(l.id);
                          setLabelDraft({ id: l.id, value: l.label });
                        }}
                      >
                        {l.label} <span style={{ color: C.dim, fontSize: 10 }}>✎</span>
                      </div>
                    )}
                    <div style={{ fontSize: 10, color: C.dim, overflow: 'hidden', textOverflow: 'ellipsis' }}>
                      {l.id} · {l.mac}
                      {l.app_id && (
                        <span style={{ color: l.app_id === appId ? C.teal : C.amber }}>
                          {' '}· {l.app_id === appId ? 'this app' : `app ${l.app_id}`}
                        </span>
                      )}
                    </div>
                  </div>
                  <div style={{ fontSize: 10, color: p.col, fontWeight: 700, whiteSpace: 'nowrap' }}>
                    {p.label}
                  </div>
                  <div style={{ fontSize: 10, color: C.dim, whiteSpace: 'nowrap' }}>
                    wake {l.heartbeat_s}s · {l.power_profile}
                  </div>
                  <button
                    onClick={(e) => removeLock(l.id, e)}
                    title="Remove this doorlock"
                    style={{
                      cursor: 'pointer',
                      fontFamily: 'inherit',
                      fontSize: 12,
                      fontWeight: 800,
                      lineHeight: 1,
                      padding: '4px 8px',
                      borderRadius: 4,
                      border: `1px solid ${C.panelEdge}`,
                      background: 'transparent',
                      color: C.dim,
                      flexShrink: 0,
                    }}
                  >
                    ✕
                  </button>
                </div>
              );
            })}
          </div>
        </div>

        {/* -- Right column: add doorlock + grant key ------------------------ */}
        <div style={{ flex: '2 1 360px', display: 'flex', flexDirection: 'column', gap: 10 }}>
          {/* Add doorlock */}
          <div style={panelStyle}>
            <PanelTitle dot={C.blue}>Add Doorlock</PanelTitle>
            <input
              style={{ ...inputStyle, marginBottom: 8 }}
              placeholder="Device ID from the doorlock (blank = auto-generate)"
              value={enrollDeviceId}
              onChange={(e) => setEnrollDeviceId(e.target.value)}
            />
            <div style={{ fontSize: 9, color: C.dim, marginBottom: 8 }}>
              Bench: paste LockSim&apos;s Device ID (System Settings → OZLOCK) here so both
              rendezvous on the same id. On real hardware BANOI grants this over BLE.
            </div>
            <div style={{ display: 'flex', gap: 8 }}>
              <input
                style={{ ...inputStyle, flex: 1 }}
                placeholder='Name (e.g. "Front Door VN")'
                value={enrollLabel}
                onChange={(e) => setEnrollLabel(e.target.value)}
              />
              <button
                onClick={beginEnroll}
                disabled={enrollBusy}
                style={{
                  cursor: enrollBusy ? 'wait' : 'pointer',
                  fontFamily: 'inherit',
                  fontWeight: 800,
                  letterSpacing: 1,
                  fontSize: 11,
                  padding: '8px 14px',
                  borderRadius: 6,
                  border: 'none',
                  background: enrollBusy ? C.gray : C.blue,
                  color: '#04131E',
                  whiteSpace: 'nowrap',
                }}
              >
                {enrollBusy ? '…' : '+ ADD DOORLOCK'}
              </button>
            </div>

            {enroll && (
              <div style={{ marginTop: 10 }}>
                <div style={{ fontSize: 10, color: C.dim, marginBottom: 4 }}>
                  Provision payload — in production BANOI writes this over BLE; in the lab paste it
                  into LockSim&apos;s <span style={{ color: C.amber }}>SERVER PUSH</span> input:
                </div>
                <div
                  style={{
                    background: '#050B14',
                    border: `1px solid ${C.panelEdge}`,
                    borderRadius: 6,
                    padding: '8px 10px',
                    fontSize: 10,
                    color: C.teal,
                    wordBreak: 'break-all',
                    maxHeight: 110,
                    overflowY: 'auto',
                  }}
                >
                  {JSON.stringify(enroll.provision_payload)}
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginTop: 8 }}>
                  <button
                    onClick={copyPayload}
                    style={{
                      cursor: 'pointer',
                      fontFamily: 'inherit',
                      fontWeight: 700,
                      fontSize: 10,
                      letterSpacing: 1,
                      padding: '6px 12px',
                      borderRadius: 5,
                      border: `1px solid ${C.teal}`,
                      background: 'transparent',
                      color: C.teal,
                    }}
                  >
                    {copied ? '✓ COPIED' : 'COPY PAYLOAD'}
                  </button>
                  <span
                    style={{
                      fontSize: 10,
                      fontWeight: 700,
                      color: enroll.status === 'enrolled' ? C.green : C.amber,
                    }}
                  >
                    {enroll.status === 'enrolled'
                      ? `● PAIRED — ${enroll.device_id}`
                      : '● WAITING FOR DOORLOCK FIRST CONTACT…'}
                  </span>
                </div>
              </div>
            )}
          </div>

          {/* Grant a key */}
          <div style={panelStyle}>
            <PanelTitle dot={C.violet}>Grant a Key {selected ? `— ${selected.label}` : ''}</PanelTitle>
            {!selected && (
              <div style={{ color: C.dim, fontSize: 11 }}>Select a doorlock to grant keys.</div>
            )}
            {selected && (
              <>
                <button
                  onClick={doUnlock}
                  disabled={unlockBusy || selected.status !== 'enrolled'}
                  title={selected.status !== 'enrolled' ? 'Lock not enrolled yet' : 'Send a remote unlock via OZLOCK'}
                  style={{
                    width: '100%',
                    cursor: unlockBusy || selected.status !== 'enrolled' ? 'not-allowed' : 'pointer',
                    fontFamily: 'inherit',
                    fontWeight: 800,
                    letterSpacing: 1,
                    fontSize: 12,
                    padding: '11px 0',
                    borderRadius: 6,
                    border: `1px solid ${C.teal}`,
                    background: unlockBusy ? C.gray : 'rgba(45,212,191,0.12)',
                    color: C.teal,
                    marginBottom: 4,
                  }}
                >
                  {unlockBusy ? 'SENDING…' : '🔓 MỞ CỬA — REMOTE UNLOCK'}
                </button>
                <div style={{ fontSize: 9, color: C.dim, marginBottom: 6 }}>
                  Away-path unlock relayed through OZLOCK (expires 60 s). At the
                  door, BANOI would open over BLE instead — no server round-trip.
                </div>

                <Label>Key Holder</Label>
                <input
                  style={inputStyle}
                  value={grantForm.user_name}
                  placeholder='e.g. "Cleaner Mai"'
                  onChange={(e) => setGrantForm((f) => ({ ...f, user_name: e.target.value }))}
                />
                <div style={{ display: 'flex', gap: 10 }}>
                  <div style={{ flex: 2 }}>
                    <Label>Type</Label>
                    <select
                      style={inputStyle}
                      value={grantForm.type}
                      onChange={(e) => setGrantForm((f) => ({ ...f, type: e.target.value }))}
                    >
                      <option value="pin">PIN Code</option>
                      <option value="rfid">RFID Token</option>
                    </select>
                  </div>
                  <div style={{ flex: 1 }}>
                    <Label>Slot</Label>
                    <input
                      style={inputStyle}
                      type="number"
                      min={1}
                      max={255}
                      value={grantForm.slot_number}
                      onChange={(e) => setGrantForm((f) => ({ ...f, slot_number: e.target.value }))}
                    />
                  </div>
                </div>
                <Label>{grantForm.type === 'pin' ? 'PIN Digits' : 'RFID UID (hex)'}</Label>
                <input
                  style={inputStyle}
                  value={grantForm.raw_value}
                  placeholder={grantForm.type === 'pin' ? '482913' : '04A2B91C'}
                  onChange={(e) => setGrantForm((f) => ({ ...f, raw_value: e.target.value }))}
                />
                <div style={{ display: 'flex', gap: 10 }}>
                  <div style={{ flex: 1 }}>
                    <Label>Valid From</Label>
                    <input
                      style={inputStyle}
                      type="datetime-local"
                      value={grantForm.date_from}
                      onChange={(e) => setGrantForm((f) => ({ ...f, date_from: e.target.value }))}
                    />
                  </div>
                  <div style={{ flex: 1 }}>
                    <Label>Valid To</Label>
                    <input
                      style={inputStyle}
                      type="datetime-local"
                      value={grantForm.date_to}
                      onChange={(e) => setGrantForm((f) => ({ ...f, date_to: e.target.value }))}
                    />
                  </div>
                </div>
                <button
                  onClick={doGrant}
                  disabled={grantBusy}
                  style={{
                    marginTop: 14,
                    width: '100%',
                    cursor: grantBusy ? 'wait' : 'pointer',
                    fontFamily: 'inherit',
                    fontWeight: 800,
                    letterSpacing: 1,
                    fontSize: 12,
                    padding: '11px 0',
                    borderRadius: 6,
                    border: 'none',
                    background: grantBusy ? C.gray : '#7C3AED',
                    color: '#F5F3FF',
                  }}
                >
                  {grantBusy ? 'QUEUEING 55 AA FRAME…' : 'GRANT KEY → DELIVER ON WAKE'}
                </button>
              </>
            )}
          </div>
        </div>
      </div>

      {/* == Multi-tab terminal ============================================== */}
      <div style={{ ...panelStyle, marginTop: 10 }}>
        <div style={{ display: 'flex', gap: 6, marginBottom: 8, flexWrap: 'wrap' }}>
          {[
            ['terminal', 'Activity', C.termGreen],
            ['keys', `Keys${selected ? ` · ${selected.label}` : ''}`, C.violet],
            ['lock', `Door Log${selected ? ` · ${selected.label}` : ''}`, C.blue],
          ].map(([id, label, col]) => (
            <button
              key={id}
              onClick={() => setTermTab(id)}
              style={{
                cursor: 'pointer',
                fontFamily: 'inherit',
                display: 'flex',
                alignItems: 'center',
                gap: 6,
                fontSize: 10,
                fontWeight: 700,
                letterSpacing: 1.2,
                textTransform: 'uppercase',
                padding: '6px 12px',
                borderRadius: 5,
                border: `1px solid ${termTab === id ? col : C.panelEdge}`,
                background: termTab === id ? '#050B14' : 'transparent',
                color: termTab === id ? col : C.dim,
              }}
            >
              <span
                style={{
                  width: 7,
                  height: 7,
                  borderRadius: '50%',
                  background: col,
                  boxShadow: termTab === id ? `0 0 6px ${col}` : 'none',
                }}
              />
              {label}
            </button>
          ))}
        </div>

        <div
          ref={termRef}
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
          {termTab === 'terminal' && (
            <>
              {logs.length === 0 && (
                <div style={{ color: C.dim }}>ozlock@cloud:~$ waiting for OZLOCK event stream…</div>
              )}
              {logs.map(renderLogLine)}
            </>
          )}

          {termTab === 'keys' &&
            (!selected ? (
              <div style={{ color: C.dim }}>ozlock@cloud:~$ select a doorlock to list its keys…</div>
            ) : (
              <>
                <div style={{ color: C.dim, marginBottom: 8 }}>
                  {selected.label} · {grants.length} key{grants.length === 1 ? '' : 's'}
                </div>
                {grants.length === 0 && (
                  <div style={{ color: C.dim }}>— no keys granted on this doorlock yet —</div>
                )}
                {grants.length > 0 && (
                  <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                    <thead>
                      <tr>
                        {['ID', 'TYPE', 'SLOT', 'HOLDER', 'VALUE', 'VALID (FROM → TO)', 'STATUS', ''].map((h) => (
                          <th
                            key={h}
                            style={{
                              textAlign: 'left',
                              color: C.dim,
                              fontWeight: 700,
                              letterSpacing: 1,
                              fontSize: 9,
                              padding: '2px 8px 6px 0',
                              borderBottom: `1px solid ${C.panelEdge}`,
                            }}
                          >
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {grants.map((g) => (
                        <tr key={g.id}>
                          <td style={{ padding: '5px 8px 5px 0', color: C.dim }}>#{g.id}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: C.violet, fontWeight: 700 }}>
                            {String(g.type).toUpperCase()}
                          </td>
                          <td style={{ padding: '5px 8px 5px 0' }}>{g.slot_number}</td>
                          <td style={{ padding: '5px 8px 5px 0' }}>{g.user_name || '—'}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: C.termGreen }}>{g.raw_value}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: C.dim }}>
                            {String(g.date_from || '').slice(0, 16)} → {String(g.date_to || '').slice(0, 16)}
                          </td>
                          <td
                            style={{
                              padding: '5px 8px 5px 0',
                              color: grantStatusColor(g.sync_status),
                              fontWeight: 700,
                            }}
                          >
                            {String(g.sync_status).toUpperCase()}
                          </td>
                          <td style={{ padding: '5px 0' }}>
                            {['pending', 'synced'].includes(g.sync_status) && (
                              <button
                                onClick={() => doRevoke(g)}
                                disabled={revokeBusyId === g.id}
                                style={{
                                  cursor: revokeBusyId === g.id ? 'wait' : 'pointer',
                                  fontFamily: 'inherit',
                                  fontSize: 9,
                                  fontWeight: 800,
                                  letterSpacing: 1,
                                  padding: '3px 8px',
                                  borderRadius: 4,
                                  border: `1px solid ${C.red}`,
                                  background: 'transparent',
                                  color: C.red,
                                }}
                              >
                                {revokeBusyId === g.id ? '…' : 'REVOKE'}
                              </button>
                            )}
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </>
            ))}

          {termTab === 'lock' &&
            (!selected ? (
              <div style={{ color: C.dim }}>ozlock@cloud:~$ select a doorlock to tail its door log…</div>
            ) : (
              <>
                <div style={{ color: C.dim, marginBottom: 8 }}>
                  {selected.label} ({selected.id}) · {lockTransactions.length} door transaction
                  {lockTransactions.length === 1 ? '' : 's'} (latest 100)
                </div>
                {lockTransactions.length === 0 && (
                  <div style={{ color: C.dim }}>— no door transactions recorded yet —</div>
                )}
                {lockTransactions.map((t) => (
                  <div key={t.id} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                    <span style={{ color: '#33445C' }}>
                      {new Date(t.lock_ts || t.created_at).toLocaleTimeString('en-AU', { hour12: false })}{' '}
                    </span>
                    <span style={{ color: accessResultColor(t.result), fontWeight: 700 }}>
                      [{String(t.result).toUpperCase().padEnd(7, ' ')}]
                    </span>{' '}
                    <span style={{ color: C.termGreen }}>{t.detail || '—'}</span>
                  </div>
                ))}
              </>
            ))}
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
        select option {
          background: ${C.panel};
        }
      `}</style>
    </div>
  );
}
