# OZKEY-05 — OZLOCK Cloud Rendezvous Service Design (Market A)

> **Lab implementation landed 2026-07-07:** `ozlockserv/` (directory, :4200,
> MySQL db `ozlock`) + `ozlock/` (personal keyring front end, :4300) +
> LockSim **Mode C** (paste the provision payload into SERVER PUSH to enroll).
> Implemented: §5 topics, §6.1 enrollment (single-use tokens, ack/nack),
> §6.2 grants + revoke, §6.3 queue `expires_at`, §6.4 log ingestion (no push
> yet). Deferred: owner auth/JWT (§4), broker ACL enforcement, TLS, cloud
> deploy (§10 steps 3-5). E2E-verified over the live lab broker.
>
> **Trust-model v2 amendment (2026-07-07, post-draft):** operator decision —
> OZLOCK authenticates **neither doorlock nor app**. The app self-generates
> its `app_id` (keypair), grants the lock its `device_id` at the BLE
> ceremony, and registers the pairing (`POST /pairings`); app ⇄ lock traffic
> is **end-to-end AES-256-GCM** keyed at the ceremony, so OZLOCK relays
> ciphertext it cannot read or forge. This supersedes §4's owner-JWT
> federation and §6.1's token *authorization* role, and moves DPID frame
> building into the app's keyring core. Full contract + open log/push
> question: FTPOS `XFtposDecisions-42.md` §13; formal spec to follow as
> **ozkey-06**. The §1.3 split sharpens: market C (OZKEYSERV) authenticates
> and is the system of record; market A (OZLOCK) is a blind registry+relay.
>
> **`/pairings` inversion landed 2026-07-08:** `POST /pairings {app_id,
> device_id, label}` registers the bond — unauthenticated, first-writer-wins
> on the random device_id (a different app claiming a live device_id → 409;
> the same app is idempotent). Enrollment is now **token-free**: the lock's
> first broker contact matches a pre-registered pairing by device_id (the
> bearer handle); an unregistered device_id gets `enrollment_nack`.
> `enroll_tokens` retired; `/enroll/begin` kept as a deprecated shim. REST
> mounted at both `/ozlock/api` and `/ozlockserv/api`. E2E-verified over the
> live broker (register → status `registered` → lock contact → `enrolled`,
> squatting guard, orphan nack, grant/DPID pipeline intact). **Still not
> §13-pure:** frame building remains server-side + plaintext; the keyring-core
> move + AEAD envelope are ozkey-06 (LockSim first, then firmware).
>
> **DRAFT 2026-07-07.** Server-side design for **OZLOCK** — the common free
> cloud service that pairs a residential doorlock to its owner's BANOI app
> across any two networks (VN ⇄ AUS). This is the "STUN server" of the
> product story: like STUN it exists purely to *introduce two parties that
> cannot reach each other directly* and stays out of the data they exchange
> as much as possible. Mechanically it is **not** literal STUN (§1.2) — it
> is a hardened MQTT broker + a thin directory service, running the same
> codebase as the on-prem market-C server (ozkey-04 §10). Consumers:
> OZKEYSERV maintainers (this repo), BANOI team, firmware team.
> Depends on: ozkey-04 (commissioning, identity, topics), ozkey-02 (frame
> codec + queue semantics, both unchanged).

---

## 1. Role and shape

### 1.1 What OZLOCK is

```
   BANOI (AUS, behind NAT)                          Lock (VN, behind NAT)
        │                                                 │
        │ HTTPS (REST, JWT)                MQTT/TLS 8883  │ (wakes, dials out)
        ▼                                                 ▼
  ┌─────────────────────────── OZLOCK ────────────────────────────┐
  │  Directory service (Node, = ozkeyserv evolved)                │
  │    owners · sites · locks · grants · queue · logs · enroll    │
  │  MQTT broker (EMQX) — per-device auth/ACL, WSS for sims       │
  │  MySQL                                                        │
  └───────────────────────────────────────────────────────────────┘
```

Both parties **dial out** to OZLOCK, so NAT is never traversed — it is
side-stepped. The lock's outbound-only, wake-and-dial behaviour is exactly
the lab's queue-and-heartbeat model (ozkey-02), unchanged.

### 1.2 Why not literal STUN/TURN

STUN discovers your public endpoint so two live peers can hole-punch a
direct path. Our lock is not a live peer: it is a 7 µA deep-sleep device
whose radio is off except during wake windows (LockSim models this
faithfully). There is no socket to hole-punch *to*. Every commercial system
with this device class (Tuya, TTLock, Nabto) converges on the same answer:
a store-and-forward rendezvous the device dials into. OZLOCK keeps the
*intent* of STUN — minimal, introduction-only, free — and drops the
mechanism that cannot work. If a future SKU is mains-powered and always-on,
a P2P upgrade path can be revisited; nothing below precludes it.

### 1.3 One codebase, two deployments (restated from ozkey-04)

| | OZLOCK cloud | Market-C on-prem |
|---|---|---|
| Tenancy | multi-tenant (`site_id` per household) | single tenant (`site_id` fixed) |
| Broker | EMQX, TLS 8883 / WSS 443, HTTP auth hook | Mosquitto on LAN (lab today) |
| Accounts | BANOI-federated owners (§4) | site-admin accounts |
| Exposure | public internet | LAN/VPN |

Feature code (queue engine, DPID codec, flush-on-heartbeat, lock_logs,
grants) is shared. Deployment differences are config + the auth/ACL modules.

## 2. Components

| Component | Choice | Why |
|---|---|---|
| Broker | **EMQX** (open source) | HTTP auth/ACL hooks against the directory, 100k+ conns/node, WSS built in. Lab keeps Mosquitto; contract is broker-agnostic |
| Directory | **Node/Express — the ozkeyserv codebase** | The lab already implements the hard parts (queue, flush, codec, logs); this doc adds tenancy + auth around them |
| DB | MySQL 8 | Continuity with the lab schema; additive migrations |
| Push | FCM + APNs via the directory | Door events → owner's phone (§6.4) |
| TLS | Let's Encrypt, terminated at broker & API | No plaintext off-LAN (ozkey-04 §8.4) |

Single `docker-compose` on one VPS is the v1 deployment (§8).

## 3. Data model (additive to the lab schema)

```
owners        id · banoi_sub (federated subject) · display_name · created_at
sites         id (site_id) · owner_id · label ("Nhà Q7") · region · created_at
locks         id (device_id) · site_id · pubkey · mac_label · fw · power_profile
              · heartbeat_s · last_seen_at · broker_username · status
enroll_tokens token · site_id · owner_id · expires_at · used_at (single-use)
grants        = lab `credentials` + site_id + issued_by (owner|ivr|api)
              · pin/rfid value encrypted at rest (§7.4)
pending_queue = lab table + site_id + expires_at (§6.3)
lock_logs     = lab table (2026-07-07) + site_id
```

The lab's `rooms` table survives only in market C (rooms are decoration on
locks there); market A has no rooms — `sites.label` + `locks.mac_label`
cover the human naming. This is the ozkey-04 §9 room-agnostic principle
applied to storage.

## 4. Identity & auth (three principals)

| Principal | Authenticates by | Gets |
|---|---|---|
| **Owner** (BANOI user) | BANOI-issued JWT (federated: OZLOCK trusts BANOI's signing key; no second password) | REST scope over their own sites/locks only |
| **Device** (lock) | MQTT `username = device_id`, `password = 32-byte secret` minted at enrollment (v2 option: mTLS client cert aligned with Matter DAC) | Broker ACL: `ozkey/<site>/locks/<device_id>/#` **only** — publish and subscribe both fenced |
| **Operator** (us) | admin JWT, separate audience | fleet metrics, no grant values |

EMQX asks the directory (`POST /broker/auth`, `POST /broker/acl`) on
CONNECT/SUBSCRIBE/PUBLISH; the directory answers from `locks.broker_username`
+ the ACL pattern. Revoking a device credential (owner release, theft)
takes effect at next CONNECT and kills the live session via EMQX API.

## 5. Topic namespace

Exactly ozkey-04 §9, cloud-instantiated:

```
ozkey/<site_id>/locks/<device_id>/enroll      lock → dir   (once, §6.1)
ozkey/<site_id>/locks/<device_id>/heartbeat   lock → dir   (timer/touch wake)
ozkey/<site_id>/locks/<device_id>/command     dir  → lock  (queued envelopes)
ozkey/<site_id>/locks/<device_id>/log         lock → dir   (door transactions)
```

Envelope and `payload_hex` DPID frames are byte-identical to ozkey-02 §4 —
the codec verified against LockSim carries over untouched.

## 6. Flows

### 6.1 Enrollment

Server side of ozkey-04 §6: `POST /enroll/begin` (owner JWT) mints the
single-use token + pre-creates the lock row; the lock's first
`…/enroll` publish carries `{device_id, pubkey, mac, enrollment_token, fw}`;
directory verifies token TTL + single-use, stores pubkey, mints the broker
secret, publishes `enrollment_ack {broker_username, broker_secret}` on the
command topic (v1 plaintext — bench only; v2 wraps in the ozkey-04 §8 ECDH
session), burns the token. Lock reconnects with its own credentials;
factory/anon credentials are refused everywhere else.

### 6.2 Grant issuance (the common path — latency-tolerant)

BANOI "cấp mã cho thợ điện đến 5 giờ" → IVR intent → 
`POST /locks/<id>/grants {type: pin, value, date_to: 17:00}` → directory
builds the DPID 21/23 frame (lab code as-is), rows into `grants` +
`pending_queue`, delivered on the lock's next wake, `synced` ack per lab
semantics. Worst-case latency = `heartbeat_s` — fine for grants that start
in the future. Revoke is the same shape via DPID 22/24 (gap #8 endpoint,
already live in the lab).

### 6.3 Remote unlock (the rare path — latency-critical and expiring)

Remote unlock MUST NOT fire stale: an unlock arriving 8 minutes after the
owner gave up is an open door with nobody there. Therefore:

- every queued command row carries `expires_at`; the flush skips + marks
  `expired` rows past due (lab queue engine gains one column + one check);
- `POST /locks/<id>/unlock` requires a fresh biometric-confirmed BANOI
  session (ozkey-04 §10 — IVR may initiate, never authorize), sets
  `expires_at = now + 60 s`, and **responds with the lock's reachability**
  so the app can be honest:

| `power_profile` | Radio behaviour | Remote-unlock UX |
|---|---|---|
| `eco` (default) | deep sleep; wakes on touch or `heartbeat_s` timer | API replies `queued, next contact ≈ T-…s` — app offers "wait" or cancel; command expires if missed |
| `responsive` | Wi-Fi modem-sleep, MQTT session alive (keepalive 60 s) | delivered in ~1–3 s; battery cost: weeks–months not a year — surfaced as a battery-slider in BANOI |
| `scheduled` | `responsive` during owner-set hours, `eco` otherwise | predictable compromise (e.g. responsive 07:00–23:00) |

The profile is a lock setting (mirrors LockSim's System Settings
`heartbeatSeconds`, 2026-07-07) and a `grants`-style queued config write.

### 6.4 Usage log → owner notification

`…/log` ingestion is the lab pipeline (2026-07-07) plus fan-out: directory
matches `site_id → owner`, applies per-owner notification prefs
(granted/denied/all/none, quiet hours), pushes via FCM/APNs. BANOI's
doorlock screen reads `GET /locks/<id>/log` — same shape the cockpit's
DOORLOCK LOG tab consumes today.

## 7. Security posture

1. **Transport:** TLS 8883 (locks), WSS 443 (sims), HTTPS 443 (apps). No
   plaintext listener exists in the cloud deployment.
2. **Blast-radius:** per-device broker credentials + ACL (§4) — a
   compromised lock reaches only its own four topics; a leaked owner JWT
   reaches only that owner's sites.
3. **Command authenticity (v2 gate, ozkey-04 §8.3):** envelopes signed by
   the directory key + monotonic counter per lock; the lock verifies before
   acting. Until v2, the broker is trusted infrastructure — acceptable for
   bench, flagged for doors.
4. **Secrets at rest:** grant values (PINs, UIDs) encrypted at rest
   (AES-GCM, key outside the DB); they must be recoverable plaintext to
   build DPID frames, so hashing is not an option — encryption + narrow
   read path is.
5. **Abuse controls:** rate limits on `/enroll/begin` and `/unlock`;
   enrollment tokens single-use TTL 10 min; audit rows for every grant,
   revoke, unlock with the authorizing principal.

## 8. Deployment, scale, cost (v1 honesty)

Per-lock traffic is tiny: one ~200 B heartbeat per `heartbeat_s` (default
600 s) + rare logs/commands ≈ **0.002 msg/s per eco lock**.

| Fleet | Msg rate | Concurrent conns (responsive share ~20%) | Box |
|---|---|---|---|
| 1 k locks | ~2 msg/s | ~200 | 2 vCPU / 4 GB VPS runs everything |
| 50 k | ~90 msg/s | ~10 k | 4 vCPU / 8 GB, still one node |
| 500 k | ~900 msg/s | ~100 k | split broker/API/DB; EMQX single node still holds the conns |

Region: one deployment in **Singapore** serves VN (~30–70 ms) and AUS
(~90–120 ms) — irrelevant next to queue semantics. Multi-region (broker
bridge) is a scale problem to be earned, not designed now. Backups: MySQL
daily snapshot + binlog; broker is stateless (queue truth lives in MySQL —
the lab's server-authoritative-queue decision pays off here: commands are
cancellable/expirable because the DB, not broker retention, is the source
of truth).

**Free-tier economics:** at 50 k locks on one ~US$40/mo VPS, marginal cost
per lock is well under $0.01/yr. Free tier = relay + grants + 30-day log
retention. Paid (later, if ever): long retention, shared owners/family
roles, integrations. Market C never touches this bill (on-prem).

## 9. REST surface (owner JWT unless noted)

| Endpoint | Method | Notes |
|---|---|---|
| `/enroll/begin` | POST | mint token for a site (§6.1) |
| `/enroll/status?token=` | GET | commissioning progress for the app |
| `/sites`, `/sites/<id>/locks` | GET/POST | CRUD, owner-scoped |
| `/locks/<id>/grants` | GET/POST | issue = lab `/pms/issue-key` re-skinned; no `room_no`, takes `device_id` |
| `/locks/<id>/grants/<gid>` | DELETE | revoke = lab `/pms/revoke-key` (gap #8) |
| `/locks/<id>/unlock` | POST | §6.3; biometric-fresh JWT claim required |
| `/locks/<id>/log` | GET | lab `/locks/log` + tenancy filter |
| `/locks/<id>/settings` | PATCH | `power_profile`, `heartbeat_s`, label |
| `/broker/auth`, `/broker/acl` | POST | EMQX hooks, broker-internal secret (no JWT) |
| `/health` | GET | unauthenticated, shallow |

Existing lab endpoints keep working in the on-prem deployment during
migration; the cockpit stays pointed at them.

## 10. Migration path from the lab (ordered)

1. **MAC/device-scoped topic refactor** (ozkey-04 §9 step 1) — drop
   `room_no` from lock-facing payloads; `site = "lab"`. LockSim + server.
2. **Queue `expires_at`** + flush check (§6.3) — small, testable in the lab.
3. **Tenancy + REST auth** — `site_id` columns, owner JWT middleware
   (single hardcoded owner in the lab deployment).
4. **Enrollment endpoints + broker auth hooks** — Mosquitto dynamic-security
   locally to prove the ACL contract before touching EMQX.
5. **Cloud deployment** — docker-compose (EMQX + directory + MySQL), TLS,
   Singapore VPS; LockSim in `ozkey-cloud` mode over WSS is the first
   "device" enrolled from another network.
6. **BANOI integration** against §9 endpoints; ozkey-04 §8 v2 envelope
   before any physical door.

## 11. Acceptance checklist

1. A LockSim on a network outside the VPS (different NAT) enrolls with a
   token from `POST /enroll/begin` and reaches ENROLLED without any inbound
   port on either side.
2. Lock A's broker credentials cannot subscribe/publish lock B's topics
   (EMQX ACL test) — and a revoked credential is refused at CONNECT and its
   live session is killed within 5 s.
3. Grant issued from a BANOI-JWT REST call while the lock sleeps is
   delivered on next heartbeat and acked `synced`; a revoke queued behind
   it is delivered in the same flush, in order.
4. Remote unlock against an `eco` lock returns the honest
   `next contact ≈` estimate; letting it lapse marks the queue row
   `expired` and the door never opens late.
5. Door event on the lock → FCM/APNs push on the owner's phone < 5 s
   (responsive) / next wake (eco), and appears in `GET /locks/<id>/log`.
6. Owner release: BANOI loses the lock, its credentials die, factory-reset
   lock re-enrolls under a different owner with a fresh `device_id` secret.
7. The identical directory codebase boots in on-prem mode with auth
   hardcoded to one site and Mosquitto — the market-C bench (this repo's
   lab) still passes its ozkey-02 checklist unmodified.

## 12. Open questions

1. **BANOI JWT federation (§4):** does BANOI already issue verifiable JWTs
   (JWKS endpoint), or does OZLOCK need to run its own OAuth and BANOI
   embeds it? Determines whether `owners.banoi_sub` is trustable v1.
2. **`responsive` power numbers:** modem-sleep current on the C6 with our
   keepalive — firmware team to measure; drives the battery slider copy.
3. **EMQX vs Mosquitto in cloud:** EMQX assumed for auth hooks + conn
   scale; if ops simplicity wins early, Mosquitto + dynamic security can
   carry the first few thousand locks — decide at migration step 5.
4. **Log retention** free-tier number (30 days proposed) and whether
   `lock_logs` needs partitioning from day one (cheap now, annoying later).
5. **Anti-abuse on the free tier:** enrollment requires a BANOI account —
   is that enough gating, or do we bind enrollment count per account?
