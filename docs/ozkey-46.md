# ozkey-46 — Bridge clock architecture: MQTT-over-WiFi is the only time source, Thread is distribution-only

**Status: 🟢 LIVE-VERIFIED, current architecture.** Written 2026-09-01 by
firmware, at the operator's request, for reuse when the bridge role is ported
onto real CH584M/ESP32-C6 network-co-processor hardware
(`ch584m-network-coprocessor-architecture-locked`, `ozkey-45`). This
documents *how the bridge gets its own clock today* — not a proposal.

## The one-line version

```
ozlockserv --(MQTT, over the bridge's own WiFi)--> bridge --settimeofday()--
                                                       |
                                                       +--(Thread beacon)--> locks
```

The bridge's clock comes from exactly one place: an `utc` field the server
publishes on the bridge's own MQTT command topic, received over the bridge's
own WiFi connection. Thread plays **no part** in how a bridge acquires time —
it is purely how that already-acquired time gets forwarded on to locks, which
have no WiFi of their own and therefore no other way to learn it.

## Why NTP isn't in this picture at all

Firmware ran real SNTP (`configTime()`) through fw 1.35. Removed at 1.36 —
`bridgeBLE.ino:2174-2197`:

- Measured on the lab network 2026-08-11: DNS resolves fine, **UDP 123
  (NTP) times out** — the network filters it.
- This is not a lab-only quirk: "a hotel or office that blocks outbound NTP"
  is explicitly named as the expected real deployment environment.
- Running SNTP *and* the server's MQTT push side by side meant two writers
  with no arbitration — NTP could silently step a clock the server had
  already set correctly.
- The system architect's call: don't build arbitration machinery for a
  second source that isn't dependable anyway. Delete NTP, keep one source.
  "If the bridge cannot reach our server there is no product anyway, so a
  fallback clock buys nothing and costs a class of bug."

**For a CH584M port: do not reintroduce NTP/SNTP as a bridge-side time
source.** If the target deployment network is expected to filter outbound
UDP 123 (assume so unless proven otherwise for that site), MQTT-pushed `utc`
should stay the only source there too.

## The actual mechanism (code)

`mqttMessageReceived()`, `bridgeBLE.ino:1012-1064` — the bridge's own MQTT
callback, firing on messages the bridge's own `mqttClient` receives on its
subscribed command topic:

```cpp
const uint32_t utcIn = doc["utc"] | 0UL;
if (utcIn >= OZ_TIME_FLOOR) {
  struct timeval tv = { .tv_sec = (time_t)utcIn, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  ...
  sendTimeBeacon();   // <- only AFTER the bridge has its own clock set
}
```

Server-side origin: `ozlockserv` pushes `utc` on the bridge's command topic
(`server.js:1150`). Nothing about this path touches Thread — it is WiFi/MQTT
in, `settimeofday()`, done. `sendTimeBeacon()` is called *after*, and is the
only place Thread enters: it is the bridge relaying its now-known time onward
to Thread-only locks (`ozkey-21` §3.3, daily beacon + a free stamp on every
forwarded command).

Trust model: same as everywhere else this value flows — a lock's own
monotonic-forward rule and 400-day cap bound what a bad value can do. Broker
ACLs are the real access-control gate (still a pre-production blocker per
`ozkey-13` S8/S9), not anything time-specific.

## Thread role (Leader/Child) is irrelevant to a bridge's own clock

Live-verified 2026-09-01 during a 2-bridge bench test (`XFtposDecisions-142/
143`): a second bridge came up attached to the first bridge's Thread mesh as
a **Child** (see the Gotcha below for why). Despite that, it acquired a real
server clock directly and promptly over its own WiFi/MQTT session, with zero
dependency on its Thread role or on the other bridge:

```
[THREAD] attach role=Child after 950ms          <- Thread state, unrelated
...
[MQTT] connected
[STATUS] BROKER_OK
...
[TIME] utc=1788225764 accepted from server/bench over MQTT (first real sync this boot)
```

The `[MON]`/panel-visible "NO SERVER TIME" string that shows before that
first sync (`drawClockRow()`, `bridgeBLE.ino:547-577`) is a **grace-period
display state**, not evidence of a Thread dependency — it is on screen for
any bridge, Leader or Child, in the seconds between MQTT connecting and the
server's first `utc` push landing. Don't read "was showing NO SERVER TIME,
was showing THREAD: CHILD" as causally linked; confirm via the log instead
(`[TIME] ... accepted ... over MQTT`) whichever way it goes.

## Gotcha to carry forward, still open (not part of the architecture above)

Also found in that same 2-bridge test: `bridgeBLE`'s Thread-dataset-restore
feature (`ozkey-45`/2026-07-28 — lets the app hand a *replaced* bridge back
its own prior mesh so locks don't orphan) fired for a **second, different**
physical bridge, not a genuine replacement. The app's provisioning write
supplied Bridge 1's exact dataset (`OZ-A7E638`, same channel/panId) to
Bridge 2's claim, so Bridge 2 restored it verbatim and attached as Bridge 1's
Thread **child** instead of forming its own independent mesh — the opposite
of the stated v0 rule ("this bridge always FORMS, never joins... it is the
only network former in the home", `bridgeBLE.ino:~271`).

Firmware did exactly what the restore feature is designed to do; this looks
like an app-side data mix-up (reusing a cached dataset across two different
bridge claims) rather than a firmware defect. Filed for App to check —
see `XFtposDecisions-142`/`143`. **Relevant to a CH584M port only in that
the same restore-on-claim logic will carry over as-is; whatever fix comes
out of that ticket should carry over with it.**
