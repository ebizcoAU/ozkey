# ozserver-core

The lock side of the OZKIE servers, written once (ozkey-50 §2). It covers MQTT
transport, the lock/bridge registry and presence, XF-144 bridge failover, the
opaque relay, the write queue, the audit trail, the MySQL schema bootstrap and
the lock-side REST API. Each product is a **profile**: it builds a CONFIG,
creates one core, mounts the lock API behind its own auth, and adds its own
routes and tables.

| Profile | Directory | Status |
|---|---|---|
| ozlock | `ozlock/server/` | on the core since S0b (`392f56e`) |
| ozpms | `ozpms/server/` | S1 |
| ozlodge | `ozlodge/server/` | S2 |

## Deploying

The core has no `node_modules` of its own. Each profile requires it by relative
path (`require('../../ozserver-core')`) and passes in its own `mysql2` and
`mqtt`, so every app keeps owning its dependency versions. **A deploy must ship
this directory beside the profile**, keeping the same relative layout:

```
<root>/ozserver-core/
<root>/ozlock/server/
```

## Contract

```js
const { createCore, util } = require('../../ozserver-core');
const core = createCore({ CONFIG, logEvent, mysql: require('mysql2/promise'), mqtt: require('mqtt') });
```

- **One core per process.** A second `createCore()` throws.
- **`util`** works without a core: secrets (`loadDotEnv`, `resolveSecret`,
  `enforceSecrets`), `detectLanIp`, `normalizeMac`, `makeSecret`. Profiles need
  it to build CONFIG, so it is usable before `createCore()` runs.
- **`logEvent(level, message)`** belongs to the profile (it owns the event ring
  its dashboard reads).
- Every other export is on the returned `core` object. Call
  `core.initDatabase()` and then `core.initMqtt()`; after `initDatabase()`,
  `core.pool` is the mysql2 pool.
- **`core.mountLockApi(router, { auth, onLockRemoved })`** puts the lock API
  on the profile's express Router: pairing, registry reads, settings, sealed
  grants/unlock/bond verbs, bridge reset, removal, queue, sim. `auth` is
  middleware run first on every one of those routes. It is **required**:
  without it the call throws at startup (fail closed, as S−1). ozlock passes
  an explicit open pass because its REST is unauthenticated today.
  `onLockRemoved(lock)` is an optional fire-and-forget hook after
  `DELETE /locks/:id` (ozlock: Nexus decommission). The profile keeps
  `/health`, `/events` and anything product-specific.
- Core modules read their dependencies lazily through `ctx.js`. The stand-ins
  are read-only: setting, defining or deleting a property through one throws.

### Required CONFIG

`createCore()` throws if any of these is missing:

| Key | Meaning (ozlock's value) |
|---|---|
| `SITE_ID` | site this server owns; topics are site-pinned (`'lab'`) |
| `DB` | mysql2 pool options incl. `database` |
| `MQTT_URL` | broker (`mqtt://10.1.1.42:1883`) |
| `SERVICE_NAME` | MQTT clientId prefix and queue envelope `source` (`'ozlockserv'`) |
| `ENROLL_ISSUED_BY` | `issued_by` in the enrollment ack (`'OZLOCK/'`) |
| `DEFAULT_HEARTBEAT_S` | heartbeat given to new locks (`60`) |
| `FAILOVER_GRACE_MS` | how long a bridge must read offline before XF-144 moves its locks (2 min) |
| `FAILOVER_SWEEP_MS` | failover sweep / presence-resubscribe tick (30 s) |
| `UTC_PUSH_REFRESH_MS` | re-push utc to online bridges (10 min) |
| `AUDIT_LOG_RETENTION_MS` | audit_log age limit (90 days) |
| `ENFORCE_CAPS` | refuse a remote unlock the lock's caps don't allow (XF-48; ozlock: on unless `OZLOCK_ENFORCE_CAPS=0`) |

The profile also supplies the topic builders (`topicCommand`,
`topicBridgeCommand`, `topicTime`) and the `SUB_*` subscriptions. A `SUB_*_LEGACY`
twin is subscribed alongside its primary during a topic move (ozlock's S16).

### Opt-in schema cutovers

`DROP_PLAINTEXT_GRANT_COLUMNS: true` drops `grants.raw_value`, `user_name`,
`date_from` and `date_to` on boot (S3 + ozkey-29). This is irreversible, so
it is never a core default. ozlock sets it. A profile that runs on the
operator's own data (ozpms) leaves it unset.

## Tests

- `cd ozserver-core && npm test`: contract tests, no DB or broker (required
  CONFIG, the auth hook, read-only lazy deps).
- `cd ozlock/server && npm test`: the route behaviour, pinned by golden
  transcripts in `test/golden/`. Rerun it after any change here.
