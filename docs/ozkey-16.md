# ozkey-16 — bridge32 MQTT uplink (PM priority #2): lock heartbeat/logs/
    delivery-confirmation back to the server, for Thread/bridged locks

**Status: DESIGN, not started.** Scoped 2026-08-09. PM priority #2 from
`ozkey-12.md` §14: "Bridge32 → MQTT uplink — lock heartbeat/logs back to
server. Enables lock state reporting, delivery confirmation, and battery
monitoring." Same root cause as ozkey-12 §8.6's finding: "no serial and no
Thread uplink/ack path" — this closes it.

## 1. Current state, precisely

**WiFi-direct locks already have this, fully built.** `publishHeartbeat()`
and `publishLog()` (`ozdoorlock_core.h`) publish JSON to `topicHeartbeat`/
`topicLog` whenever `mqtt.connected()` — which is only ever true for a
direct-WiFi lock, since a Thread lock has no MQTT client of its own.

**Thread locks already capture the data locally — they just have no
transport out.** `publishLog()` calls `txlogAppend()` **unconditionally
first**, before the `mqtt.connected()` gate. `txlogAppend()` is a real,
durable local log: LittleFS, JSONL, two rotating files of 5,000 lines each
(10,000-event ring buffer), survives reboots. Every `publishLog()` call
site already exists and already fires for Thread locks — `granted`,
`bond_revoked`, `invite_cancelled`, `bonds_listed`, `dp_report`,
`battery_alarm`, `expired` (the XF-58 assisted-unlock timeout case) — this
is genuinely the delivery-confirmation data the priority item asks for,
just stuck on-device.

**`bridge32.ino`'s Thread UDP is send-only.** `threadUdp` (an `OThreadUDP`
instance) is used exclusively via `beginPacket`/`write`/`endPacket` in
`sendToThreadGroup()` — its own comment says "sender: plain unicast bind."
There is no receive path at all on the bridge side — no `parsePacket()`,
no socket read loop, nothing. The bridge has never listened for anything
from a lock; only sent to it.

**The lock-side receive fix already exists and is proven — just not on the
bridge.** `ozdoorlock_core.h`'s `pollThreadUdp()` (built during the
`ozkey-10`/`ozkey-11` Thread relay work, `[[thread-relay-debug-2026-07-28]]`)
uses raw lwIP sockets (`lwip_recvfrom` on a socket bound via
`otUdpBind`-equivalent), because `OThreadUDP`'s own receive half never
worked on this Arduino-ESP32 core (confirmed dead end at the time: "no
inbound multicast ever matched that socket"). This is the exact mechanism
the bridge would need for its own new receive capability — a port, not a
reinvention.

## 2. What's needed — three pieces, reusing what exists wherever possible

### Lock side: an uplink send path

New function, call it `publishUplink()` or fold into existing call sites —
when `mqtt.connected()` is false (Thread mode) but Thread UDP is ready,
send the same JSON payload that would have gone to MQTT over Thread UDP to
the bridge instead. Concretely: wrap `{from: deviceId, kind: "heartbeat"|
"log", ...payload}` and unicast/multicast it the same way the bridge
already sends commands down (`sendToThreadGroup`'s pattern, mirrored in
reverse) — reusing the same multicast group/port the downlink already
uses, or a second dedicated port to keep directions cleanly separated
(recommend a second port — simpler to reason about than shared traffic on
`OZ_THREAD_UDP_PORT` 5052, and avoids the receive-side needing to
distinguish "is this a command for me" from "is this my own echo").

### Bridge side: the new receive capability

Port `pollThreadUdp()`'s lwIP-socket technique into `bridge32.ino`, bound
to the new uplink port. On receipt: parse `{from, kind, ...}`, then
publish to MQTT under **the lock's own topic shape**
(`ozkey/<site>/locks/<from>/heartbeat` or `.../log`), not the bridge's own
topic. This is the one design choice that makes the server side free —
see §3.

### Bridge side: heartbeat cadence for locks it's relaying

Open question, not resolved here: does each Thread lock send its own
uplink heartbeat on the same 60-600s cadence a WiFi lock would (per-lock,
many small Thread packets), or does the bridge itself send one
consolidated "these N locks are alive" heartbeat periodically (fewer
packets, but a new message shape the server doesn't already parse)? Recommend
**per-lock**, matching the WiFi behavior exactly and keeping §3's "server
needs zero changes" property — but flagging this as a real bandwidth/
battery tradeoff worth a second opinion, not a unilateral call.

## 3. Server side: designed to need zero changes, verify before assuming

`ozlockserv`'s `SUB_HEARTBEAT` is `ozkey/lab/locks/+/heartbeat` — a
wildcard. The existing handler in `onMqttMessage`'s MQTT callback
(`server.js`) doesn't care which physical device published to that topic,
only that the topic's device_id segment matches a known lock. If the
bridge relays a lock's heartbeat under **that lock's own device_id** in
the topic (not the bridge's), the server literally cannot tell the
difference from the lock publishing directly — `last_seen_at`, `fw`,
`transport`, `caps` all update exactly as they do today. Same should hold
for the `log` topic once §1b/§6's broader "every MQTT command sealed"
migration is accounted for — **check whether the log/heartbeat topics also
need `envelope_hex` treatment post-ozkey-13**, since this doc predates
confirming that; uplink data arguably needs the same "app can't be
impersonated" property command data got, though the threat model differs
(a forged heartbeat is a nuisance, not a physical-access breach — worth a
real decision, not an assumption either way).

## 4. Open questions needing a decision before implementation

1. Second Thread UDP port for uplink vs. reusing port 5052 with a
   direction/kind field — recommend second port (§2).
2. Per-lock heartbeat relay vs. bridge-consolidated — recommend per-lock
   (§2), flagged as a real bandwidth tradeoff.
3. Does uplink data (heartbeat/log) need the same sealed-envelope
   authentication treatment as downlink commands now have (ozkey-13), or
   is plaintext acceptable given the different threat model (forged
   heartbeat vs. forged unlock)? Not decided.
4. Full historical `txlog` backlog drain on first uplink connection, or
   only new events going forward? A full 10,000-event backlog drain over
   a Thread link on every reconnect could be substantial traffic — probably
   want only-new-events plus a bounded catch-up window, not a full replay,
   but this needs an explicit call, not an assumption.

## 5. Work breakdown (once §4 is decided)

| Task | Where | Description |
|---|---|---|
| U1 | `ozdoorlock_core.h` | New uplink send path — heartbeat/log payloads over Thread UDP when MQTT unavailable |
| U2 | `bridge32.ino` | New lwIP-socket receive path on the uplink port (port `pollThreadUdp()`'s technique) |
| U3 | `bridge32.ino` | Relay received uplink data to MQTT under the lock's own topic shape |
| U4 | `ozlockserv` | Verify zero server-side change is actually sufficient (§3) — live test, don't assume |
| U5 | TBD per §4.3 | Sealed-envelope treatment for uplink data, if decided needed |
| U6 | `ozdoorlock_core.h` | Backlog/catch-up policy for the existing `txlog` buffer, per §4.4 |

Not started. This is the scoping pass only.
