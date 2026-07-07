/*
 * ============================================================================
 *  OZKEY CORE COCKPIT — Sovereign Smart Lock Laboratory Dashboard (Port 3300)
 *  ---------------------------------------------------------------------------
 *  - 30-room pairing matrix (Block A) driven live from OZKEYSERV (Port 3200)
 *  - Discovered Unpaired Hardware panel (MQTT discovery + Web Serial capture)
 *  - Credential injector (PIN / RFID / Fingerprint) via the API gateway
 *  - Web Serial desk-module reader + scrolling green lab terminal
 * ============================================================================
 */

import { useCallback, useEffect, useRef, useState } from 'react';

const API = 'http://localhost:3200/ozkeyserv/api';
const MAC_REGEX = /(?:[0-9A-F]{2}[:\-]){5}[0-9A-F]{2}|[0-9A-F]{12}/gi;

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */
const C = {
  bg: '#0F172A',
  panel: '#1E293B',
  panelEdge: '#334155',
  text: '#E2E8F0',
  dim: '#94A3B8',
  green: '#22C55E',
  blue: '#3B82F6',
  red: '#EF4444',
  gray: '#475569',
  amber: '#F59E0B',
  termGreen: '#4ADE80',
};

function normalizeMac(raw) {
  const hex = String(raw).replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(':');
}

function roomColor(room) {
  if (!room.mac_address) return { bg: C.gray, glow: 'none', label: 'UNPAIRED' };
  if (room.status === 'Occupied') return { bg: C.blue, glow: `0 0 8px ${C.blue}88`, label: 'OCCUPIED' };
  if (room.status === 'PendingUpdate') return { bg: C.red, glow: `0 0 8px ${C.red}88`, label: 'PENDING UPDATE' };
  return { bg: C.green, glow: `0 0 8px ${C.green}66`, label: 'AVAILABLE' };
}

/* ---------------------------------------------------------------------------
 * Shared UI atoms
 * ------------------------------------------------------------------------- */
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

/* ===========================================================================
 * Main cockpit
 * ========================================================================= */
export default function Cockpit() {
  /* -- live server state -- */
  const [rooms, setRooms] = useState([]);
  const [unpaired, setUnpaired] = useState([]);
  const [gatewayUp, setGatewayUp] = useState(false);
  const [mqttUp, setMqttUp] = useState(false);

  /* -- pairing cockpit -- */
  const [selectedMac, setSelectedMac] = useState('');
  const [selectedRoom, setSelectedRoom] = useState('');
  const [pairBusy, setPairBusy] = useState(false);

  /* -- credential injector -- */
  const [form, setForm] = useState({
    room_no: '',
    guest_name: '',
    type: 'pin',
    raw_value: '',
    slot_number: 1,
    date_from: '',
    date_to: '',
  });
  const [issueBusy, setIssueBusy] = useState(false);

  /* -- serial link -- */
  const [serialSupported, setSerialSupported] = useState(false);
  const [serialConnected, setSerialConnected] = useState(false);
  const [serialMacs, setSerialMacs] = useState([]);
  const portRef = useRef(null);
  const readerRef = useRef(null);
  const keepReadingRef = useRef(false);

  /* -- terminal -- */
  const [logs, setLogs] = useState([]);
  const lastEventIdRef = useRef(0);
  const termRef = useRef(null);
  const logSeqRef = useRef(0);

  /* -- multi-tab terminal: lab feed | room key list | per-lock log -- */
  const [termTab, setTermTab] = useState('terminal');
  const [inspectedRoom, setInspectedRoom] = useState('');
  const [credentials, setCredentials] = useState([]);
  const [revokeBusyId, setRevokeBusyId] = useState(null);
  const [lockLog, setLockLog] = useState([]);
  /* ref mirror so the long-lived poll loop reads the current selection */
  const inspectedRoomRef = useRef('');
  useEffect(() => {
    inspectedRoomRef.current = inspectedRoom;
  }, [inspectedRoom]);

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
   * Polling loop: rooms + unpaired discovery + server event stream
   * ----------------------------------------------------------------------- */
  useEffect(() => {
    let alive = true;

    async function poll() {
      try {
        const [healthRes, roomsRes, unpairedRes, eventsRes, credsRes] = await Promise.all([
          fetch(`${API}/health`).then((r) => r.json()),
          fetch(`${API}/rooms`).then((r) => r.json()),
          fetch(`${API}/locks/unpaired`).then((r) => r.json()),
          fetch(`${API}/events?after=${lastEventIdRef.current}`).then((r) => r.json()),
          fetch(`${API}/credentials`).then((r) => r.json()),
        ]);
        if (!alive) return;

        setGatewayUp(true);
        setMqttUp(!!healthRes.mqtt);
        if (roomsRes.ok) setRooms(roomsRes.rooms);
        if (unpairedRes.ok) setUnpaired(unpairedRes.unpaired);
        if (credsRes.ok) setCredentials(credsRes.credentials);

        if (inspectedRoomRef.current) {
          const logRes = await fetch(
            `${API}/locks/log?room_no=${encodeURIComponent(inspectedRoomRef.current)}`
          ).then((r) => r.json());
          if (!alive) return;
          if (logRes.ok) setLockLog(logRes.log);
        }
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

  /* refresh the door transaction log as soon as a room is selected */
  useEffect(() => {
    if (!inspectedRoom) {
      setLockLog([]);
      return;
    }
    fetch(`${API}/locks/log?room_no=${encodeURIComponent(inspectedRoom)}`)
      .then((r) => r.json())
      .then((d) => {
        if (d.ok) setLockLog(d.log);
      })
      .catch(() => {});
  }, [inspectedRoom]);

  /* autoscroll the terminal (log-style tabs only, not the key table) */
  useEffect(() => {
    if (termTab !== 'keys' && termRef.current)
      termRef.current.scrollTop = termRef.current.scrollHeight;
  }, [logs, lockLog, termTab]);

  /* Web Serial capability check (client only) */
  useEffect(() => {
    setSerialSupported(typeof navigator !== 'undefined' && 'serial' in navigator);
  }, []);

  /* -------------------------------------------------------------------------
   * Web Serial — desk test module reader
   * ----------------------------------------------------------------------- */
  /* ref mirror of serialMacs so the long-lived read loop sees fresh state */
  const serialMacsRef = useRef([]);
  useEffect(() => {
    serialMacsRef.current = serialMacs;
  }, [serialMacs]);

  const registerSerialMac = useCallback(
    async (mac) => {
      setSerialMacs((prev) => (prev.includes(mac) ? prev : [...prev, mac]));
      appendLog('serial', `MAC captured on serial link: ${mac}`);
      // Feed the gateway's discovery cache so pairing works from either source.
      try {
        await fetch(`${API}/sim/unpaired-heartbeat`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ mac_address: mac }),
        });
      } catch (_) {
        appendLog('warn', `Gateway offline — ${mac} kept locally only`);
      }
    },
    [appendLog]
  );

  const disconnectSerial = useCallback(async () => {
    keepReadingRef.current = false;
    try {
      if (readerRef.current) await readerRef.current.cancel();
    } catch (_) {}
    try {
      if (portRef.current) await portRef.current.close();
    } catch (_) {}
    readerRef.current = null;
    portRef.current = null;
    setSerialConnected(false);
    appendLog('serial', 'Serial link closed');
  }, [appendLog]);

  const connectSerial = useCallback(async () => {
    if (!serialSupported) {
      appendLog('error', 'Web Serial API unavailable — use Chrome/Edge over localhost or HTTPS');
      return;
    }
    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      portRef.current = port;
      keepReadingRef.current = true;
      setSerialConnected(true);
      appendLog('serial', 'Serial link OPEN @ 115200 baud — listening for desk module');

      const decoder = new TextDecoder();
      let lineBuf = '';

      while (keepReadingRef.current && port.readable) {
        const reader = port.readable.getReader();
        readerRef.current = reader;
        try {
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            lineBuf += decoder.decode(value, { stream: true });

            // Emit complete lines to the terminal.
            const lines = lineBuf.split(/\r?\n/);
            lineBuf = lines.pop();
            for (const line of lines) {
              const clean = line.trim();
              if (clean) appendLog('rx', `<< ${clean}`);
            }

            // Scan the whole rolling window for MAC addresses.
            const scanWindow = lines.join('\n') + '\n' + lineBuf;
            const matches = scanWindow.match(MAC_REGEX) || [];
            for (const m of matches) {
              const mac = normalizeMac(m);
              if (mac && !serialMacsRef.current.includes(mac)) {
                await registerSerialMac(mac);
              }
            }
          }
        } catch (err) {
          appendLog('error', `Serial read fault: ${err.message}`);
        } finally {
          try {
            reader.releaseLock();
          } catch (_) {}
        }
      }
    } catch (err) {
      appendLog('error', `Serial connect aborted: ${err.message}`);
      setSerialConnected(false);
    }
  }, [serialSupported, appendLog, registerSerialMac]);

  /* -------------------------------------------------------------------------
   * Actions
   * ----------------------------------------------------------------------- */
  const doPair = async () => {
    if (!selectedMac || !selectedRoom) {
      appendLog('warn', 'Select both a discovered MAC and a target room before pairing');
      return;
    }
    setPairBusy(true);
    appendLog('pair', `Executing bind: ${selectedMac} -> room ${selectedRoom} ...`);
    try {
      const res = await fetch(`${API}/locks/pair`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ room_no: selectedRoom, mac_address: selectedMac }),
      });
      const data = await res.json();
      if (data.ok) {
        appendLog('pair', `PAIR OK — room ${data.room_no} now owns ${data.mac_address}`);
        setSerialMacs((prev) => prev.filter((m) => m !== selectedMac));
        setSelectedMac('');
        setSelectedRoom('');
      } else {
        appendLog('error', `PAIR REJECTED — ${data.error}`);
      }
    } catch (err) {
      appendLog('error', `Gateway unreachable: ${err.message}`);
    } finally {
      setPairBusy(false);
    }
  };

  const doIssueKey = async () => {
    if (!form.room_no || !form.guest_name || !form.raw_value) {
      appendLog('warn', 'Injector needs room, guest name and a credential value');
      return;
    }
    setIssueBusy(true);
    appendLog('key', `Injecting ${form.type.toUpperCase()} "${form.raw_value}" -> room ${form.room_no} ...`);
    try {
      const res = await fetch(`${API}/pms/issue-key`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ...form,
          slot_number: Number(form.slot_number) || 1,
          date_from: form.date_from || undefined,
          date_to: form.date_to || undefined,
        }),
      });
      const data = await res.json();
      if (data.ok) {
        appendLog('key', `QUEUED cred #${data.credential_id} — Tuya frame: ${data.payload_hex}`);
        setForm((f) => ({ ...f, guest_name: '', raw_value: '' }));
      } else {
        appendLog('error', `ISSUE REJECTED — ${data.error}`);
      }
    } catch (err) {
      appendLog('error', `Gateway unreachable: ${err.message}`);
    } finally {
      setIssueBusy(false);
    }
  };

  const doRevoke = async (cred) => {
    setRevokeBusyId(cred.id);
    appendLog('key', `Revoking cred #${cred.id} (${cred.type} slot ${cred.slot_number}) room ${cred.room_no} ...`);
    try {
      const res = await fetch(`${API}/pms/revoke-key`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ credential_id: cred.id }),
      });
      const data = await res.json();
      if (data.ok) {
        appendLog('key', `REVOKE QUEUED — cred #${data.credential_id} DPID delete frame: ${data.payload_hex}`);
      } else {
        appendLog('error', `REVOKE REJECTED — ${data.error}`);
      }
    } catch (err) {
      appendLog('error', `Gateway unreachable: ${err.message}`);
    } finally {
      setRevokeBusyId(null);
    }
  };

  /* -------------------------------------------------------------------------
   * Derived data
   * ----------------------------------------------------------------------- */
  const allDiscovered = [
    ...unpaired.map((u) => ({ mac: u.mac_address, src: u.fw === 'sim' ? 'SERIAL/SIM' : 'MQTT' })),
    ...serialMacs
      .filter((m) => !unpaired.some((u) => u.mac_address === m))
      .map((m) => ({ mac: m, src: 'SERIAL' })),
  ];

  const unpairedRooms = rooms.filter((r) => !r.mac_address);
  const pairedRooms = rooms.filter((r) => r.mac_address);

  const counts = {
    unpaired: rooms.filter((r) => !r.mac_address).length,
    available: rooms.filter((r) => r.mac_address && r.status === 'Available').length,
    occupied: rooms.filter((r) => r.status === 'Occupied').length,
    pending: rooms.filter((r) => r.status === 'PendingUpdate').length,
  };

  const levelColor = (level) =>
    ({
      error: C.red,
      warn: C.amber,
      pair: '#38BDF8',
      key: '#C084FC',
      sync: '#2DD4BF',
      serial: '#FBBF24',
      lock: '#38BDF8',
      rx: C.dim,
    }[level] || C.termGreen);

  const credStatusColor = (s) =>
    ({ pending: C.amber, synced: C.green, revoking: C.red, revoked: C.gray }[s] || C.dim);

  /* -- inspected-room views for the ROOM KEYS / DOORLOCK LOG tabs -- */
  const inspected = rooms.find((r) => r.room_no === inspectedRoom) || null;
  const roomKeys = credentials.filter((c) => c.room_no === inspectedRoom);
  // /locks/log returns newest-first; the terminal renders oldest-first.
  const lockTransactions = [...lockLog].reverse();

  const accessResultColor = (r) =>
    ({ granted: C.green, denied: C.red, expired: C.amber }[r] || C.dim);

  const renderLogLine = (l) => (
    <div key={l.key} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
      <span style={{ color: '#334155' }}>
        {new Date(l.ts).toLocaleTimeString('en-AU', { hour12: false })}{' '}
      </span>
      <span style={{ color: levelColor(l.level), fontWeight: 700 }}>
        [{String(l.level).toUpperCase().padEnd(6, ' ')}]
      </span>{' '}
      <span style={{ color: C.termGreen }}>{l.message}</span>
    </div>
  );

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
      {/* == Header (single slim row) ======================================= */}
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
          OZKEY <span style={{ color: C.green }}>//</span> LOCK COCKPIT
        </div>
        <div style={{ display: 'flex', gap: 14, fontSize: 10 }}>
          {[
            ['GATEWAY', gatewayUp],
            ['MQTT', mqttUp],
            ['SERIAL', serialConnected],
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

      {/* == Unpaired hardware + pairing (single inline row) ================ */}
      <div
        style={{
          ...panelStyle,
          padding: '8px 14px',
          marginBottom: 10,
          display: 'flex',
          alignItems: 'center',
          gap: 10,
          flexWrap: 'wrap',
        }}
      >
        <span
          style={{
            fontSize: 10,
            fontWeight: 700,
            letterSpacing: 1.2,
            textTransform: 'uppercase',
            color: C.amber,
            whiteSpace: 'nowrap',
          }}
        >
          Unpaired HW
        </span>
        <div
          style={{
            display: 'flex',
            gap: 6,
            flexWrap: 'wrap',
            alignItems: 'center',
            flex: '2 1 260px',
          }}
        >
          {allDiscovered.length === 0 && (
            <span style={{ color: C.dim, fontSize: 11 }}>— none broadcasting —</span>
          )}
          {allDiscovered.map((d) => (
            <button
              key={d.mac}
              onClick={() => setSelectedMac(d.mac)}
              style={{
                cursor: 'pointer',
                fontFamily: 'inherit',
                fontSize: 11,
                padding: '4px 8px',
                borderRadius: 5,
                border: `1px solid ${selectedMac === d.mac ? C.amber : C.panelEdge}`,
                background: selectedMac === d.mac ? '#3a2f10' : C.bg,
                color: selectedMac === d.mac ? C.amber : C.text,
              }}
            >
              {d.mac}
              <span style={{ color: C.dim, marginLeft: 5, fontSize: 9 }}>{d.src}</span>
            </button>
          ))}
        </div>

        <select
          value={selectedRoom}
          onChange={(e) => setSelectedRoom(e.target.value)}
          style={{ ...inputStyle, width: 'auto', flex: '0 1 170px', padding: '6px 8px', fontSize: 12 }}
        >
          <option value="">— target room —</option>
          {unpairedRooms.map((r) => (
            <option key={r.id} value={r.room_no}>
              F{r.floor} / {r.room_no}
            </option>
          ))}
        </select>

        <button
          onClick={doPair}
          disabled={pairBusy || !selectedMac || !selectedRoom}
          style={{
            cursor: pairBusy || !selectedMac || !selectedRoom ? 'not-allowed' : 'pointer',
            fontFamily: 'inherit',
            fontWeight: 800,
            letterSpacing: 1,
            fontSize: 11,
            padding: '7px 14px',
            borderRadius: 5,
            border: 'none',
            background: pairBusy || !selectedMac || !selectedRoom ? C.gray : C.green,
            color: '#04170A',
            whiteSpace: 'nowrap',
          }}
        >
          {pairBusy ? 'BINDING…' : 'PAIR LOCK TO ROOM'}
        </button>
      </div>

      {/* == Main split: matrix | injector + serial ========================= */}
      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', alignItems: 'stretch' }}>
        {/* -- 30-room matrix --------------------------------------------- */}
        <div style={{ ...panelStyle, flex: '3 1 520px' }}>
          <PanelTitle dot={C.blue}>Room Matrix — Block A</PanelTitle>
          <div style={{ display: 'flex', gap: 14, fontSize: 10, color: C.dim, marginBottom: 10, flexWrap: 'wrap' }}>
            {[
              [C.gray, `UNPAIRED ${counts.unpaired}`],
              [C.green, `AVAILABLE ${counts.available}`],
              [C.blue, `OCCUPIED ${counts.occupied}`],
              [C.red, `PENDING UPDATE ${counts.pending}`],
            ].map(([col, label]) => (
              <span key={label} style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
                <span style={{ width: 10, height: 10, borderRadius: 2, background: col }} />
                {label}
              </span>
            ))}
          </div>
          <div
            style={{
              display: 'grid',
              gridTemplateColumns: 'repeat(10, 1fr)',
              gap: 6,
            }}
          >
            {rooms.map((room) => {
              const cc = roomColor(room);
              return (
                <div
                  key={room.id}
                  title={`Room ${room.room_no} — ${cc.label}${
                    room.mac_address ? `\nMAC ${room.mac_address}` : '\nNo lock bound'
                  }`}
                  onClick={() => {
                    setInspectedRoom(room.room_no);
                    if (!room.mac_address) setSelectedRoom(room.room_no);
                    else setForm((f) => ({ ...f, room_no: room.room_no }));
                  }}
                  style={{
                    cursor: 'pointer',
                    aspectRatio: '1 / 1',
                    borderRadius: 5,
                    background: cc.bg,
                    boxShadow: cc.glow,
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    fontSize: 10,
                    fontWeight: 700,
                    color: room.mac_address ? '#0B1220' : '#CBD5E1',
                    border:
                      selectedRoom === room.room_no || form.room_no === room.room_no
                        ? '2px solid #FFFFFF'
                        : '2px solid transparent',
                    transition: 'box-shadow .2s, transform .1s',
                  }}
                >
                  {room.room_no}
                </div>
              );
            })}
            {rooms.length === 0 && (
              <div style={{ gridColumn: '1 / -1', color: C.dim, fontSize: 12, padding: 20 }}>
                Waiting for OZKEYSERV room matrix… (is the gateway on :3200 running?)
              </div>
            )}
          </div>
        </div>

        {/* -- Right column: injector + serial ----------------------------- */}
        <div style={{ flex: '2 1 360px', display: 'flex', flexDirection: 'column', gap: 10 }}>
          {/* Credential injector */}
          <div style={panelStyle}>
            <PanelTitle dot="#C084FC">Credential Injector</PanelTitle>

            <Label>Paired Room</Label>
            <select
              value={form.room_no}
              onChange={(e) => setForm((f) => ({ ...f, room_no: e.target.value }))}
              style={inputStyle}
            >
              <option value="">— select paired room —</option>
              {pairedRooms.map((r) => (
                <option key={r.id} value={r.room_no}>
                  Room {r.room_no} ({r.status}) — {r.mac_address}
                </option>
              ))}
            </select>

            <Label>Guest / Staff Name</Label>
            <input
              style={inputStyle}
              value={form.guest_name}
              placeholder="e.g. Jane Nguyen"
              onChange={(e) => setForm((f) => ({ ...f, guest_name: e.target.value }))}
            />

            <div style={{ display: 'flex', gap: 10 }}>
              <div style={{ flex: 2 }}>
                <Label>Type</Label>
                <select
                  style={inputStyle}
                  value={form.type}
                  onChange={(e) => setForm((f) => ({ ...f, type: e.target.value }))}
                >
                  <option value="pin">PIN Code</option>
                  <option value="rfid">RFID Token</option>
                  <option value="fingerprint">Fingerprint ID</option>
                </select>
              </div>
              <div style={{ flex: 1 }}>
                <Label>Slot</Label>
                <input
                  style={inputStyle}
                  type="number"
                  min={1}
                  max={255}
                  value={form.slot_number}
                  onChange={(e) => setForm((f) => ({ ...f, slot_number: e.target.value }))}
                />
              </div>
            </div>

            <Label>{form.type === 'pin' ? 'PIN Digits' : form.type === 'rfid' ? 'RFID UID (hex)' : 'Template ID'}</Label>
            <input
              style={inputStyle}
              value={form.raw_value}
              placeholder={form.type === 'pin' ? '482913' : form.type === 'rfid' ? '04A2B91C6F5D80' : '17'}
              onChange={(e) => setForm((f) => ({ ...f, raw_value: e.target.value }))}
            />

            <div style={{ display: 'flex', gap: 10 }}>
              <div style={{ flex: 1 }}>
                <Label>Valid From</Label>
                <input
                  style={inputStyle}
                  type="datetime-local"
                  value={form.date_from}
                  onChange={(e) => setForm((f) => ({ ...f, date_from: e.target.value }))}
                />
              </div>
              <div style={{ flex: 1 }}>
                <Label>Valid To</Label>
                <input
                  style={inputStyle}
                  type="datetime-local"
                  value={form.date_to}
                  onChange={(e) => setForm((f) => ({ ...f, date_to: e.target.value }))}
                />
              </div>
            </div>

            <button
              onClick={doIssueKey}
              disabled={issueBusy}
              style={{
                marginTop: 14,
                width: '100%',
                cursor: issueBusy ? 'wait' : 'pointer',
                fontFamily: 'inherit',
                fontWeight: 800,
                letterSpacing: 1,
                fontSize: 12,
                padding: '11px 0',
                borderRadius: 6,
                border: 'none',
                background: issueBusy ? C.gray : '#7C3AED',
                color: '#F5F3FF',
              }}
            >
              {issueBusy ? 'QUEUEING 55 AA FRAME…' : 'ISSUE KEY → QUEUE FOR HEARTBEAT'}
            </button>
          </div>

          {/* Serial link */}
          <div style={panelStyle}>
            <PanelTitle dot={C.amber}>Web Serial — 115200 baud</PanelTitle>
            <button
              onClick={serialConnected ? disconnectSerial : connectSerial}
              style={{
                width: '100%',
                cursor: 'pointer',
                fontFamily: 'inherit',
                fontWeight: 800,
                letterSpacing: 1,
                fontSize: 12,
                padding: '11px 0',
                borderRadius: 6,
                border: `1px solid ${serialConnected ? C.red : C.amber}`,
                background: 'transparent',
                color: serialConnected ? C.red : C.amber,
              }}
            >
              {serialConnected ? '■ DISCONNECT SERIAL LINK' : '▶ CONNECT SERIAL MODULE'}
            </button>
            {!serialSupported && (
              <div style={{ fontSize: 10, color: C.red, marginTop: 8 }}>
                Web Serial API not available in this browser — use Chrome or Edge on
                localhost/HTTPS.
              </div>
            )}
          </div>
        </div>
      </div>

      {/* == Multi-tab lab terminal ========================================= */}
      <div style={{ ...panelStyle, marginTop: 10 }}>
        <div style={{ display: 'flex', gap: 6, marginBottom: 8, flexWrap: 'wrap' }}>
          {[
            ['terminal', 'Lab Terminal', C.termGreen],
            ['keys', `Room Keys${inspectedRoom ? ` · ${inspectedRoom}` : ''}`, '#C084FC'],
            ['lock', `Doorlock Log${inspectedRoom ? ` · ${inspectedRoom}` : ''}`, '#38BDF8'],
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
                background: termTab === id ? '#020617' : 'transparent',
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
            background: '#020617',
            border: `1px solid ${C.panelEdge}`,
            borderRadius: 6,
            height: 260,
            overflowY: 'auto',
            padding: '10px 12px',
            fontSize: 12,
            lineHeight: 1.7,
          }}
        >
          {/* -- Tab 1: full gateway event stream -- */}
          {termTab === 'terminal' && (
            <>
              {logs.length === 0 && (
                <div style={{ color: C.dim }}>ozkey@lab:~$ waiting for gateway event stream…</div>
              )}
              {logs.map(renderLogLine)}
            </>
          )}

          {/* -- Tab 2: issued keys for the inspected room -- */}
          {termTab === 'keys' &&
            (!inspectedRoom ? (
              <div style={{ color: C.dim }}>
                ozkey@lab:~$ click a room tile in the matrix to list its issued keys…
              </div>
            ) : (
              <>
                <div style={{ color: C.dim, marginBottom: 8 }}>
                  room {inspectedRoom}
                  {inspected?.mac_address
                    ? ` · lock ${inspected.mac_address}`
                    : ' · no lock bound'}{' '}
                  · {roomKeys.length} credential{roomKeys.length === 1 ? '' : 's'}
                </div>
                {roomKeys.length === 0 && (
                  <div style={{ color: C.dim }}>— no keys issued to this room yet —</div>
                )}
                {roomKeys.length > 0 && (
                  <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                    <thead>
                      <tr>
                        {['ID', 'TYPE', 'SLOT', 'USER', 'VALUE', 'VALID (FROM → TO)', 'STATUS', ''].map(
                          (h) => (
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
                          )
                        )}
                      </tr>
                    </thead>
                    <tbody>
                      {roomKeys.map((c) => (
                        <tr key={c.id}>
                          <td style={{ padding: '5px 8px 5px 0', color: C.dim }}>#{c.id}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: '#C084FC', fontWeight: 700 }}>
                            {String(c.type).toUpperCase()}
                          </td>
                          <td style={{ padding: '5px 8px 5px 0' }}>{c.slot_number}</td>
                          <td style={{ padding: '5px 8px 5px 0' }}>{c.user_name || '—'}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: C.termGreen }}>{c.raw_value}</td>
                          <td style={{ padding: '5px 8px 5px 0', color: C.dim }}>
                            {String(c.date_from || '').slice(0, 16)} → {String(c.date_to || '').slice(0, 16)}
                          </td>
                          <td
                            style={{
                              padding: '5px 8px 5px 0',
                              color: credStatusColor(c.sync_status),
                              fontWeight: 700,
                            }}
                          >
                            {String(c.sync_status).toUpperCase()}
                          </td>
                          <td style={{ padding: '5px 0' }}>
                            {['pending', 'synced'].includes(c.sync_status) && (
                              <button
                                onClick={() => doRevoke(c)}
                                disabled={revokeBusyId === c.id}
                                style={{
                                  cursor: revokeBusyId === c.id ? 'wait' : 'pointer',
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
                                {revokeBusyId === c.id ? '…' : 'REVOKE'}
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

          {/* -- Tab 3: door access transactions pushed by the lock -- */}
          {termTab === 'lock' &&
            (!inspectedRoom ? (
              <div style={{ color: C.dim }}>
                ozkey@lab:~$ click a room tile in the matrix to tail its doorlock log…
              </div>
            ) : (
              <>
                <div style={{ color: C.dim, marginBottom: 8 }}>
                  lock {inspected?.mac_address || '— unbound —'} · room {inspectedRoom} ·{' '}
                  {lockTransactions.length} door transaction
                  {lockTransactions.length === 1 ? '' : 's'} (latest 100)
                </div>
                {lockTransactions.length === 0 && (
                  <div style={{ color: C.dim }}>
                    — no door transactions recorded for this lock yet —
                  </div>
                )}
                {lockTransactions.map((t) => (
                  <div key={t.id} style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all' }}>
                    <span style={{ color: '#334155' }}>
                      {new Date(t.lock_ts || t.created_at).toLocaleTimeString('en-AU', {
                        hour12: false,
                      })}{' '}
                    </span>
                    <span style={{ color: accessResultColor(t.result), fontWeight: 700 }}>
                      [{String(t.result).toUpperCase().padEnd(7, ' ')}]
                    </span>{' '}
                    <span style={{ color: C.termGreen }}>{t.detail || '—'}</span>
                    <span style={{ color: C.dim }}> · {t.mac}</span>
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
          background: #0b1220;
        }
        ::-webkit-scrollbar-thumb {
          background: #334155;
          border-radius: 4px;
        }
        select option {
          background: ${C.panel};
        }
      `}</style>
    </div>
  );
}
