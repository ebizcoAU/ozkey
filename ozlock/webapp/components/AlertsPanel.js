import { C } from '../lib/theme';

const SEV_COLOR = { critical: C.red, warn: C.amber };

/* Surfaces threshold breaches as named alerts instead of making an admin
 * read gauges. Real signals (db/mqtt up, presence, battery, MCU link) come
 * from actual /locks data; queue/error-rate/reconnect signals are still
 * mock (`docs/designspec.md` §6 — no backend counters exist yet), each
 * alert below says which it is. */
export function buildAlerts({ gatewayUp, mqttUp, fleet, mcuDownCount, lowBatteryCount, metrics, reconnects10m }) {
  const alerts = [];

  if (!gatewayUp) alerts.push({ sev: 'critical', text: 'Database unreachable', src: 'real' });
  if (!mqttUp) alerts.push({ sev: 'critical', text: 'MQTT broker disconnected', src: 'real' });
  if (mcuDownCount > 0) alerts.push({ sev: 'critical', text: `${mcuDownCount} doorlock(s) report MCU link down`, src: 'real' });
  if (fleet.never > 0) alerts.push({ sev: 'warn', text: `${fleet.never} doorlock(s) never seen`, src: 'real' });
  if (lowBatteryCount > 0) alerts.push({ sev: 'warn', text: `${lowBatteryCount} doorlock(s) below 20% battery`, src: 'real' });
  if (metrics.queue_depth > 12) alerts.push({ sev: 'warn', text: `Queue backlog elevated (${metrics.queue_depth} pending)`, src: 'mock' });
  if (metrics.error_rate > 0.08) alerts.push({ sev: 'critical', text: `Error rate elevated (${Math.round(metrics.error_rate * 100)}%)`, src: 'mock' });
  if (reconnects10m > 3) alerts.push({ sev: 'warn', text: `MQTT reconnecting frequently (${reconnects10m}/10min — flapping)`, src: 'mock' });

  return alerts.sort((a, b) => (a.sev === 'critical' ? -1 : 1) - (b.sev === 'critical' ? -1 : 1));
}

export default function AlertsPanel({ alerts = [] }) {
  if (alerts.length === 0) {
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, color: C.green, fontSize: 12, fontWeight: 700 }}>
        <span style={{ width: 7, height: 7, borderRadius: '50%', background: C.green, boxShadow: `0 0 6px ${C.green}` }} />
        All clear — no active alerts
      </div>
    );
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
      {alerts.map((a, i) => (
        <div
          key={i}
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: 8,
            fontSize: 12,
            padding: '6px 10px',
            borderRadius: 6,
            border: `1px solid ${SEV_COLOR[a.sev]}`,
            background: a.sev === 'critical' ? '#2A0E0E' : '#2A1E0B',
          }}
        >
          <span style={{ width: 7, height: 7, borderRadius: '50%', background: SEV_COLOR[a.sev], flexShrink: 0 }} />
          <span style={{ color: C.text, fontWeight: 600 }}>{a.text}</span>
          {a.src === 'mock' && (
            <span style={{ marginLeft: 'auto', fontSize: 8, color: C.dim, textTransform: 'uppercase', letterSpacing: 0.5 }}>
              mock
            </span>
          )}
        </div>
      ))}
    </div>
  );
}
