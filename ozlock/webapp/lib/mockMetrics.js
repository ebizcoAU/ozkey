/*
 * MOCK — deterministic fake data for the Overview dashboard's mockup phase.
 * Replace with real polling of GET /ozlockserv/api/metrics once the backend
 * instrumentation described in docs/designspec.md §6 exists. Nothing in this
 * file talks to the network.
 */

const WINDOW = 40; // samples shown per chart — one "screen" of history

// Deterministic 0..1 pseudo-random from an integer seed. Not Math.random()
// on purpose: the same tick always produces the same frame, so a re-render
// mid-poll-interval never jitters the chart.
function pseudoRandom(seed) {
  const x = Math.sin(seed * 12.9898) * 43758.5453;
  return x - Math.floor(x);
}

function wave(i, base, amp, period, phase = 0, noise = 0.15) {
  const v = base + amp * Math.sin((i / period) * Math.PI * 2 + phase);
  const jitter = (pseudoRandom(i + phase * 100) - 0.5) * 2 * amp * noise;
  return Math.max(0, v + jitter);
}

/** `tick` should increase by 1 each poll — the window "scrolls" as it grows. */
export function mockMetrics(tick = 0) {
  const httpRps = Array.from({ length: WINDOW }, (_, i) => wave(tick + i, 12, 6, 9, 0));
  const mqttInRps = Array.from({ length: WINDOW }, (_, i) => wave(tick + i, 30, 14, 13, 1));
  const mqttOutRps = Array.from({ length: WINDOW }, (_, i) => wave(tick + i, 18, 9, 11, 2));

  const queueDepth = Math.max(0, Math.round(wave(tick, 4, 4, 20, 3, 0.4)));
  const errorRate = Math.min(0.15, Math.max(0, wave(tick, 0.02, 0.02, 25, 4, 0.6)));

  // Reconnect churn + payload size: still mock (no counters exist server
  // side yet), but modeled deliberately — the codebase has a known history
  // of a 256-byte MQTT payload cliff silently dropping oversized messages,
  // so payload-size is a real thing worth a chart, not an invented metric.
  const reconnects10m = Math.round(wave(tick, 0.6, 2.2, 40, 12, 0.5));
  const bytesInPerS = Array.from({ length: WINDOW }, (_, i) => wave(tick + i, 1400, 700, 15, 13));
  const bytesOutPerS = Array.from({ length: WINDOW }, (_, i) => wave(tick + i, 900, 500, 12, 14));

  const mqttBreakdown = {
    heartbeat: Math.round(4200 + wave(tick, 0, 300, 30, 5)),
    presence_locks: Math.round(1800 + wave(tick, 0, 150, 30, 6)),
    presence_bridges: Math.round(410 + wave(tick, 0, 40, 30, 7)),
    enroll: Math.round(12 + wave(tick, 0, 6, 30, 8)),
    uplink: Math.round(260 + wave(tick, 0, 30, 30, 9)),
    thread_liveness: Math.round(980 + wave(tick, 0, 80, 30, 10)),
    member_relay: Math.round(35 + wave(tick, 0, 10, 30, 11)),
  };

  return {
    uptime_s: 3600 * 41 + tick * 2,
    queue_depth: queueDepth,
    error_rate: errorRate,
    reconnects_10m: Math.max(0, reconnects10m),
    series: {
      http_rps: httpRps,
      mqtt_in_rps: mqttInRps,
      mqtt_out_rps: mqttOutRps,
      bytes_in_per_s: bytesInPerS,
      bytes_out_per_s: bytesOutPerS,
    },
    mqtt_breakdown: mqttBreakdown,
  };
}

export function fmtBytes(n) {
  if (n < 1024) return `${Math.round(n)} B/s`;
  return `${(n / 1024).toFixed(1)} KB/s`;
}

export function fmtUptime(s) {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}
