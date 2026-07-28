# OZKEY-11 — OZLOCK-HOME Remote-Unlock Access Model + Hosted-Broker Auth

> **APP→LOCK REMOTE UNLOCK WORKING 2026-07-29 + MATTER MOVED TO THE BRIDGE
> (operator decision) + BOM SETTLED: N8 FOR THE LOCK.**
> Supersedes the mode table in **ozkey-10 §0** — Mode 1 (Matter) is no longer a
> lock firmware mode at all. Read this note and the 2026-07-27 one below it.
>
> ## 1. The full chain works, driven by the app
>
> `BANOI → ozlockserv → MQTT → bridge32 → Thread multicast → doorlock → MCU`,
> confirmed on real hardware with a server-generated `msg_id` (i.e. a genuine
> app-originated command, not a hand-made test publish), ending in the real
> frame on the Tuya bus: `[TUYA->] 55 AA 00 06 00 05 01 01 00 01 01 0E`.
> Observed latency ≈ **1 s**, comfortably inside the 2–5 s premium claim.
>
> **THE BUG THAT BLOCKED IT ALL NIGHT — PubSubClient's 256-byte ceiling.**
> `MQTT_MAX_PACKET_SIZE` defaults to **256** and PubSubClient **silently
> discards any larger inbound packet** — no error, no callback, no log line.
> ozlockserv's full command envelope (`msg_id`/`device_id`/`action`/`grant_id`/
> `payload_hex`/`issued_at`/`source`/`target`/`payload`) is ~280 bytes of JSON
> plus a ~45-byte topic, so **every command from the server was binned by every
> bridge**, while short hand-typed `{target,payload}` bench publishes (~130
> bytes total) always got through. That asymmetry is exactly why the transport
> looked perfect in testing and the app never worked once. The failure mode is
> vicious: server reports `delivery:"delivered"`, `mosquitto_sub` shows the
> message on the broker, and the bridge logs nothing at all.
> Fixed both ends: bridge32 calls `mqttClient.setBufferSize(1024)` (headroom for
> the ozkey-06 sealed envelope, which will be larger again), and ozlockserv now
> sends bridge topics a minimal `{msg_id, target, payload}` — the other fields
> never cross the Thread hop, since bridge32 rebuilds its datagram from
> `target`/`payload` alone. Small wire is the belt to that braces: a
> stock-configured or older bridge still works.
>
> ## 2. The six paths (supersedes ozkey-10 §0)
>
> | # | Path | Where Matter runs | Firmware status |
> |---|---|---|---|
> | 1 | Lock → **Bridge** → Matter → AppleTV/Google/Echo | **bridge** | `cfgMode="matter-bridge"` field exists, unbuilt |
> | 2 | Lock → Matter → ecosystems (native) | lock | **DROPPED — see §3** |
> | 3 | Lock → Bridge → MQTT → BANOI (PREMIUM, ~1–5 s) | — | ✅ **proven 2026-07-29** |
> | 4 | Lock → WiFi → MQTT → BANOI (ECO, 2–10 min) | — | firmware done |
> | 5 | Lock → WiFi → MQTT → OZKEY local → MAOI | — | firmware done |
> | 6 | Lock → WiFi → MQTT → OZPMS cloud | — | **same binary as 5** (ozkey-07 §14) |
>
> After dropping #2 the lock has just **two transports** (Thread-via-bridge, or
> WiFi-direct) and one broker-host config field. Both already exist and are
> proven — there is no new transport mode left to build.
>
> ## 3. OPERATOR DECISION — native Matter on the lock is dropped
>
> Anyone wanting Matter buys the bridge. Rationale:
> - A Matter **Bridge** exposes devices *to* a controller; it is not a
>   controller. So path #1 still requires the customer to own an Apple TV /
>   HomePod / Nest Hub / Echo — a $150–300 AUD box most households do not have.
>   **Smart TVs generally do not qualify**: AirPlay or Google TV does not make a
>   set a Matter controller *and* Thread Border Router (a few high-end Samsung
>   sets ship a SmartThings hub with Thread; that is the exception).
> - Therefore Matter never grows the addressable market — it serves the slice
>   who already bought into an ecosystem, which skews affluent and suits a
>   premium SKU. In Vietnam hub penetration is very low, making it close to
>   irrelevant there; Australia is better but far from universal.
> - What *does* remove the requirement is path #3 — our own bridge, which needs
>   no third-party hardware and IS the hub.
> - Matter's real value is therefore **channel and credibility** ("Works with
>   Apple Home", retail listings, tenders that specify it) — not user-facing
>   reach. Build it **bridged** when there is a commercial reason.
>
> Dropping native Matter deletes, from the lock: a 2.64 MB stack, the DAC/factory
> partition, the RAM pressure, certification burden on a battery device, and
> multi-tens-of-minutes Matter OTA per lock over Thread.
>
> ## 4. Flash budget — MEASURED, and the partition correction
>
> All figures built for `esp32c6`, `CDCOnBoot=cdc`:
>
> | Build | Flash | % of slot |
> |---|---|---|
> | `blecomm` (WiFi + BLE + MQTT + Tuya) | 1,580,668 | 47% |
> | `doorlock` (adds OpenThread + LCD + crypto + LittleFS) | 2,060,142 | 61% |
> | `MatterMinimum` (bare stack, no app logic) | **2,644,204** | **79%** |
> | `MatterOnOffLight` (realistic device) | 2,649,368 | 79% |
>
> Two things this settles. **Matter's cost is almost entirely fixed** —
> Minimum→OnOffLight is only +5 KB, so application logic is noise beside the
> stack. And the whole Thread stack + LCD + crypto + FS cost only **+479 KB**
> over `blecomm`.
>
> **PARTITION CORRECTION (this tripped us up twice).** `default_8MB` is
> **already dual-OTA**:
> ```
> app0  0x10000  0x330000   ← 3.19 MB
> app1  0x340000 0x330000   ← 3.19 MB
> spiffs 0x670000 0x180000  ← 1.5 MB
> ```
> So the "Maximum is 3342336 bytes" in every build result **is** the OTA slot
> size — OTA costs nothing extra. (The "OTA halves it to ~1.5 MB" figure is a
> **4 MB** board's `default.csv`, 1.25 MB per slot.) Also note **BLE DFU does
> not avoid the OTA partition**: the running app cannot be overwritten in place,
> so `esp_ota_*` always writes to the inactive slot regardless of transport.
> Only esptool-over-USB avoids it, and that means visiting every door.
>
> **BOM: N8 for the lock, WITH OTA.** ~2.06 MB in a 3.19 MB slot ≈ **1.1 MB
> headroom**. (Had native Matter stayed, it would have been ~2.9–3.05 MB — a
> 5–10% margin, with RAM tighter still: Matter alone takes 140 KB of 328 KB.)
> The bridge stays the 16 MB GEEK module, which is where Matter belongs anyway:
> mains-powered, WiFi-speed OTA, certified once per home.
>
> ## 5. Commercial validation — ECO latency is fine, missed wakes are not
>
> Operator, on ~500 Airbnb properties in Perth: a 10–15 min PIN-sync delay is
> perfectly acceptable — booking to check-in leaves ample time, and
> `pending_queue` + flush-on-heartbeat already implements exactly this.
> Credential jobs correctly persist (only `unlock` rows carry a 60 s expiry).
>
> **But ECO cannot do remote unlock** — a 2–10 min wake cadence makes "open my
> door" unusable on demand. ECO's honest story is: **unlock is BLE at the door;
> the WiFi link carries key management and audit.** The app copy must say so or
> ECO customers will think the lock is broken. This is precisely what makes the
> ~$50 bridge worth buying, and premium's proposition is "remote unlock actually
> works".
>
> **Open gap for Mode 3/4 at fleet scale:** the risk is not latency, it is a
> **missed wake** — a lock that sleeps through its heartbeat leaves a guest at
> the door at 11pm with no PIN. Grants survive in the queue, but delivery
> **confirmation** needs surfacing to the property manager rather than just
> "queued". Cheaper to design now than to retrofit across 500 doors.

> **BRIDGE ROUTING BUILT 2026-07-27 (lab-verified) + SEQUENCING DECISION.**
> §4's step ordering below is **superseded** by the five-step sequence in this
> note — read this first.
>
> **Shipped (uncommitted).** ozlockserv now routes a command for a Thread lock
> to its *bridge's* own topic. This is exactly the **"S4" server-side task**
> `bridge32.ino:372-380` has been waiting on since 2026-07-25 — and as that
> comment predicted, **no firmware change was needed**: the bridge already
> subscribes to the correct final topic. Changes: `locks.bridge_id` (additive
> migration), `POST /pairings` accepts `bridge_id`, `flushQueueForDevice()`
> selects `ozkey/<site>/bridges/<bridgeId>/command` and names the lock in
> `target`/`payload` per §3, `/pairings/status` returns it, plus an
> `OZLOCK_HTTP_PORT` override (ozlockserv had none). Verified live against the
> real broker + MySQL on scratch device_ids (rows deleted after): bridged →
> bridges topic carrying `target`+`payload`; direct → unchanged locks topic and
> unchanged envelope shape.
>
> **The gap this closed, stated plainly:** only the *app* ever knew a lock sat
> behind a bridge — it read the bridge's Thread dataset over BLE at
> commissioning — and it never told the server. ozlockserv published to
> `locks/<deviceId>/command`, a topic no Thread lock subscribes to. Silent
> failure, every time.
>
> **Decision — a Thread lock stays `registered`, and that counts as reachable.**
> It has no MQTT uplink, so `handleEnroll()` can never run for it and its status
> can never advance past `registered`; the unlock guard demanded `enrolled` and
> would have 409'd forever. Now `bridge_id && registered` is treated as
> reachable rather than back-dating a fake enrollment — `enrolled` means "the
> lock has spoken to us" and issues broker credentials, neither of which is true
> nor needed for a lock that never touches the broker. Direct locks keep the
> stricter bar unchanged.
>
> **ozkey-06 findings (investigated 2026-07-27 — these change the roadmap).**
> The e2e envelope is far more built than ozkey-06 §9 implies:
> - **Built + verified:** `ozkey_commissioner`'s `envelope.dart` (full
>   AES-256-GCM, ver 0x02, anti-replay) and `keyring.dart` (X25519 bonds,
>   per-bond counters). **23 tests pass**, including ozkey-06 §5's frozen byte
>   vectors, the tamper test, the replay test, and "keyring is the sole frame
>   builder → seal → lock-side open yields the exact DPID frame". The Keyring is
>   wired into BANOI via flutter_secure_storage and used for real by the member
>   ceremony.
> - **The blocker (the keystone):** doorlock pairing **never runs the X25519
>   ceremony**, so there is no bond and no `pairing_secret` — the envelope is
>   complete but has no key to use. `doorlock_service.dart:414-435` stores locks
>   as `{"pairing":"v1-bench"}`, commented "no X25519 ceremony ran, so no
>   Keyring bond yet".
> - **Also missing:** firmware `blelock/blecomm/ozcrypto.h` has X25519 + HKDF +
>   HMAC (self-tested vs known-answer vectors) but **no AES-GCM**; and
>   ozlockserv is still wholly plaintext, persisting cleartext credentials
>   (`grants.raw_value` = the actual PIN/RFID UID, `user_name`, validity dates,
>   readable `audit_log` lines). **That is the sovereignty breach** — OZLOCK is
>   specified to keep no usage/credential data, but today it *is* the ledger.
>
> **Sequencing decision (operator-approved 2026-07-27),** chosen so nothing
> built now gets rewritten. The test for each candidate is: *is it
> payload-agnostic?*
> 1. **Bench-verify the 2a relay in PLAINTEXT first** — transport-first
>    precisely *because* of the crypto: debugging AES-GCM over an unverified
>    multicast path cannot distinguish a GCM tag failure from a dropped packet.
>    Prove the pipe while the payload is still readable on a serial monitor.
> 2. **Bridge routing** — done, above.
> 3. **STOP.** Build nothing further on the plaintext base — not hosted-broker
>    auth (§2.3), not the remote-unpair `op` field, not new credential
>    endpoints. All three touch the payload contract and would be written twice.
> 4. **ozkey-06 §9 step 3** — ozlockserv goes relay-opaque: the app seals, the
>    server relays `envelope_hex` and stops storing credentials at all.
> 5. Resume this doc's remaining items on the sealed base.
>
> **Survives the migration untouched:** bridge32's `forwardOverThread()` (it
> never parses the payload), the Thread multicast + doorlock `target` filter,
> and the bridge routing above. **Deleted by it:** server-side
> `buildCredentialFrame()` and `grants.raw_value`.
>
> **Root cause of the rework the operator flagged:** ozkey-06 §9 is a five-step
> migration written 2026-07-08 whose step 1 never started, while the lab kept
> adding features to the pre-migration plaintext base for three weeks. Each such
> feature enlarges the eventual migration — the debt compounds, which is why it
> felt like churn. Step 3 above exists to cap that surface.

> **SCOPED 2026-07-27 (operator directive) — design only, not built.** Written
> right after ozkey-10's Thread commissioning fixes went live-verified
> end-to-end (bridge32 forms Thread, doorlock joins it — see ozkey-10.md's
> 2026-07-27 status note). This doc covers what's still open for **Mode 2a
> (OZLOCK-HOME, premium — Thread → bridge32 → MQTT → ozlockserv)** to go from
> "joins the mesh" to "a real product a non-technical person can set up and
> use for remote unlock." Modes 3/4 (OZKEY/OZPMS — the commercial products)
> are explicitly **out of scope here**: their firmware is already proven
> (`mode=ozkey-local`), their server-side RBAC/fleet model already exists
> (ozkey-07 §3/§8), and each commercial deployment runs its own
> broker/tenant — the "free shared broker" abuse concern this doc spends
> most of its time on simply doesn't apply to them. What's missing for 3/4
> is app+server wiring, not new design.
>
> Consumers: whoever builds the BANOI OZLOCK-HOME onboarding flow next,
> ozlockserv (broker auth issuance), bridge32/doorlock firmware (only if §3
> below turns out to need a firmware change, which it currently doesn't).

---

## 0. Where things actually stand (recap, so this doc doesn't re-litigate it)

| Mode | Transport | Status |
|---|---|---|
| 2a — OZLOCK-HOME premium | Thread → bridge32 → MQTT → ozlockserv | **Join proven live 2026-07-27.** The command-relay path (App → ozlockserv → bridge32 MQTT → Thread UDP multicast → doorlock → MCU unlock) is already *implemented* (`forwardOverThread()` / `forwardHexToMcu()`, both real code, not stubs) but has **not yet been exercised end-to-end on the bench** — only the join itself has been. First thing to actually verify, not design. **Server-side routing to the bridge topic now exists** (built 2026-07-27, see header) — that was the missing link between ozlockserv and this path. |
| 2b — OZLOCK-HOME economy | Wi-Fi direct → MQTT → ozlockserv | Proven (`mode=ozkey-cloud`). Firmware-complete; needs app/server UX wiring — the same UX gap §2 below describes applies here too, verbatim (2b and 2a differ only in whether a bridge is in the picture). |
| 3 — OZKEY | Wi-Fi direct → MQTT → ozkeyserv | Proven (`mode=ozkey-local`). Server-side RBAC already built (ozkey-07). Commercial — not this doc's concern. |
| 4 — OZPMS | same wire contract as #3, cloud deployment | No firmware difference from #3 (ozkey-07 §14). Commercial — not this doc's concern. |

So the honest one-line status: **2b/3/4 are firmware-done, app/server-pending. 2a is firmware-done for joining, needs its relay path bench-verified, and — same as 2b — needs the onboarding UX this doc designs.**

---

## 1. Access model — local BLE vs. remote WiFi/MQTT gateway

Restating this cleanly because it's the frame for everything else: OZLOCK
supports two independent ways to reach a lock, not a hierarchy.

- **Local BLE (close range).** The phone connects directly to the lock's own
  GATT service — the `provision`/`status`/`info` characteristics already
  used for commissioning double as a standing control channel once paired.
  This works identically regardless of which mode the lock is in, needs no
  server/broker, and needs no bridge. It's the fallback that always works
  standing at the door.
- **Remote, via WiFi+MQTT gateway.** This is what "remote unlock from
  anywhere" actually means product-wise, and it has two shapes depending on
  the lock's own radio:
  - **2b (Wi-Fi-direct lock):** the lock *is* the WiFi/MQTT gateway. App →
    ozlockserv → MQTT → lock, no intermediary.
  - **2a (Thread lock):** the lock has no WiFi radio at all — bridge32 is
    the gateway. App → ozlockserv → MQTT → bridge32 → Thread UDP multicast
    → lock. This is the "conventional doorlock uses BLE at close range, a
    BLE-WiFi gateway for remote" pattern the operator described — bridge32
    *is* that gateway, just over Thread on the lock-facing side instead of
    BLE, because BLE's range/topology doesn't suit a whole-mesh relay the
    way Thread's multicast does.

Matter (Mode 1) is a *third*, unrelated way to reach a lock — through
someone else's border router and someone else's app (Apple Home/Google
Home/Alexa) — and stays out of scope per ozkey-10 §1/§6.

---

## 2. App/server config UX — MQTT endpoint + site_id + auth

**The problem, stated plainly:** today, correctly provisioning a lock means
knowing `broker_host:port` and `site_id` — infrastructure details a
residential customer will never understand and shouldn't have to. But
*someone's* broker is doing the relaying, and if it's ours (the free hosted
one), it needs to not be free-for-all-the-internet.

**Proposed design:**

1. **Broker selection = a dropdown, not a text field.** The app ships a
   short list of ready-to-use endpoints:
   - the lab/dev broker (`10.1.1.20:1883`) for bench work — hidden behind a
     debug flag in release builds, never shown to a real customer;
   - the hosted production broker (`ebizco.com.au:1883` or whatever the
     real hostname ends up being) — the default, one-tap choice for anyone
     who just wants it to work;
   - **"Advanced: enter your own"** — a free-text `host:port` field for
     anyone self-hosting ozlockserv (a power user, or eventually an OZKEY/
     OZPMS operator who wants dedicated infra even for a home-mode
     deployment).
2. **`site_id` is never user-facing in the default (hosted-broker) path.**
   Generated randomly (UUID or similar) by the app at first pairing and
   sent as part of the provision payload, same field that already exists —
   just never surfaced in UI. The "Advanced" path can expose a "custom
   site_id" field for someone who actually needs one (e.g. running their
   own ozlockserv instance for multiple homes and wanting to namespace
   them), but it defaults to auto-generated + hidden either way.
3. **Hosted-broker auth, to stop free-for-all use of `ebizco.com.au`:**
   ozlockserv issues a per-app-install `username`/`password` (or a token
   serving the same purpose) the *first time* the app talks to it — before
   any lock provisioning happens, as part of app setup/first-run. This
   credential rides along with every subsequent MQTT connect the app or its
   bridges/locks make to the hosted broker. Mechanically this reuses the
   same shape as ozkey-07's fleet-slice `orgs/operators/scopes/tokens`
   work — that's already built server-side for the commercial modes; this
   is the same primitive issuing a lightweight per-install credential
   instead of an org-scoped one. Not a new auth system, a new *issuance
   path* for one that exists.
4. **Self-hosted path owns its own auth.** If the user picks "enter your
   own" broker, the same screen also asks for the `username`/`password`
   *they* set up on *their* broker — ozlockserv/ozkeyserv already read
   broker credentials from the provision payload's config, this just means
   actually surfacing those fields in the UI for that one path, not
   inventing a new field.

Net effect: a residential user taps "use ready-made server," never sees
`site_id` or a broker hostname, and the hosted broker still can't be
hammered anonymously. Someone who wants their own infra gets the manual
path, unchanged from what already works today.

---

## 3. Bridge routing — does bridge32 need a lock database?

**No — not for the relay path that already exists, and that's deliberate,
not an oversight to fix.**

The current design (`forwardOverThread()` in bridge32.ino,
`onThreadUdpPacket`-equivalent in doorlock.ino) is **multicast + per-lock
filter**, not a routed unicast:

```
App → ozlockserv → MQTT (bridge's own command topic)
    → bridge32: {target: device_id, payload: hex}
    → Thread UDP multicast to the WHOLE mesh (OZ_THREAD_GROUP)
    → every lock on the mesh receives it, checks "target == my deviceId?"
    → only the matching lock actually forwards the frame to its MCU
```

Bridge32 never needs to know which locks exist, which ones are alive, or
maintain any mapping — it just relays whatever `{target, payload}` MQTT
hands it onto the shared multicast group, and the *locks* do the filtering.
This is why adding a lock, removing one, or a lock being temporarily
offline requires zero bridge-side state change. A "mini DB" would be
solving a problem this design doesn't have.

**Where bridge-side state genuinely would matter (not built, flagging for
later, not this doc's scope to design in full):**
- The remote-unpair/factory-reset extension discussed separately (adding an
  `op` field alongside `target`/`payload`) fits this *exact same*
  multicast-and-filter pattern — still no bridge-side DB needed, the lock
  being told to reset checks `target` exactly like it already does for
  unlock frames.
- If a future feature needs the *app* to know which locks are actually
  reachable through a given bridge right now (e.g. a "bridge health" or
  per-lock last-seen dashboard), that's state either the **lock** reports
  back up through the same MQTT path (heartbeat-style, already how Wi-Fi-
  direct locks report liveness) or the **app's own local cache** already
  tracks (`ozlock_locks`/`stateJson`) — not something bridge32 itself needs
  to persist. Worth revisiting only if/when that specific feature is
  actually wanted.

---

## 4. Suggested next steps, in order

> **SUPERSEDED 2026-07-27 — see the sequencing decision in this doc's header.**
> Items 2 and 3 below are now explicitly **deferred behind the ozkey-06
> envelope migration**: both touch the payload contract, so building them on
> the plaintext base means writing them twice. The live ordering is the
> header's five steps. Item 1 is unchanged and still first; item 4 still holds.

1. **Bench-verify the 2a unlock relay end-to-end** — this is proven-by-code-
   reading, not proven-by-testing yet. Publish a `{target, payload}` MQTT
   message to bridge32's command topic and confirm the paired doorlock
   actually unlocks. This is the one item here that's a bug hunt, not a
   design decision, if it doesn't work first try.
   *(Note: bridge32 also accepts `{device_id, payload_hex}` as a fallback —
   bridge32.ino:404-407 — so a raw `mosquitto_pub` straight to the bridge
   topic tests the relay without involving ozlockserv at all, which is the
   cleanest way to isolate the transport. Unlock frame:
   `55 AA 00 06 00 05 01 01 00 01 01 0E`. The bridge's own `ozb-<machex>`
   device_id is MAC-derived, so it is **stable across factory resets** —
   capture it once from serial or the BLE `info` characteristic; bridge32
   never publishes on MQTT, so it cannot be discovered passively.)*
2. ~~Hosted-broker credential issuance (§2.3)~~ — **deferred behind the
   envelope migration** (header, step 3).
3. ~~Broker-selection dropdown + hidden `site_id` (§2.1/§2.2)~~ — **deferred
   behind the envelope migration** (header, step 3).
4. Leave §3's routing model as-is — nothing to build there. *(Confirmed
   2026-07-27: the server-side routing built this session needs no bridge-side
   state, exactly as §3 predicted.)*
