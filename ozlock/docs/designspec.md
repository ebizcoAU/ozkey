# OZLOCK Console — Fleet Health Dashboard, Design Spec

**Status:** 🟡 DRAFT — mockup phase (static/mock data). Real-data wiring is a
deferred follow-up, not scoped here.
**Owner:** server team. **Consumers:** admin/operator using the console.

---

## 1. Purpose & audience

`ozlock` (port 4300) is the operator-facing console over `ozlockserv`'s
registry (:4200). Today it answers two questions well — *"what does this app
have paired?"* and *"what does this doorlock belong to?"* — via a
click-through browser + inspector. It answers a third question poorly: **"is
the fleet OK right now, and if not, where's the problem?"** That question
currently requires reading a raw event firehose line by line.

This spec adds a dashboard that answers the third question in one glance,
without taking anything away from the first two. Primary user: whoever is on
call or doing daily fleet review — someone who needs to notice "MQTT traffic
dropped to zero" or "this app's lock has been unseen for 3 days" in seconds,
not by scrolling logs.

## 2. Information architecture

Two top-level tabs, switched in the header (new):

- **Overview** (new, default landing tab) — the health dashboard described
  below.
- **Registry** (existing) — today's entire app/lock browser + inspector +
  firehose, unchanged in behavior, moved behind this tab.

Nothing in `Registry` changes. This is additive.

## 3. Overview layout

Top to bottom, same panel/grid conventions as `Registry` (see §5):

### 3.1 Health strip

A row of stat tiles:

| Tile | Content |
|---|---|
| Database | up/down dot + label |
| MQTT broker | up/down dot + label |
| Uptime | formatted duration |
| Apps | total count |
| Doorlocks | total count |
| Fleet presence | 3-way split: online / asleep / never-seen, as a small stacked bar + counts |

Presence split is derived the same way `Registry`'s `presence()` helper
already classifies a lock — this tile is a fleet-wide rollup of the same
per-row logic, not a new classification.

### 3.2 Traffic charts row

Two line/sparkline charts, side by side:

- **HTTP requests** — requests/sec over the visible window.
- **MQTT messages** — in vs out, two overlaid series, msgs/sec.

### 3.3 MQTT packet breakdown

Horizontal bar chart, one bar per topic category: `heartbeat`,
`presence (locks)`, `presence (bridges)`, `enroll`, `uplink`,
`thread-liveness`, `member-relay`. Count + share-of-total per bar. Categories
match the branches already in `ozlockserv`'s MQTT message handler — this
chart is a rollup of real branches, not an invented taxonomy.

### 3.4 Meters row

Three arc/radial gauges:

- **Queue backlog** — depth of `pending_queue` (rows not yet `sent`).
- **Error rate** — share of recent event-log lines at `error`/`warn` level.
- **Fleet online %** — locks currently `online` ÷ total.

### 3.5 Quick-find

A promoted search box, visually similar to `Registry`'s existing search
input, living on the Overview tab itself:

- Typing an app id / device id / label jumps to the `Registry` tab with that
  entity already selected in the inspector (reuses the existing
  `selectApp`/`selectLock` functions — no new selection logic).
- A filter chip pair — **Offline only** / **Never seen** — narrows the
  quick-find results to exactly the locks an admin is usually hunting for
  when something's "lost."

This is the direct answer to "quickly monitor and search for a lost app/DL":
it's two clicks (type, click the match) instead of switching tabs and
scanning a list manually.

## 4. Visual language

No new palette, no new font, no new dependency. Reuse exactly:

- `C{}` color tokens (`ozlock/pages/index.js`) — teal/green/blue/red/amber/
  violet/gray/dim on the dark `bg`/`panel` pair.
- `panelStyle` (bordered rounded panel), `PanelTitle` (dot + uppercase label
  + optional right-aligned slot) as the section-header pattern.
- SF Mono / JetBrains Mono monospace stack, same as today.
- Charts are **inline SVG**, hand-rolled, styled with the same `C{}` tokens —
  no chart library, no canvas. Motion (if any) is CSS, not a dependency.

Goal: a screenshot of `Overview` and a screenshot of `Registry` should read
as the same product, not a themed-differently add-on.

## 5. Component inventory

New, in `ozlock/components/`:

| Component | Props (mockup phase) | Renders |
|---|---|---|
| `StatTile` | `{label, value, sub, dot}` | One health-strip tile |
| `Sparkline` | `{series: number[], color, height, width}` | Inline SVG line/area chart |
| `DualSparkline` | `{seriesA, seriesB, colorA, colorB, height, width, labelA, labelB}` | Overlaid two-series line chart (MQTT in/out) |
| `BarBreakdown` | `{rows: {label, value, color}[]}` | Horizontal bar list with count + % |
| `Gauge` | `{value, max, label, color, sublabel}` | Arc/radial meter, 0–max |
| `PresenceStack` | `{online, asleep, never}` | Small horizontal stacked bar + legend |

All plain function components, inline `style={}` objects, SVG via JSX
(`<svg><path/></svg>`) — matching the existing file's convention of zero
external UI dependencies.

## 6. Data contract (for the deferred wiring phase)

Not implemented this session. Recorded here so wiring later is a
fill-in-the-blanks job against a shape already agreed, not a redesign.

A future `GET /ozlockserv/api/metrics` would return:

```jsonc
{
  "ok": true,
  "uptime_s": 123456,
  "db": true,
  "mqtt": true,
  "totals": { "apps": 12, "locks": 34 },
  "presence": { "online": 20, "asleep": 10, "never": 4 },
  "queue_depth": 3,
  "error_rate": 0.02,          // share of recent eventRing at warn/error
  "series": {                  // fixed-size ring, e.g. 180 samples @ 10s
    "t": [ /* epoch ms, oldest→newest */ ],
    "http_rps": [ /* number per sample */ ],
    "mqtt_in_rps": [ /* number per sample */ ],
    "mqtt_out_rps": [ /* number per sample */ ]
  },
  "mqtt_breakdown": {
    "heartbeat": 0, "presence_locks": 0, "presence_bridges": 0,
    "enroll": 0, "uplink": 0, "thread_liveness": 0, "member_relay": 0
  }
}
```

Source mapping (from `ozlockserv/server.js` research, 2026-08-17):

- `presence`/`totals` — derivable client-side from existing `/locks`,
  `/apps` responses; no new query needed.
- `queue_depth` — `SELECT COUNT(*) FROM pending_queue WHERE status='queued'`
  (table already backs `GET /queue`).
- `error_rate` — derivable from `eventRing` (existing 500-entry ring buffer
  behind `logEvent()`/`GET /events`).
- `series.*`, `mqtt_breakdown.*` — **new instrumentation required**: counter
  hooks in the MQTT message handler's topic branches (one per category) and
  in `mqttPublish()` (outbound), plus a new fixed-size time-series ring
  (mirroring the `eventRing` pattern) sampled on an interval. Not built yet.

## 6.0 Addendum — horizontal-first layout (2026-08-17)

Original layout stacked every panel full-width, one per row — wasted the
plentiful horizontal space and pushed everything below the fold. Reworked
to 3-column rows, tightened panel padding/chart heights, and capped the
three list-shaped panels (Alerts, Needs Attention, Quick Find) to a fixed
scrollable height so they don't grow the row to fit their longest list:

1. **Fleet Health** — own full-width row (unchanged content, tighter padding).
2. **HTTP Requests | MQTT Traffic | Connection Quality** — one row.
3. **MQTT Packet Breakdown | Meters | Battery Health** — one row.
4. **Active Alerts | Needs Attention | Quick Find** — one row, each capped
   at a fixed height with internal scroll.

Four content rows total instead of seven, all reachable without scrolling
on a typical viewport.

## 6.1 Addendum — monitoring-engineer pass (2026-08-17)

First pass covered "is it up" and "is traffic flowing" but not the two
things a fleet engineer actually watches for: things trending toward
failure, and connection quality hiding behind a binary up/down. Added:

| Panel | Data | Why |
|---|---|---|
| **Active Alerts** | Mixed real (db/mqtt up, presence, MCU-link, battery) + mock (queue depth, error rate, reconnect rate) | Named threshold breaches instead of requiring someone to read gauges and notice one's in the red zone |
| **Needs Attention** | **Real** — ranks locks by MCU-link-down / never-seen / long-silent / low-battery, using fields `/locks` already returns (`mcu_link_up`, `battery_pct`, `last_seen_at`) | Click-to-inspect, zero typing — the proactive complement to Quick-Find's reactive "I have an id" search. Raised directly: typing/pasting an opaque device id is error-prone: this is the answer |
| **Battery Health Distribution** | **Real** — bucketed histogram of `battery_pct` across the fleet | These are sleepy battery-powered end devices; a bucket histogram catches a fleet-wide battery problem before locks start going dark, not after |
| **Connection Quality** | Mock — reconnect count (10 min) + in/out payload-bandwidth sparkline | Binary MQTT up/down hides flapping. Payload size specifically chosen because this codebase has a known history (`mqtt-256-byte-cliff`) of oversized payloads being silently dropped — worth a real chart once instrumented, not an arbitrary metric |

`Needs Attention` and `Battery Health Distribution` use **real** `locks`
data already fetched by the existing poll loop — no mock, no deferred
wiring needed for those two. Alerts and Connection Quality remain
partially/fully mock per §6/§7 until the backend instrumentation lands;
each mock number is labeled inline in the UI.

## 7. Non-goals (this session)

- No changes to `ozlockserv/server.js` or any backend.
- No new npm dependency (no chart library).
- No real data wiring — `Overview`'s numbers are from a mock generator,
  clearly marked as such in code, swapped out in a later session.
- No change to `Registry` tab behavior.
