# ozkey-54 — Fleet map, install geolocation, bridge coverage, and three languages

> Status: **DRAFT, 2026-10-19, firmware (team lead).** Operator request of 2026-10-19, which came
> with his acceptance of the control-dashboard mockup
> (https://claude.ai/artifact/Y9v6FQpajwYZQdRCcPgru4). Mockup v2, with the OZPMS fleet map and the
> Settings language switcher: https://claude.ai/artifact/Hds4AXWYtycfXB9hLLy48z
> Consumers: server (ozlock / ozlodge / ozpms + `ozserver-core`), ftpos (BANOI / AURA apps), Nexus
> backend (map stack, nexus-08), firmware (bridgeBLE, AK1037).
> Inputs:
> - AK1037 Processor Specification V0.3.0 (BLE section) and AK1037 datasheet V1.0.1;
> - the ESP32-C6 Arduino core 3.3.11 sdkconfig (our bridge build);
> - ozkey-48/50/51.

## 0. What the operator asked for

1. **A fleet map**, in OZPMS and on the OZLOCK side, for **1K–100K locks**.
2. **Locations captured by the AURA/BANOI app** when it pairs locks through a bridge.
3. **Coverage:** one bridge serves several locks over BLE. The operator's working figures are
   10–30 m, 2–3 floors (6–9 m) in one hop, and 12–32 locks per bridge. Check them against the chip specs.
4. **The map served from our own OpenStreetMap / OSRM stack**, the one that runs the Ride business.
5. **All products in EN, VI and ZH**, with a language switcher in Settings. Later: SE Asian,
   Middle Eastern and EU languages.

## 1. Radio reality: what one bridge can cover

### 1.1 The two radios (from the vendor documents)

| | AK1037 lock | ESP32-C6 bridge |
|---|---|---|
| BLE version | v6.1 (datasheet) | 5.x, NimBLE |
| PHY | **1M and 2M only. No Coded PHY, so no BLE long-range mode** (Spec V0.3.0 §BLE) | 1M, 2M, Coded |
| TX power | **−20 to +4 dBm**, 4 dB steps | build default +9 dBm (`CONFIG_BT_LE_DFT_TX_POWER_LEVEL_P9`); higher settable 🟡 confirm the max in the C6 datasheet |
| RX sensitivity | **−90 dBm** | 🟡 to confirm in the C6 datasheet (≈ −97 dBm at 1M is typical for the class) |
| Roles | master and slave | central and peripheral |
| Simultaneous links | — | **10** in our build (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=10`) |
| Advertising current | **31 µA**, but measured at **−20 dBm**, 1 s interval, 23-byte payload | mains-powered |

### 1.2 Link budget

The weaker direction is **lock → bridge**: +4 dBm out, about −97 dBm in, so ~101 dB. With the bridge
at its higher TX setting, the other direction is better, so the lock's +4 dBm sets the range.
Keeping a 15 dB fade margin, which a door that must always answer needs, leaves **~85 dB of usable
path loss**.

| Path at 2.4 GHz | Typical loss | Fits in 85 dB? |
|---|---|---|
| 10 m free space | ~60 dB | yes |
| 30 m free space | ~70 dB | yes |
| + 2–3 interior walls (plasterboard / brick) | +5 to +20 dB | yes at 10–20 m, marginal at 30 m |
| + 1 reinforced-concrete floor slab | +15 to +30 dB | **marginal**, depends on the building |
| + 2–3 concrete slabs | +40 to +80 dB | **no** |
| + 2–3 timber / light floors | +10 to +25 dB | often yes |

**Verdict on the 10–30 m / 2–3 floors figure:**
- **Same floor, 10–30 m through a few walls:** realistic.
- **2–3 floors in one hop:** realistic only in light construction (timber, plasterboard, older
  houses). In concrete buildings (hotels, apartment blocks, mining-camp modules on slabs), plan
  **one bridge per floor**, or per two floors where a site survey shows enough signal.
- The AK1037 has no Coded PHY, so the C6's long-range mode can't make up the difference.

### 1.3 Not BLE Mesh: a star with on-demand links

Our design is not Bluetooth Mesh. It is a **star**: each bridge is a BLE central that connects to a
lock when there is something to deliver, then disconnects (bridgeBLE today, ozkey-48 relay).
Locks don't relay for each other. What this means:
- The **10-connection limit doesn't cap locks per bridge.** Links are short-lived and sequential.
- What does cap them:
  - **airtime:** a relayed unlock is a connection plus the EDHOC or OSCORE exchange, ~1 s;
  - **lock reachability:** the lock has to be advertising or listening when the bridge wants it;
  - **range** (§1.2).
- **12–32 locks per bridge is a sound planning figure for hotel corridors and apartment floors.**
  The default plan is **≤ 16 locks per bridge**, raised to 32 where the site survey (§1.5) shows
  every lock at a good signal.
- **Failover is already built** (multi-bridge, XF-144): give each lock **two bridges in range**
  where possible, so one bridge failing doesn't cut a corridor off.

### 1.4 🔴 An open firmware question this raises: how the bridge reaches a sleeping lock

v1's contract has an enrolled lock advertise only for ~60 s after a keypad touch
(CONTRACT.md, for battery life and to avoid a trackable beacon). That works for phones at the
door. **It does not work for a bridge** that must reach the lock for remote unlock, credential
writes and the vacate confirmation in the dashboard.
- AK1037 options: advertise periodically with a rotating private address (RPA), or listen on a
  schedule the bridge knows.
- **The battery cost is not yet known at +4 dBm.** The datasheet's 31 µA is at −20 dBm, roughly
  a 3 m range. To be measured in ozkey-49 P6.
- Firmware owns this. It is filed here because the coverage numbers above assume the lock can be
  reached.

### 1.5 Siting by measurement, not by formula

- Bridges already scan. They will report **RSSI per lock** in the presence payload (`rssi` next
  to `ozkie_version`, `common/ozpresence.h`).
- The install flow in the app shows each lock's signal at the bridge, with a planning floor of
  **−80 dBm**, and warns below it.
- The map can colour each lock by its best bridge's RSSI, so weak spots show before they
  become complaints.

## 2. Install geolocation, captured by the app

### 2.1 What the app sends

The app sends a location when it pairs a bridge and when it pairs each lock: the claim in
ozkey-48 §3.4, member-free installs, and ozpms installer claims in ozkey-51 §6.

```
location: { lat, lng, accuracy_m, alt_m?, source: "gps"|"network"|"manual",
            captured_at, floor_label?, unit_label? }
```

- **GPS indoors is coarse**, 10–50 m, and can't tell floors apart. The app asks the installer for
  floor and unit, and lets them **drag the pin** on the map. The server keeps both the measured
  value and the adjusted one.
- **No location for a lock:** the lock takes its bridge's location, marked `inherited`.
- The location never goes to the lock. It is server data, carried in the existing pairing HTTP
  calls. **OZKIE and the firmware are unchanged.**

### 2.2 Privacy, by product (🟡 operator decision, Q1)

| | What is stored | Who sees it |
|---|---|---|
| **OZPMS** | exact location, floor, unit | company staff, within their region/manager scope. These are business assets |
| **OZLODGE** | exact location of the property and bridges; rooms by floor and unit | hotel staff only. The data stays on the hotel's server |
| **OZLOCK (residential)** | **only if the owner opts in**, and exact only for the owner | the platform ops map shows **coarse density** only (geohash 5, ≈ 5 km cells). Never a pin on a home |

**Why the OZLOCK row:** a list of where front doors are, held by a platform, is exactly what
OZLOCK's privacy promise ("no usage logs", ozkey-50 §3) is meant to rule out. A home's location
is personal data under AU and VN privacy law. The density view still answers the ops question,
"where are bridges failing?", without it.

## 3. The map

### 3.1 Where it appears

- **OZPMS:** a portfolio map. It shows houses, coloured by tenancy state and lock health,
  clustered at low zoom, and filters by region and manager. This is the 1K–100K case: one
  company, ~10,000 houses (ozkey-51).
- **OZLOCK ops:** bridge and lock **health density** across the whole platform (coarse, §2.2),
  plus offline bridges as pins (bridges aren't homes).
- **OZLODGE:** a single property. The useful view is a **floor plan** (floors, rooms, which bridge
  covers which corridor), with the street map only as a locator.

### 3.2 Serving 100K points

- **Client:** MapLibre GL JS (BSD licence), drawing vector or raster tiles from our own OSM stack.
- **Server clustering:** `GET /map/locks?bbox=…&zoom=…&filter=…` returns clusters at low zoom and
  points at high zoom. Uses supercluster or equivalent, built once and updated on change. The
  client never downloads 100K rows.
- **Live status:** a small delta feed (lock id → state) colours the visible points. The map
  doesn't reload.
- **Attribution:** "© OpenStreetMap contributors" on every map, as the ODbL requires.

### 3.3 Our OSM stack: what exists (nexus-e8, 2026-10-19, read from Nexus code and docs)

- **No tile server runs anywhere.**
  - Ride draws raster PNG tiles.
  - The planned XF-312 tile proxy (nginx cache in front of Geoapify osm-carto, signed 6 h passes)
    is **not configured** in lab or prod.
  - The lab and VN fall back to public `tile.openstreetmap.org`. AU prod has tiles off.
- **OSRM:** routing only, VN / ACT / AU extracts, on MacBook2 (currently down). Not needed for a
  fleet map.
- **Nominatim:** a Canberra extract only so far.
- **Reverse geocoding** goes through the Nexus API (Bearer auth): `lang=en|vi`, no `zh` yet
  (Nexus says it's a one-line change), with points snapped to an ~11 m grid.
- Nothing is exposed to other apps: no CORS for ozkey origins, no service keys.
- **Rules that bind us:**
  - public OSM tiles and Nominatim are **not allowed for production traffic** (OSM usage policy);
  - Nexus's ruling: **VN tiles must stay in-country**, with no silent foreign fallback.
- **Cost scales with map views, not lock count.** 100K locks is a clustering problem, solved by
  our own API (§3.2) on top of any basemap.

**Recommendation (🟡 Q5):**
- **Self-host a vector basemap per region:** Planetiler → OpenMapTiles mbtiles (AU ~2–4 GB,
  VN ~1 GB), served by martin or tileserver-gl behind an nginx cache, on one 2–4 vCPU VPS per
  region, rebuilt monthly. The cost is flat, data stays in the region, and it meets the VN
  in-country rule.
- **The same server can serve Ride.** Build it once, as a shared Nexus/ozkey service, rather
  than two.
- **Self-hosted (OZLODGE, OZPMS on customer VPS) and offline sites:** the dashboard must work
  without the basemap, showing points plus a floor plan, because doors work without internet
  (ozkey-50 §3).
- **Reverse geocoding:** our own Nominatim per region later. For now, the Nexus endpoint, with
  `zh` enabled.

## 4. Languages: EN, VI, ZH now; more later

- **Codes:** `en`, `vi` (Vietnamese; the operator wrote "VN", which is the country code),
  `zh-Hans` (Simplified Chinese; 🟡 Q2: is Traditional `zh-Hant` needed for HK, TW or
  Singapore?).
- **Every UI string comes from message catalogues** (ICU MessageFormat, for plurals and gender),
  never hard-coded. Webapps use one shared catalogue per product, kept in the repo and
  machine-checked so no key is missing in any language.
- **Switcher:** in Settings, stored per user on the server. The default comes from the browser.
  Dates, numbers, currency and time zones are formatted by the locale (CLDR via `Intl`).
- **Ready for the next languages today, cheaply:**
  - **SE Asia** (th, id, ms, km, lo, my, fil):
    - fonts with full Thai, Khmer, Lao and Burmese shaping (Noto family);
    - no truncated labels, since Thai and Burmese run long.
  - **Middle East** (ar, fa, he, which are **right-to-left**): build all layouts with CSS logical
    properties (`margin-inline-start`, not `margin-left`) and `dir` support **now**. Retrofitting
    RTL later is a rewrite.
  - **EU** (de, fr, es, it, pl, …): German strings run ~30% longer, so layouts must wrap.
- **Fonts:** the mockup's IBM Plex Sans covers Vietnamese but not Chinese. Pair it with
  **Noto Sans SC** for `zh-Hans`, and add more Noto families as languages arrive.
- **Map labels:** OSM `name:en` / `name:vi` / `name:zh`, falling back to the local name.
- **Firmware:**
  - the lock's voice prompts (AK1037 audio DAC, MP3/WAV) and any display text ship in EN, VI and
    ZH, selected per lock (`lock/settings`, ozkey-48 §5);
  - result codes stay language-neutral integers (ozkey-48 App. A); apps and dashboards
    translate them.
- **Apps (ftpos):** BANOI and AURA follow the same three languages. That belongs in the ftpos XF;
  it isn't specified here.

## 5. Who does what

| Team | Work |
|---|---|
| **server** (ozkey-94) | location columns and history (measured vs adjusted) for locks and bridges; the per-product privacy rules of §2.2; the clustering endpoint + delta feed; i18n framework + catalogues in all three webapps, **with the S1/S2 webapp rebuild rather than after it**; RSSI from presence on the map |
| **ftpos** (via PM) | location capture in the pairing flows, floor/unit entry, pin drag; signal display at install; app languages. Needs an XF |
| **Nexus backend** (nexus-08) | tile server + reverse geocoding for our use; hosting, limits, cost |
| **firmware** (me) | `rssi` per lock in bridge presence; how a bridge reaches a sleeping AK1037 (§1.4) and its battery cost (P6); voice prompts in 3 languages; bridge TX power setting |

## 6. Questions for the operator

1. **Residential privacy (§2.2):** OZLOCK stores a home's location only with the owner's opt-in,
   and platform ops see coarse density only? Recommended.
2. **Chinese:** Simplified only, or Traditional too?
3. **Planning numbers (§1.2–1.3):** default ≤ 16 locks and one concrete floor per bridge, raised
   up to 32 locks by site survey? Recommended over planning on 2–3 floors as the norm.
4. **OZLODGE:** is a floor-plan view (rooms and corridors) acceptable in place of a street map
   for a single hotel? Recommended.
5. **Basemap (§3.3):** a self-hosted vector tile server per region (AU, VN), shared with Ride, on
   one small VPS each? Recommended. The alternative, a hosted provider, is billed per map view,
   and VN tiles must stay in-country.
