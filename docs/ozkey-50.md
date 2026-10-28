# ozkey-50 — One server core, three deployments: ozlock, ozlodge, ozpms

> Status: **DRAFT for operator approval, 2026-10-18, firmware (team lead).** Third in the agreed order
> (52 → 51 → 50 → 53). **The servers belong to the server developer:** this doc proposes, they
> design the details and build, on the operator's own go (their standing rule since the 08-14 halt).
> Inputs:
> - the operator's tier description of 2026-10-18;
> - `proddev/` (ozlock v4.0, ozlodge v2.4, ozpms);
> - ozkey-48 (OZKIE v2), ozkey-51 (tenancy), ozkey-52 (device certificates);
> - a read of all three `server.js` files on 2026-10-18.
>
> 🟢 **Operator rulings 2026-10-18 (§9):**
> - **Q1:** direction APPROVED;
> - **Q3:** guest-key binding accepted (ozkey-48 §3.3 `OZINV2`);
> - **Q4:** only hotel staff approve resellers;
> - **Q5:** `issuer` role accepted (ozkey-48 §6);
> - **Q6:** both the broker ACL and the firmware fix.
>
> - **Q2:** (a), booking payloads encrypted end to end to ozlodge's key; Cloudflare stays as the
>   hotel-owned tunnel.
>
> **rev 2 (same day): the server developer's review is folded in** (10 points; their claims were
> checked against the code). Main changes:
> - §3: no server holds bond #0. A bounded `issuer` role is added;
> - §8: secrets, plus ozpms's unsealed unlock;
> - §6: lease fencing;
> - §7: a test suite first, then a dual-topic period.

## 0. The idea in one paragraph

ozlock, ozlodge and ozpms do the **same job toward the locks**: talk to bridges, track presence,
relay sealed OZKIE messages, queue credential writes, take in events. They differ only in **who is in
charge and where they run**. So build the lock side **once**, as a shared core, and keep three thin
**profiles** on top for each business. Do it **before** the OZKIE v2 relay is written, so v2 is
written once and not three times.

## 1. What the code shows today (2026-10-18)

| | ozlock | ozlodge | ozpms |
|---|---|---|---|
| Size | 4,611 lines, 28 routes | 1,949 lines, 25 routes | 1,392 lines, 20 routes |
| Origin | the mature one | own model (rooms, PMS, operators, scopes) | **fork of ozlock**: 19 of 20 routes identical |
| MQTT topics | `ozkie/<site>/…` | `hotel/…` and `ozkey/<site>/…` | `ozkey/pms/…` (old scheme) |
| **Bridge handling** (presence, failover, scan, time push) | ✅ full (XF-138/144/152) | ❌ none | 🟡 **static routing only**: a command goes to `locks.bridge_id` (ozkey-11). No presence, failover, scan or time push, yet every lock now goes through a bridge (operator, 2026-10-18) |
| Fixes XF-144/152/155 (presence starvation, revoke reliability) | ✅ | ❌ | ❌ |
| v2 groundwork (`ozkie_version`, owner-only, 501) | ✅ today | ❌ | ❌ |
| Builds Tuya credential frames on the server | removed (C2) | ✅ `buildCredentialFrame` | ✅ `buildCredentialFrame`, `buildUnlockFrame` |
| Region / 3-level admin | — | operators + scopes exist | ❌ not in code yet |

The same helpers are copied into all three: `normalizeMac`, `deviceIdFromMac`, `buildTuyaFrame`,
`buildDpPayload`, `initDatabase`, `mqttPublish`, `flushQueueFor…`, `initMqtt`, `guardDb`, `boot`.
Each copy has since been fixed differently. This is the "two builders diverge" failure the firmware
team hit twice in August, now at the scale of whole servers.

## 2. Target shape

```
              ┌──────────────── profiles (business side, one per product) ─────────────────┐
              │ public (ozlock)        lodge (ozlodge)              pms (ozpms)             │
              │ owners, apps,          rooms, MAVEN PMS sync,       company→region→manager  │
              │ Nexus directory,       staff + scopes, reseller     →house, tenancy         │
              │ NO usage logs          booking API (§4), mDNS LAN   (ozkey-51), custodians  │
              └───────────────────────────────┬─────────────────────────────────────────────┘
                                              │ core API (in-process module)
┌─────────────────────────────────────────────┴──────────────────────────────────────────────┐
│ core (lock side, written once)                                                              │
│  transport   MQTT to bridges, one topic scheme ozkie/<site>/…                               │
│  registry    locks, bridges, presence, multi-bridge failover, ozkie_version from bridge     │
│  relay       v1 envelopes + v2 OSCORE passthrough; Echo routed to the original sender;     │
│              one active sender per context; seq blocks (ozkey-48 §4)                        │
│  queue       credential writes only, sealed at flush (§4.3 epoch); actuation never queued  │
│  service     service/custodian keys (env/0600 + KEK), OSCORE relay contexts in DB          │
│  events      intake, seq gap detection, utc_s / arrival time (ozkey-48 §5.1)              │
│  identity    device-certificate check (ozkey-52) + revocation list                         │
│  audit, time push, DB migrations, health                                                    │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **One codebase, three apps.** Each product is a small app that imports the core and adds its own
  routes and tables. The deployment chooses which profile runs.
- **The topic scheme becomes `ozkie/<site>/…` everywhere.** `hotel/…` and `ozkey/pms/…` are retired,
  and bridges are configured with the site. The `ozkey` prefix shouldn't appear in anything new,
  since OZKEY is a third party's trademark.
- **Server-side Tuya frame building is deleted.**
  - v1 credential writes come sealed from the app or from the service bond.
  - v2 credential writes are OSCORE `cred` requests from the service bond.
  - The sealed-envelope promise (C1/C2) then holds on all three tiers, not only ozlock.

## 3. The three deployments

| | public (ozlock) | lodge (ozlodge) | pms (ozpms) |
|---|---|---|---|
| Runs on | our cloud (STUN-like public relay) | the hotel's own machine on its LAN | the customer's own VPS |
| Bridges point to | ozlock | ozlodge (LAN address) | ozpms |
| App reaches it via | Nexus | Nexus or any reseller → **Cloudflare Tunnel** → ozlodge (§5) | Nexus → VPS |
| Bond #0 (lock admin) | the owner's phone | **a hotel admin's staff device** (a person), never the ozlodge server | ozpms's custodian key per region (ozkey-51): **no routine unlock**, by lock rule |
| What the server holds | relay only | an **`issuer`** bond (below) + a `service` bond (events) | custodian bond (above) + `service` |
| Remote unlock | owner only | hotel admin only, from the staff app (relayed). Guests on premises only | **occupant**: the top resident role in custodial mode (ozkey-51 §2), not a member. ozkey-48's owner-only rule generalises to "top resident role only" |
| Door-event logs | **none** (privacy promise) | yes, hotel audit (service bond) | yes, landlord audit (custodian) |
| Without internet | — | **doors, PINs, front desk keep working** on the LAN; only remote/reseller stops | doors and PINs keep working; remote stops |
| Device trust (ozkey-52) | eBizco root | configurable | configurable |
| Events module | **not loaded at all** (structural, not a setting) | loaded | loaded |

**Why no server holds bond #0** (server review #1). ozkey-48 §6 already says the service role never
holds bond #0 and never actuates. A compromised ozlodge box holding bond #0 could open every room
and mint keys. So the hotel's server gets a new, lock-enforced **`issuer`** role, a change to
ozkey-48 §6 (§9 Q5):
- it may only create **member** invites;
- each invite has a bounded window (≤ N days, set at claim by bond #0);
- no actuation, no admin or bond verbs, no reading credentials;
- the lock rejects anything else.

A stolen ozlodge box can then mint short guest keys until the admin revokes `issuer`, but it cannot
open a door itself or take over a lock.

**Public-profile privacy** (server review #9): "no usage logs" must hold by construction. The public
app does not import the core's events module, so a later change can't turn logging on by accident.

## 4. OZLODGE open booking API: the anti-monopoly piece

The operator, 2026-10-18:
- the hotel chooses its resellers freely;
- Nexus is one of them, with a contractual right to resell at 15–25%;
- dropping this for a Nexus-only link would recreate the Tuya/TTLock lock-in.

So the openness has to be **in the API**, and Cloudflare is only the road to it.

- **Endpoints** (any reseller, the same for all):
  - availability;
  - hold;
  - confirm (with payment reference);
  - cancel;
  - booking status;
  - **request guest key**.
- **Resellers are authorised by the hotel.** Each reseller gets its own credential, created and
  revocable by hotel staff in ozlodge. Nexus is not special in the code. Its commission is a
  contract term.
- **The hotel issues every guest key itself.** On confirm, ozlodge, **through its `issuer` bond**
  (§3), issues a member invite valid **only for the booking window** (check-in to check-out). The reseller just
  delivers it to the guest's app (BANOI/AURA or the reseller's own).
- 🟡 **Spec extension needed (ozkey-48 §3.3):** bind the invite to the **guest app's public key**,
  given by the reseller at booking. Then an invite intercepted on the reseller's path can't be
  redeemed by anyone else. Today OZINV is redeemable by whoever presents it first.
- MAVEN (the hotel's PMS) stays the system of record for rooms and bookings. ozlodge syncs from it,
  as `/pms/rooms` does today, and answers availability from that data.

## 5. Cloudflare rules

1. **The hotel owns the Cloudflare account and the tunnel.** If eBizco held them, eBizco would be
   the gatekeeper the whole design avoids.
2. **Cloudflare Tunnel only:** outbound from the hotel, with no open inbound ports. Put Cloudflare
   Access in front, with **a separate service token per reseller**.
3. **Cloudflare decrypts TLS at its edge, so it can read the booking data that passes through.**
   Lock traffic is OZKIE-sealed and safe. Guest names, dates and payment references are not.
   Choose one:
   - (a) the booking API encrypts its payloads end to end to ozlodge's key, so Cloudflare sees
     only sealed bodies. **Recommended**, as it keeps ozlodge v2.4's "guest records stay in hotel
     custody" promise true.
   - (b) accept it, and state in the hotel contract and spec that Cloudflare processes booking metadata.
4. **Replaceable:** nothing in ozlodge depends on Cloudflare. Any reverse tunnel works (WireGuard to
   a VPS, frp), so the hotel can switch without our help.
5. 🔴 **Fail closed.** Today ozlodge's PMS API is **open when `OZLODGE_PMS_SECRET` is not set**
   (`guardPmsSecret`, server.js:805-806, "lab: open"). Behind a public tunnel that means anyone on
   the internet. In production the server must **refuse to start** without credentials. That needs
   fixing before any tunnel exists.

## 6. Scale: the mining-camp target (10,000 locks, 4 servers)

The server memory sets the target:
- 4 servers: **10 Node instances split across 2 app hosts**, plus a separate MySQL primary/replica pair;
- firmware's measured relay ceiling is about 440 requests/s per path (XF-154 session).

The core should be built for that from the start:

- **Each lock is owned by one worker at a time.**
  - Ownership is a lease row in MySQL, taken per bridge, since locks are reached through bridges.
  - The worker that owns a bridge subscribes to that bridge's topics explicitly.
  - On failover the lease moves.
  - This also gives ozkey-48 §4.1's **"one context = one active sender"** for free.
- **Fencing** (server review #5a): bridge failover (XF-144) moves a lock to another bridge, and so
  possibly to another worker, mid-flight.
  - Every lease carries an increasing **fencing epoch**.
  - Every OSCORE seq-block reservation (ozkey-48 B.1.1) records the epoch it was taken under.
  - A worker whose lease is gone can't send from its old block, because a send checks the current
    epoch first.
- **State that must survive a worker failover moves to MySQL**: `lastGrantSentByDevice`, OSCORE
  contexts and seq blocks, and the queue.
- **Per-device ordering stays in process** (server review #5b): `flushQueueQueues` and
  `bridgePresenceQueues` are correct as in-process promise chains under single-owner leases.
  No Redis.
- The details are the server developer's design. This doc only fixes the rule.

## 7. Migration plan: small steps, v1 locks keep working throughout

| Step | What | Exit test |
|---|---|---|
| S−1 | **Fail-closed secrets, all three servers** (§8.1–8.2). Small and independent, so it goes first | each server refuses to start in production without real secrets |
| S0 | **First write an API-level regression suite against today's ozlock** (none exists: no tests and no test script in any `package.json`). Start from the server's harness: a copy of server.js, a local mosquitto on a spare port and a throwaway DB. Then extract the shared helpers and the ozlock lock-side into the core. **ozlock behaviour unchanged** | the new suite passes before and after. A bench lock pairs, unlocks and revokes as before |
| S1 | Rebuild **ozpms** on the core. It gains presence, failover and the August fixes, and drops server-side Tuya frames and the unsealed unlock (§8.3). **Topic move:** the server subscribes to both `ozkey/pms/…` and `ozkie/<site>/…`, bridges are reconfigured, then the old scheme is dropped (as ozlock's S16 did) | ozpms drives a bench lock through a bridge, including bridge failover |
| S2 | Rebuild **ozlodge**'s lock side on the core, with the same dual-subscribe topic move for `hotel/…` | ozlodge rooms/PMS flows unchanged |
| S3 | **OZKIE v2 relay, written once in the core** (ozkey-48 §7) | interop against firmware's host test lock (ozkey-49 P2) |
| S4 | Worker ownership leases (§6) | 2 hosts × N workers, failover of a worker without lost or duplicated commands |
| S5 | Profile features: lodge booking API (§4); pms tenancy (ozkey-51) | per-feature tests |

S0–S2 are pure refactoring and can start now. S3 waits for firmware's host test lock.

**Status 2026-10-19 (server):**
- S−1 committed `2cf04fb`. S0a committed `df820d0`. S0a.1 committed `1788e67`.
- S0b committed `392f56e`. Verified by `npm test` 44/44, reproduced by the firmware lead, plus line accounting: 68 moved bodies byte-identical, 3 intended line edits.
- **S0b bench check PARTIAL** (2026-10-19 12:14–12:15Z, ozlock :4400 from `392f56e`; Bridge 1 ozb-98a316a7e638 + LockB ozk-acebe63acab8; firmware matched the capture against both serial logs):
  - ✅ Boot, S−1, XF-144 reconcile-on-connect (LockB moved from dead 206ef116f390 to Bridge 1 with no PATCH), presence → online/ok, retained time + bridge utc push.
  - ✅ Failover sweep sent `pair_lock` for LockA and ozk-b0a6048b5fd8 to Bridge 1. Bridge logged "will connect on the next scan" (no bond change; both locks powered off). Lesson: before booting against the live lab DB, list which sweeps will fire.
  - ✅ unlock / grants without `envelope_hex` → 400 (XF-120 §2, S4 cutover), correct fail-closed.
  - ⬜ Sealed unlock/grant delivery UNVERIFIED: a v1 envelope needs the owner's bond key from BANOI; ozapp.py is v2 only. Needs BANOI at the bench (firmware raising with PM/ftpos).
  - ⬜ scan-bridges: Bridge 1 answered found:false. Open question, not a fault: LockB was already BLE-connected, and a connected peripheral normally stops advertising.
  - The Thread child removed at 23:15:00+11 (ext 6e478289f2405941) was a stale entry aging out at the 240 s child timeout, NOT LockB (LockB is on BLE, thread=down).
  - 🟢 CLOSED: the 11 stale test bridges (ozb-fx*/gt*/pd*, ozk-racetest*) that read "online" and drew scan commands were deleted from the ozlock lab DB by firmware on the operator's direct approval: bridges 29 → 18, bridges_presence 28 → 17, lock_bridges 26 → 20. Only Bridge 1 reads online now. 16 other August test rows remain; leave them unless they show up in a scan.
- S1a committed `422da45` (core prep: lazy traps, profile CONFIG keys, opt-in grants plaintext drop, core package.json + README). Next S1b: lock REST routes into the core as `mountLockApi`, which refuses to mount without an auth hook.
- S1b committed `229d468`. S1c committed `35c90ab` (ozpms on the core; open API only with `OZPMS_LAB_OPEN_API=1`).

**Status 2026-10-29 (server): S1d BUILT, uncommitted, for firmware to reproduce**
- What it adds:
  - `ozserver-core/testkit/harness.js`, the shared harness. `configure(profile)` sets it up per profile, and `ozlock/server/test/harness.js` is now a thin wrapper around it.
  - ozpms suite: `ozpms/server/test/{harness,refuse,ozpms,failover}.test.js` plus golden files `ozpms.json` and `failover.json`.
  - `*_LOCK_RESET_TIMEOUT_MS` / `*_BRIDGE_RESET_TIMEOUT_MS` overrides in both profiles. The defaults stay 5000. The harness uses 20000/10000, which fixes the load-sensitive sealed-reset verdict flake.
- Two flakes found and fixed after the stand-down:
  - The golden file pinned the harness's ephemeral broker port. ozpms provisions `broker_tcp_port` from its own `MQTT_URL`; ozlock hardcodes 1883. The testkit now writes `<harness-broker-port>` only when the value equals the private broker's port, so ozlock's golden is unchanged.
  - The legacy door-log ordering. The log orders by arrival (`id`), and the two roots are separate topics with no delivery order between them. The test now waits for the first row before it publishes the second. No server change.
- Results:
  - `cd ozpms/server && npm test`: 23/23 on 5 compare runs, after a re-pin with `OZPMS_GOLDEN_UPDATE=1`.
  - `cd ozlock/server && npm test`: 44/44 ×3, golden unchanged.
  - `cd ozserver-core && npm test`: 6/6.
  - The pre-stand-down "DELETE a bridged lock with a sealed reset" failure (once in 23 tests) did not recur in 6 full runs.
- The commit pathspec, on the operator's go: `ozserver-core/testkit/ ozlock/server/{server.js,test/harness.js} ozpms/server/{server.js,package.json,test/}`.
- Next: S1e (ozlock `HASH_LOCK_BROKER_SECRETS`, plaintext bridge creds, MySQL/Node TZ skew, label-less re-register overwrite).

## 8. Security items to fix along the way

1. 🔴 ozlodge PMS API is open without a secret (§5.5). Fix before any tunnel.
2. 🔴 **Hardcoded secrets in all three servers** (server review #3):
   - the DB root password: ozlock :166, ozlodge :69, ozpms :81;
   - ozlock's JWT secret falls back to a hardcoded `lab-only-INSECURE` string (:126);
   - ozpms keeps `broker_secret` in plaintext in `locks`.

   All of them should fail closed like §5.5: refuse to start in production without real values.
   Secrets come from env or a 0600 file, never the repo; bridge secrets are hashed or encrypted at rest.
3. 🔴 **ozpms is still pre-XF-120** (server review #4):
   - it builds an **unsealed** unlock frame (`buildUnlockFrame`, used at :1191);
   - it receives raw PIN/RFID values (`rawValue`, :1097), so the server sees plaintext credentials;
   - its API has no authentication.

   **ozpms must not go on a customer VPS before S1.**

   **Firmware answer to "does v1 firmware reject an unsealed unlock?": NO, not in general**
   (checked 2026-10-18, `ozdoorlock_core.h` + `ozprofile_gen.h`).
   - **The legacy `payload` / `payload_hex` field is still accepted** on MQTT and on the
     bridge relay (ozkey-13 F6, "drop legacy payload_hex", was never done). The only gate is
     the profile's DP allow-list, `ozDpForwardable()`.
   - **ozpms's own frame is rejected by luck:** it writes DP 1, which DS013-T3 doesn't have and
     the default `tuya-wifi-lock-pro` profile marks report-only.
   - **On the DS013-T3 profile (the real supplier lock), DP 76 `unlock_ble` and DP 72
     `unlock_remote` are CONFIRMED and downstream-capable.** An unsealed frame on one of them is
     forwarded to the MCU.
   - On an anon-open broker, anyone who can publish can send one. **Firmware fix:** refuse every
     unlock-verb DP on the unauthenticated legacy path. Unlock arrives only sealed (`envelope_hex`
     / `control`). This is a v1-only issue: v2 (ozkey-48) has no unsealed path.
4. ozlodge/ozpms build credential frames server-side. These are deleted in S1/S2 (§2).

## 8a. Where lock management lives (🟢 operator ruling 2026-10-19)

The Nexus PM asked; the operator approved firmware's recommendation:

- **Nexus = registry only:** device models, public keys by MAC, the owner's lock inventory, and
  later the device-certificate revocation list (ozkey-52). Never live lock control. The Nexus
  /admin "Door Lock" page shrinks to that registry work.
- **Platform team:** live fleet operations (bridges, presence, write queues, firmware mix) live in
  the OZLOCK ops dashboard. It links to the Nexus registry and does not duplicate it.
- **Hotel owners:** manage their locks in OZLODGE on the hotel's own server, which keeps working
  offline and keeps Nexus as one reseller among several (§4). **Landlords:** OZPMS on their own VPS.
- Dashboard mockup: ACCEPTED by the operator 2026-10-19 (https://claude.ai/artifact/Y9v6FQpajwYZQdRCcPgru4). v2 adds the fleet map and the language switcher (ozkey-54): https://claude.ai/artifact/Hds4AXWYtycfXB9hLLy48z

## 9. Questions for the operator

1. **Approve the direction** (one core, three profiles, S0–S5)? If yes, tell the server developer
   in their own session.
2. **Cloudflare payloads:** (a) encrypt booking data end to end (recommended), or (b) disclose that
   Cloudflare can read it?
3. **Guest-key binding:** extend OZINV so an invite is bound to the guest app's key (§4)? Recommended.
4. **Who approves a new reseller:** only hotel staff in ozlodge (recommended), or can Nexus onboard
   resellers on the hotel's behalf?
5. **`issuer` role** (§3): approve the ozkey-48 §6 change, so hotel servers mint bounded guest
   invites and never hold bond #0? Recommended. If you do want **the server itself** to unlock hotel
   rooms remotely, that has to be your explicit decision, recorded in ozkey-48. The table no longer
   implies it.
6. **v1 unsealed-unlock hole** (§8.3): two fixes, recommended **both**:
   - (a) **Broker ACL now** (server's suggestion). This doesn't wait for firmware. Only the servers'
     broker users may publish to `+/+/locks/+/command` and `+/+/bridges/+/command`; anonymous
     clients can't. You decide who owns the mosquitto config.
   - (b) **Firmware fix:** refuse every unlock-verb DP on the legacy path, in the ESP32 v1 builds.
     That's a small change, then a flash and a bench test. It still matters with the ACL in place,
     because a server with broker credentials (ozpms, before S1) can still publish an unsealed frame.

   AK1037 v2 doesn't have this path at all.
7. **Do ozpms locks get registered in Nexus?** (raised by the server developer 2026-10-19, S1). ozlock tells
   Nexus to tombstone a lock's MAC on `DELETE /locks/:id`. ozpms is the operator's own server and §8a
   makes Nexus a registry only, so the S1 default is **off**: ozpms has no Nexus hook. If ozpms locks
   should appear in the Nexus registry (public keys, inventory), say so and it becomes a separate item.
