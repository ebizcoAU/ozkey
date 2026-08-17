import { useEffect, useMemo, useState } from 'react';
import { C, panelStyle, inputStyle, agoLabel, shortId, presence, presenceSummary } from '../lib/theme';
import { mockMetrics, fmtUptime, fmtBytes } from '../lib/mockMetrics';
import PanelTitle from './PanelTitle';
import StatTile from './StatTile';
import Sparkline from './Sparkline';
import DualSparkline from './DualSparkline';
import BarBreakdown from './BarBreakdown';
import Gauge from './Gauge';
import PresenceStack from './PresenceStack';
import AlertsPanel, { buildAlerts } from './AlertsPanel';
import AttentionTable from './AttentionTable';

const BATTERY_BUCKETS = [
  { label: '0–20%', min: 0, max: 20, color: C.red },
  { label: '21–40%', min: 21, max: 40, color: C.amber },
  { label: '41–60%', min: 41, max: 60, color: C.amber },
  { label: '61–80%', min: 61, max: 80, color: C.green },
  { label: '81–100%', min: 81, max: 100, color: C.green },
];

const MQTT_CATEGORY_COLOR = {
  heartbeat: C.teal,
  presence_locks: C.green,
  presence_bridges: C.blue,
  enroll: C.violet,
  uplink: C.amber,
  thread_liveness: C.termGreen,
  member_relay: C.gray,
};

const MQTT_CATEGORY_LABEL = {
  heartbeat: 'heartbeat',
  presence_locks: 'presence · locks',
  presence_bridges: 'presence · bridges',
  enroll: 'enroll',
  uplink: 'uplink',
  thread_liveness: 'thread liveness',
  member_relay: 'member relay',
};

const grid3 = { display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 10, alignItems: 'stretch' };
const tightPanel = { ...panelStyle, padding: 10 };
const scrollBox = (h) => ({ maxHeight: h, overflowY: 'auto' });

export default function OverviewTab({ apps, locks, gatewayUp, mqttUp, onJumpToApp, onJumpToLock }) {
  // MOCK polling — a local tick drives lib/mockMetrics's deterministic
  // generator so the charts feel live without any network call. Swap this
  // effect for a GET /ozlockserv/api/metrics poll in the wiring phase.
  const [tick, setTick] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setTick((v) => v + 1), 2500);
    return () => clearInterval(t);
  }, []);
  const metrics = useMemo(() => mockMetrics(tick), [tick]);

  const fleet = useMemo(() => presenceSummary(locks), [locks]);
  const fleetTotal = fleet.online + fleet.asleep + fleet.never || 1;

  // Real signals (not mock): both fields already come back on every /locks row.
  const mcuDownCount = useMemo(
    () => (locks || []).filter((l) => l.mcu_link_up === false || l.mcu_link_up === 0).length,
    [locks]
  );
  const lowBatteryCount = useMemo(
    () => (locks || []).filter((l) => typeof l.battery_pct === 'number' && l.battery_pct < 20).length,
    [locks]
  );
  const batteryRows = useMemo(() => {
    const counts = BATTERY_BUCKETS.map(() => 0);
    let na = 0;
    for (const l of locks || []) {
      if (typeof l.battery_pct !== 'number') { na += 1; continue; }
      const idx = BATTERY_BUCKETS.findIndex((b) => l.battery_pct >= b.min && l.battery_pct <= b.max);
      if (idx >= 0) counts[idx] += 1;
    }
    const rows = BATTERY_BUCKETS.map((b, i) => ({ label: b.label, value: counts[i], color: b.color }));
    if (na > 0) rows.push({ label: 'n/a (mains/bridge)', value: na, color: C.gray });
    return rows;
  }, [locks]);

  const alerts = useMemo(
    () =>
      buildAlerts({
        gatewayUp,
        mqttUp,
        fleet,
        mcuDownCount,
        lowBatteryCount,
        metrics,
        reconnects10m: metrics.reconnects_10m,
      }),
    [gatewayUp, mqttUp, fleet, mcuDownCount, lowBatteryCount, metrics]
  );
  const alertDot = alerts.some((a) => a.sev === 'critical') ? C.red : alerts.length ? C.amber : C.green;

  const breakdownRows = Object.entries(metrics.mqtt_breakdown).map(([key, value]) => ({
    label: MQTT_CATEGORY_LABEL[key] || key,
    value,
    color: MQTT_CATEGORY_COLOR[key] || C.dim,
  }));

  /* ---- Quick-find ------------------------------------------------------ */
  const [q, setQ] = useState('');
  const [offlineOnly, setOfflineOnly] = useState(false);
  const [neverSeenOnly, setNeverSeenOnly] = useState(false);

  const term = q.trim().toLowerCase();
  const lockMatches = useMemo(() => {
    let rows = locks || [];
    if (neverSeenOnly) rows = rows.filter((l) => !l.last_seen_at);
    else if (offlineOnly) rows = rows.filter((l) => presence(l).col !== C.green);
    if (term) {
      rows = rows.filter(
        (l) =>
          String(l.id).toLowerCase().includes(term) ||
          String(l.app_id).toLowerCase().includes(term) ||
          String(l.label).toLowerCase().includes(term)
      );
    }
    return rows.slice(0, 12);
  }, [locks, term, offlineOnly, neverSeenOnly]);

  const appMatches = useMemo(() => {
    if (!term || offlineOnly || neverSeenOnly) return [];
    return (apps || []).filter((a) => String(a.app_id).toLowerCase().includes(term)).slice(0, 8);
  }, [apps, term, offlineOnly, neverSeenOnly]);

  const showingFiltered = offlineOnly || neverSeenOnly || term;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
      {/* ---- Row 1: Fleet health strip ------------------------------------ */}
      <div style={tightPanel}>
        <PanelTitle dot={gatewayUp && mqttUp ? C.green : C.amber}>Fleet Health</PanelTitle>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 8, alignItems: 'stretch' }}>
          <StatTile label="Database" value={gatewayUp ? 'UP' : '—'} dot={gatewayUp ? C.green : C.red} accent={gatewayUp ? C.green : C.red} />
          <StatTile label="MQTT Broker" value={mqttUp ? 'UP' : 'DOWN'} dot={mqttUp ? C.green : C.red} accent={mqttUp ? C.green : C.red} />
          <StatTile label="Uptime" value={fmtUptime(metrics.uptime_s)} />
          <StatTile label="Apps" value={(apps || []).length} accent={C.violet} />
          <StatTile label="Doorlocks" value={(locks || []).length} accent={C.teal} />
          <div style={{ flex: '2 1 220px', minWidth: 200 }}>
            <div style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1, textTransform: 'uppercase', color: C.dim, marginBottom: 4 }}>
              Fleet presence · {fleetTotal}
            </div>
            <PresenceStack online={fleet.online} asleep={fleet.asleep} never={fleet.never} />
          </div>
        </div>
      </div>

      {/* ---- Row 2: HTTP requests | MQTT traffic | Connection quality ----- */}
      <div style={grid3}>
        <div style={tightPanel}>
          <PanelTitle dot={C.blue} right={<span style={{ fontSize: 9, color: C.dim, textTransform: 'none' }}>req/s · mock</span>}>
            HTTP Requests
          </PanelTitle>
          <Sparkline series={metrics.series.http_rps} color={C.blue} height={140} />
        </div>
        <div style={tightPanel}>
          <PanelTitle dot={C.teal} right={<span style={{ fontSize: 9, color: C.dim, textTransform: 'none' }}>msgs/s · mock</span>}>
            MQTT Traffic
          </PanelTitle>
          <DualSparkline
            seriesA={metrics.series.mqtt_in_rps}
            seriesB={metrics.series.mqtt_out_rps}
            colorA={C.teal}
            colorB={C.amber}
            labelA="in"
            labelB="out"
            height={140}
          />
        </div>
        <div style={tightPanel}>
          <PanelTitle dot={C.violet} right={<span style={{ fontSize: 9, color: C.dim, textTransform: 'none' }}>mock</span>}>
            Connection Quality
          </PanelTitle>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 6, marginBottom: 4 }}>
            <span style={{ fontSize: 18, fontWeight: 800, color: metrics.reconnects_10m > 3 ? C.amber : C.text }}>
              {metrics.reconnects_10m}
            </span>
            <span style={{ fontSize: 9, color: C.dim, textTransform: 'uppercase' }}>reconnects / 10m</span>
          </div>
          <div style={{ fontSize: 9, color: C.dim, marginBottom: 3 }}>
            {fmtBytes(metrics.series.bytes_in_per_s.at(-1))} in / {fmtBytes(metrics.series.bytes_out_per_s.at(-1))} out
          </div>
          <DualSparkline
            seriesA={metrics.series.bytes_in_per_s}
            seriesB={metrics.series.bytes_out_per_s}
            colorA={C.blue}
            colorB={C.violet}
            labelA="in"
            labelB="out"
            height={128}
          />
        </div>
      </div>

      {/* ---- Row 3: MQTT breakdown | Meters | Battery health -------------- */}
      <div style={grid3}>
        <div style={tightPanel}>
          <PanelTitle dot={C.termGreen}>MQTT Packet Breakdown</PanelTitle>
          <BarBreakdown rows={breakdownRows} />
        </div>
        <div style={tightPanel}>
          <PanelTitle dot={C.amber}>Meters</PanelTitle>
          <div style={{ display: 'flex', justifyContent: 'space-around', flexWrap: 'wrap', gap: 6 }}>
            <Gauge
              pct={Math.min(1, metrics.queue_depth / 20)}
              centerText={metrics.queue_depth}
              label="Queue"
              sublabel="pending"
              color={metrics.queue_depth > 12 ? C.amber : C.teal}
              size={153}
            />
            <Gauge
              pct={Math.min(1, metrics.error_rate / 0.15)}
              centerText={`${Math.round(metrics.error_rate * 100)}%`}
              label="Errors"
              color={metrics.error_rate > 0.08 ? C.red : C.green}
              size={153}
            />
            <Gauge
              pct={fleet.online / fleetTotal}
              centerText={`${Math.round((fleet.online / fleetTotal) * 100)}%`}
              label="Online"
              color={C.blue}
              size={153}
            />
          </div>
        </div>
        <div style={tightPanel}>
          <PanelTitle dot={C.green} right={<span style={{ fontSize: 9, color: C.dim, textTransform: 'none' }}>real</span>}>
            Battery Health
          </PanelTitle>
          <BarBreakdown rows={batteryRows} />
        </div>
      </div>

      {/* ---- Row 4: Active alerts | Needs attention | Quick find ---------- */}
      <div style={grid3}>
        <div style={tightPanel}>
          <PanelTitle dot={alertDot}>Active Alerts {alerts.length > 0 && `(${alerts.length})`}</PanelTitle>
          <div style={scrollBox(230)}>
            <AlertsPanel alerts={alerts} />
          </div>
        </div>

        <div style={tightPanel}>
          <PanelTitle dot={C.red} right={<span style={{ fontSize: 9, color: C.dim, textTransform: 'none' }}>real</span>}>
            Needs Attention
          </PanelTitle>
          <div style={scrollBox(230)}>
            <AttentionTable locks={locks} onJumpToLock={onJumpToLock} limit={6} />
          </div>
        </div>

        <div style={tightPanel}>
          <PanelTitle dot={C.dim}>Quick Find</PanelTitle>
          <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', alignItems: 'center', marginBottom: 6 }}>
            <input
              style={{ ...inputStyle, flex: '1 1 140px', padding: '5px 8px' }}
              placeholder="id / label…"
              value={q}
              onChange={(e) => setQ(e.target.value)}
            />
            {[
              ['Offline', offlineOnly, () => { setOfflineOnly((v) => !v); setNeverSeenOnly(false); }],
              ['Never seen', neverSeenOnly, () => { setNeverSeenOnly((v) => !v); setOfflineOnly(false); }],
            ].map(([label, on, toggle]) => (
              <button
                key={label}
                onClick={toggle}
                style={{
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                  fontSize: 9,
                  fontWeight: 700,
                  letterSpacing: 0.5,
                  padding: '5px 8px',
                  borderRadius: 5,
                  border: `1px solid ${on ? C.amber : C.panelEdge}`,
                  background: on ? '#2A1E0B' : 'transparent',
                  color: on ? C.amber : C.dim,
                  whiteSpace: 'nowrap',
                }}
              >
                {label}
              </button>
            ))}
          </div>

          {!showingFiltered && (
            <div style={{ color: C.dim, fontSize: 10 }}>Type an id/label, or use a chip.</div>
          )}

          {showingFiltered && (
            <div style={{ ...scrollBox(190), display: 'flex', flexDirection: 'column', gap: 5 }}>
              {appMatches.map((a) => (
                <div
                  key={a.app_id}
                  onClick={() => onJumpToApp(a.app_id)}
                  style={{ cursor: 'pointer', border: `1px solid ${C.panelEdge}`, borderRadius: 6, padding: '5px 8px', display: 'flex', alignItems: 'center', gap: 6, background: C.bg }}
                >
                  <span style={{ fontSize: 8, fontWeight: 700, color: C.violet, textTransform: 'uppercase' }}>App</span>
                  <span style={{ fontSize: 11, color: C.violet, fontWeight: 700, wordBreak: 'break-all' }}>{shortId(a.app_id, 16)}</span>
                  <span style={{ marginLeft: 'auto', fontSize: 9, color: C.dim, whiteSpace: 'nowrap' }}>{agoLabel(a.last_seen_at)}</span>
                </div>
              ))}
              {lockMatches.map((l) => {
                const p = presence(l);
                return (
                  <div
                    key={l.id}
                    onClick={() => onJumpToLock(l.id)}
                    style={{ cursor: 'pointer', border: `1px solid ${C.panelEdge}`, borderRadius: 6, padding: '5px 8px', display: 'flex', alignItems: 'center', gap: 6, background: C.bg }}
                  >
                    <span style={{ width: 6, height: 6, borderRadius: '50%', background: p.col, flexShrink: 0 }} />
                    <span style={{ fontSize: 11, fontWeight: 700 }}>{l.label}</span>
                    <span style={{ marginLeft: 'auto', fontSize: 9, color: C.dim, whiteSpace: 'nowrap' }}>{p.label}</span>
                  </div>
                );
              })}
              {appMatches.length === 0 && lockMatches.length === 0 && (
                <div style={{ color: C.dim, fontSize: 11 }}>— no matches —</div>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
