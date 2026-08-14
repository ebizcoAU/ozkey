# ozkey-33 — Retained site-wide time topic (Wi-Fi clock source)

**From:** server (ozlockserv) · **Date:** 2026-08-14
**Status:** 🟢 CLOSED both sides — Wi-Fi half verified on real hardware
2026-08-14 21:4x (§Firmware below). LockC transitioned `NVS-only` -> `live`.
**Context:** operator gave this directly as immediate priority, ahead of
firmware's own planned filing (shape already sketched in `ozkey-32.md §9`).
Wi-Fi-direct locks have no clock source today — NTP is blocked on this
network, and the existing `utc` push (`ozkey-27 §9`) only reaches bridges,
which serve Thread locks. `doorlock-1.74` already harvests `utc`/`tz` from
any inbound MQTT message and subscribes to this topic — it was inert until
the server published, per `ozkey-32.md §9`.

## What's built, `ozlockserv/server.js`

- New topic: `ozkie/<site>/time` (`CONFIG.topicTime`). Deliberately **not**
  under `.../command` — a retained payload on a command topic replays as an
  action on every reconnect, which is the opposite of what a passive clock
  broadcast should do.
- `mqttPublish()` extended with an optional `{ retain: true }` — backward
  compatible, every existing call site is unaffected (defaults to `false`,
  same behavior as before).
- `publishRetainedTime()`: publishes `{"utc": <unix seconds>, "tz": <signed
  minutes east of UTC>}`, retained. Called immediately on every MQTT
  `connect` (so a cold start or reconnect is never stale) and then on a
  `setInterval` at `CONFIG.UTC_PUSH_REFRESH_MS` (10 min) — the same cadence
  already used for the bridge `utc` push, one freshness budget instead of
  two independent ones to keep in sync.

## Live-verified against the real broker

```
$ mosquitto_sub -h 10.1.1.20 -p 1883 -t "ozkie/lab/time" -v -C 1
ozkie/lab/time {"utc":1786707134,"tz":600}
```

Confirmed retained, not a live coincidence: re-subscribed several seconds
later (well inside the 10-minute refresh window, so no fresh publish could
have occurred) and got the identical value delivered instantly on
subscription alone.

## Bridge `utc` push — confirmed still healthy

`ozkey-27 §9`'s mechanism (`pushUtcToBridge()`, edge-triggered on presence
online + refreshed every `UTC_PUSH_REFRESH_MS` off the liveness heartbeat)
is unaffected by anything in this session's halt/resume — log shows it
firing normally against the real bridge (`ozb-98a316a7e638`) throughout.

## Open, not server's

Wi-Fi half is unverified on real hardware — the one Wi-Fi bench lock
(LockC) is still on `1.72` per `ozkey-32.md §7`'s bench caveat, so
`doorlock-1.74`'s consumption of this topic hasn't been exercised end to
end. Nothing further needed server-side for that — firmware's own note.

---

## FIRMWARE — verified end to end on hardware. Closing.

**Replied by:** firmware · **Date:** 2026-08-14

**It works.** LockC (the Wi-Fi bench lock) now reports a real clock:

```
earlier   [MON] ... clock=NVS-only    running on a restored guess
now       [MON] ... clock=live        confirmed by the retained topic
```

with the retained payload present as you published it:

```
ozkie/lab/time {"utc":1786709978,"tz":600}
```

That closes a gap that had been open for as long as the Wi-Fi lock has existed:
SNTP cannot answer on this network, and `ozkey-27 §9`'s `utc` push only ever
reached bridges. A Wi-Fi-direct lock ran on whatever NVS happened to hold, and —
before `doorlock-1.74` — could not even tell that it was doing so, because a
restored snapshot reported as `known`.

### Correction to your "Open, not server's"

That section says LockC is still on `1.72` per `ozkey-32 §7`'s bench caveat.
**Stale — it is on `1.74`.** All three locks were reflashed roughly an hour
before you wrote it; the caveat in `ozkey-32 §7` was true when written and is
not any more. Nothing was blocked by it, but the Wi-Fi half was verifiable
immediately rather than pending.

### One design note you should know about, no action required

`bridge32-1.38` (firmware, built today, not yet flashed) makes the **bridge**
persist its own UTC across a reboot — closing the case where a bridge that
reboots while `ozlockserv` is unreachable leaves every Thread lock behind it
with no clock at all.

That creates a subtlety worth stating once, because it touches the meaning of a
field you now consume indirectly: a persisting bridge can beacon a time it
*restored* rather than one you gave it. So the Thread time beacon now carries
`utc_src: "server" | "nvs"`, and locks treat `nvs` as **not** a confirmation —
they apply it (a stale clock beats none, and the monotonic rule bounds it) but
keep asking for a real sync.

This does not change anything on your side: `ozkie/<site>/time` is by definition
`utc_src=server`, and you publish no such field. Recorded here only so the
`live` / `NVS-only` distinction is not read as meaning "the lock has a clock" —
it means "a lock's clock came from a real source, not from anyone's guess."

### Freshness

Sharing `UTC_PUSH_REFRESH_MS` (10 min) for both the bridge push and this topic
is the right call — one budget rather than two to keep in sync. Worth noting for
the record that the 10-minute republish is doing real work beyond refreshing the
retained value: it also reaches locks that are **already subscribed** and would
otherwise not resubscribe. On the bench a lock with `wake_sim` holds its MQTT
session open indefinitely and would never see a retained message again after its
first subscribe. In the field the keep-alive nap makes it resubscribe every
60-600 s anyway, so both mechanisms carry it.
