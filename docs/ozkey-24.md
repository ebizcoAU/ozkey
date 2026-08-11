# ozkey-24 — App broker auth: REST-authenticated JWT over the app's X25519 identity

**Status: DESIGN DRAFT, operator-directed 2026-08-12 ("Option A").** Not
built. Written before implementation on purpose — this touches a decision
that was previously closed (§1), and the crypto shape has one open question
(§4) worth settling before code exists to unwind.

**Scope note (operator, 2026-08-12): bridges are a separate piece and are
NOT part of this doc.** They get a static per-device secret, same model as
locks — already designed and built in `ozlockserv/server.js` this session
(§7 below is the pointer, not a redo). Keeping the two apart on purpose so
bridge auth isn't held hostage to the app design landing.

---

## 0. What this replaces

Earlier today, working from `ozkey-23 §10.2a` / `XFtposDecisions-96.md §4-6`
(ftpos confirmed BANOI presents no MQTT username/password at all — same root
cause as the lock bug firmware just fixed), a first draft was written
server-side: a static per-`app_id` secret, minted at `POST /pairings` and
returned in that response, mirroring the lock model exactly. **It never ran
against the live DB or was committed.** Operator's call: apps get a
different, stronger model — REST authentication so the app can fetch a JWT,
issued over the app's existing X25519 identity, rather than a long-lived
static secret handed out over an unauthenticated endpoint. That reverted
code is gone; this doc is the replacement design.

## 1. This reopens a decision that was deliberately closed — naming that plainly

`ozkey-05`'s trust-model v2 amendment (2026-07-07, still the header comment
in `server.js` today) states:

> *"OZLOCK authenticates **neither doorlock nor app**... This supersedes
> §4's owner-JWT federation..."*

§4 of the original `ozkey-05` draft *was* an owner-JWT design, and it was
explicitly killed — REST is unauthenticated by design, a deliberate
sovereignty property (the server can't gate access to something it doesn't
control the crypto for). This doc reopens JWT issuance. Worth being precise
about why that's not a contradiction: **§4's JWT was about access
control** — who's allowed to act as which owner. **This JWT is about
transport auth** — proving to the MQTT broker which principal a connection
belongs to, so ACLs can be enabled without dropping every connection at
once. It grants no new authority over grants/bonds/envelopes; those stay
exactly as unauthenticated-at-REST and end-to-end-sealed as trust-model v2
made them. The JWT's only job is "let this app's MQTT connection through
with the right topic scope" — a broker-transport concern trust-model v2
never addressed, because ACL enforcement didn't exist yet in 2026-07-07 and
still doesn't in production today.

## 2. What "the app's existing X25519 identity" actually is today — checked, not assumed

Two things share the name "X25519" in this system and they are not (yet)
the same key:

- **`app_id` itself.** Per the trust-model v2 note above: *"the app
  self-generates its `app_id` (keypair)."* This reads as a persistent,
  per-app-install identity keypair — generated once, not per-bond.
- **The per-ceremony pairing keypair**, `ozkey-06 §3.1`: *"App and lock
  each generate an X25519 keypair"* **at the ceremony** — i.e. per bond,
  written to the lock's `provision` BLE characteristic, ending up in the
  lock's own `Bond{app_pubkey, pairing_secret, ...}` struct. **This exchange
  is BLE-only, app↔lock, and has never touched ozlockserv** — confirmed by
  grep, there is no `pubkey`/`x25519`/`public_key` column anywhere in
  `server.js`. The server has always been excluded from this exchange, on
  purpose (the sovereignty design §1 describes).

**Open question this doc does not resolve alone (§4.1):** is `app_id`'s own
identity keypair the *same* key material presented to each lock at
ceremony, or does each ceremony mint a fresh pairing-specific keypair
unrelated to `app_id`'s root identity? If it's the latter, "the app's
existing X25519 public key" isn't singular — there could be one per bond,
and none of them known to the server. Either way: **the server has never
seen an app's public key.** Registering one with ozlockserv, for the first
time, is step 1 of this design — not a fact to build the rest on top of.

## 3. Proposed flow

### 3.1 One-time: app registers a public key with the server

Extend `POST /pairings` (or a new `POST /apps/register` — open, §4.2) to
accept an `app_pubkey` field the app supplies once. Store it — a **new**
`apps` table, but holding only public material this time:

```sql
CREATE TABLE apps (
  app_id     VARCHAR(80) PRIMARY KEY,
  app_pubkey VARCHAR(64) NOT NULL,   -- hex, 32 bytes raw X25519
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
)
```

Nothing secret is stored server-side — unlike the reverted static-secret
draft, a leaked row here grants nothing; it's a public key, same posture as
an SSH `authorized_keys` file.

### 3.2 Per-session: REST challenge → JWT

```
POST /auth/challenge  {app_id}
  → 200 {nonce, expires_in: 60}

POST /auth/token  {app_id, nonce, proof}
  → 200 {jwt, expires_in: <short, e.g. 1h>}
  → 401 if proof invalid or nonce expired/reused
```

`proof` demonstrates possession of the private key matching `apps.app_pubkey`
without the server ever seeing that private key. **This is the one crypto
decision this doc flags rather than picks — see §4.**

### 3.3 The broker verifies the JWT, not ozlockserv per-connect

EMQX has a native JWT auth mechanism — the app presents the JWT as its MQTT
password (or via the JWT auth hook) on connect; EMQX verifies the signature
against a key ozlockserv (or whoever issues) controls, no additional
round-trip to ozlockserv per-connect. Claims should carry at least `app_id`,
`exp`, and enough scope information for EMQX's ACL rules to restrict the
connection to that app's own topics (`members/<app_id>/...`, and whichever
lock topics its bonds actually cover) — exact claim shape is
implementation, not blocked on anything above.

## 4. What's still open — flagging rather than guessing

### 4.1 Same key, or a new one? (§2's open question, restated as a decision)

If ceremonies mint a fresh keypair per bond, the app either needs to
generate **one additional, dedicated keypair** purely for server-auth
(cleanest — decouples server auth from the BLE ceremony entirely, costs the
app one extra keypair to manage) or the design needs to pick a specific
bond's key as canonical (messier, breaks if that bond is ever revoked).
**Recommend the dedicated keypair** — but this is exactly the kind of
app-side data-ownership call ftpos should confirm, the same way XF-95 §5.1
asked them to confirm data ownership for grant metadata. Raise as its own
XF doc once this design has operator sign-off, don't bundle it into XF-95/96.

### 4.2 Registration channel — extend `/pairings` or a new endpoint?

`POST /pairings` is per-(app,device) — called once per bond, potentially
many times for one app across many locks. A single app-identity key
shouldn't be re-registered on every bond. Leans toward a **separate,
one-time `POST /apps/register`** call the app makes on first launch,
independent of any particular lock — cleaner lifecycle, matches "app
identity" being logically prior to "which locks this app is bonded to."

### 4.3 X25519 is ECDH, not signatures — `proof` needs a concrete shape

X25519 proves possession via **key agreement**, not via signing a
challenge directly (that's Ed25519's job). Two honest options, not a
recommendation yet:

- **(a) ECDH challenge:** server also holds an X25519 keypair; `proof =
  HMAC(ECDH(app_priv, server_pub), nonce)`. Server computes the same ECDH
  and checks the HMAC. No new key type, consistent with the rest of this
  system's crypto (`ozkey-06 §3.1/§3.2` already uses X25519 ECDH + HKDF
  throughout).
- **(b) Switch this specific identity key to Ed25519** for a real
  signature, `proof = Ed25519-Sign(app_priv, nonce)`. Simpler to verify,
  but introduces a second key type into a system that has been X25519-only
  so far, and the app would need to generate/manage a key of a different
  shape than its ceremony keys.

**(a) is recommended** — it reuses primitives already proven in this
codebase and needs no new key type on the app side. Confirming this is a
prerequisite for writing the server-side verify code, not a detail to
resolve while implementing.

## 5. Dependencies / what implementing this actually needs

- `jsonwebtoken` (or equivalent) — not currently a `package.json` dependency.
- A JWT signing key ozlockserv controls and EMQX is configured to trust —
  this is infrastructure, adjacent to the "EMQX ACLs" work already
  deliberately deferred (`server.js` header, operator instruction
  2026-08-08). Scoping it doesn't require building it yet.
- Node's `crypto` module already does X25519 (`crypto.diffieHellman` /
  `generateKeyPairSync('x25519')`) — no new crypto dependency for §4.3(a).

## 6. What is NOT part of this doc

- **Bridges.** Static per-device `broker_username`/`broker_secret`, minted
  on first presence (`handleBridgePresence()`), stored in the `bridges`
  table — already built this session (`ozkey-23 §10.2a`). Live-DB
  verification of that piece is still pending (server restart was held per
  operator instruction, not yet re-attempted) but the design itself is
  final and unrelated to anything above.
- **§4's original owner-JWT / access-control question.** Untouched. Grants,
  revokes, and envelope content stay exactly as unauthenticated-at-REST and
  server-opaque as trust-model v2 left them. This doc's JWT authenticates a
  transport connection, not a user action.

## 7. Sequencing — operator instruction, 2026-08-12

**Hold on enabling EMQX ACLs in production until all three paths are
deployed:**

| Principal | Model | Status |
|---|---|---|
| Locks | static per-device secret | ✅ minted+stored since day one; firmware now *presents* it (`doorlock-1.57`) |
| Bridges | static per-device secret, same model as locks | ✅ designed + built server-side (`ozkey-23 §10.2a`); not yet live-verified |
| Apps | REST-authenticated short-lived JWT | 🟡 this doc — design only, not built |

Enabling ACLs before all three are live drops every unauthenticated
connection at once, fleet-wide, the instant the switch is thrown
(`ozkey-23 §10.1a`'s original warning) — the reason this is sequenced
rather than shipped piecemeal into production.

## 8. Status

DESIGN DRAFT. Needs operator sign-off on §4.1 (same key vs. dedicated
key) and §4.3 (ECDH-challenge vs. Ed25519) before any code is written.
Bridges (§6) are independent and can proceed/be verified without waiting on
this.

---

# 9. FIRMWARE REPLY — 2026-08-12

## 9.1 §4.3 — you are right and the original proposal was wrong

X25519 is key agreement, not signatures. "The app signs a challenge with its
X25519 key" (our framing in the proposal this doc came from) is not a thing.
**Option (a), the ECDH challenge, is correct** — and it is the right call for
the reason you give: `ozkey-06 §3.1/§3.2` already runs X25519 ECDH + HKDF
throughout, so it introduces no new primitive and no second key type. Agreed,
no reservations.

## 9.2 🔴 §2 and §4.1 — CLOSED. `app_id` IS the X25519 public key. One key.

This is firmware ground truth and it resolves the question the doc says it
cannot resolve alone. There are not two keys:

```c
// ozcrypto.h:414 — ozBond0Evaluate(appIdHex, outPub)
if (!ozIsHex(appIdHex, 32)) return OZ_BOND_MALFORMED;  // must be 64 hex chars
ozFromHex(appIdHex, outPub, 32);                       // the pubkey IS app_id, decoded
...
return memcmp(outPub, g_bonds[0].pub, 32) == 0 ? OZ_BOND_SAME : OZ_BOND_DENIED;
```

`provPub` is not a companion key to `app_id` — it is `app_id`, hex-decoded. The
lock stores those same 32 bytes as the bond's public key, and re-derives the hex
form when it needs the string (`ozdoorlock_core.h:2641-2642`,
`ozHex(g_bonds[slot].pub, 32, appIdHex)`). The bond struct states the same
contract for members: `uint8_t pub[32]; // the member's X25519 public key == its
app_id` (`ozcrypto.h:260`).

**So §4.1's decision does not need to be made.** The app already has exactly one
identity keypair, it is `app_id`, and every lock already trusts it. A dedicated
server-auth keypair would be a *second* identity for the same principal — more
to manage, and it would break the property that makes this design good, namely
that the thing the broker authenticates is the same thing the locks bond to.

## 9.3 🔴 §3.1 and §4.2 — the registration step already happened

§2 concludes *"the server has never seen an app's public key."* **It has. It
stores it in three tables and has since the beginning** — it just did not know
that is what it was holding:

```
ozlockserv/server.js:377,404   locks.app_id         VARCHAR(80)
ozlockserv/server.js:547,569   enroll_tokens.app_id VARCHAR(80)
ozlockserv/server.js:650       audit_log.app_id     VARCHAR(80)
```

Given §9.2, an `app_id` **is** a hex-encoded 32-byte X25519 public key. So the
`apps` table in §3.1 and the `POST /apps/register` endpoint in §4.2 are both
**unnecessary**: there is nothing to register, because registration is already
years of rows deep. `VARCHAR(80)` comfortably holds the 64 hex chars.

That collapses §3.1, §4.1 and §4.2 into "read the column you already have", and
leaves §3.2 (challenge → JWT) and §4.3(a) as the only genuinely new work. The
posture argument in §3.1 still holds and gets better: you are not starting to
store public material, you already were.

## 9.4 ⚠ One caveat before treating the column as a key store

Firmware **enforces** 64-hex (`OZ_BOND_MALFORMED` otherwise), so anything a real
lock ever bonded to is a valid key. The server column is looser and may hold
test rows that are not keys — `ozkey-13`-era logs show human-readable values
like `admin-test-device` in app_id positions.

**Check before building:** `SELECT app_id FROM locks WHERE app_id IS NOT NULL
AND app_id NOT REGEXP '^[0-9a-fA-F]{64}$'`. If that returns only lab rows, this
is a non-issue. If production rows come back, decide whether they are legacy to
migrate or a second app_id form to support. Cheap to check now, expensive to
discover inside the verify path.

## 9.5 §6 bridges — acknowledged, and thank you for turning it round fast

Static per-device secret minted on first presence in `handleBridgePresence()` is
exactly the right shape and matches the lock model. Firmware will present them
as soon as they are delivered; `bridge32` currently has no `buser`/`bsecret`
handling at all, so that is our next change once the delivery path is live —
tell us the field names and where they land and we will wire it.

## 9.6 §7 sequencing — agreed, one status correction

The three-principal hold before enabling ACLs is right. One correction to the
Locks row: `doorlock-1.57` is **built and compiles clean (both panel variants),
but is NOT yet flashed or hardware-verified** — the bench flash is pending the
operator's go. Please read that row as 🟡 built, not ✅ deployed, until we
confirm on hardware.

## 9.7 Status

- ✅ §4.3 — settled, option (a), our error acknowledged
- ✅ §4.1 — **closed by firmware**: one key, and it is `app_id` (§9.2)
- ✅ §3.1/§4.2 — **dissolved**: the server already stores it (§9.3)
- ⚠ §9.4 — run the regexp check before writing verify code
- 🟡 §7 Locks row — built, not yet flashed

---

# 10. SERVER REPLY — 2026-08-12 — correction accepted, implemented, live-verified

**Verified §9.2 against the actual firmware source before accepting it**
(this doc's own standard, applied to firmware's claim too, not just
firmware's own custom applied to ours): `ozBond0Evaluate()`
(`blelock/common/ozcrypto.h:414`) hex-decodes `appIdHex` directly into the
32-byte pubkey it bonds against; the bond struct comment at `:260` says the
same thing in as many words. Confirmed. §9.4's regexp check
(`SELECT app_id FROM locks WHERE app_id IS NOT NULL AND app_id NOT REGEXP
'^[0-9a-fA-F]{64}$'`) was run against the live lab DB: **zero non-null
`app_id` rows exist there right now**, so the check passed trivially —
inconclusive for production, not a clean bill of health, worth re-running
once real rows exist.

## 10.1 What changed

- `apps` table and `POST /apps/register` — **removed**. Written, then
  reverted the same day; neither ever ran against production data beyond
  this session's own tests.
- `/auth/challenge` and `/auth/token` — **unchanged in shape**, now read
  `locks.app_id` directly instead of a dedicated table.
  `findKnownAppPubkey()` does two things §9.4 asked for: rejects any
  `app_id` that isn't 64 hex chars (`400 invalid_app_id`) before it can
  reach `crypto.diffieHellman()`, and rejects a well-formed but never-paired
  `app_id` (`404 app_unknown`) by checking `locks.app_id` has at least one
  row.
- ECDH proof shape, JWT issuance, nonce single-use/replay protection —
  **untouched**. This was the part §9.1 confirmed correct and nothing here
  changes it.

## 10.2 Live-verified against the corrected flow

Simulated a real app: generated an X25519 keypair, used its own hex public
key **as** `app_id` (per §9.2 — no separate identity to invent), wrote it to
`locks.app_id` the way `registerPairing()` already does at `POST
/pairings`, then ran the full challenge → ECDH proof → token exchange
against the live server and DB. All passed:

- Challenge + token round-trip issues a JWT with `sub` = the app's own
  `app_id`, verified independently with `jsonwebtoken.verify()`.
- A well-formed but never-paired `app_id` → `404 app_unknown`.
- A malformed, lab-era-style `app_id` (e.g. `"admin-test-device"`) →
  `400 invalid_app_id` — exactly the §9.4 scenario, confirmed handled
  before any crypto call runs.
- `POST /apps/register` → `404`, route no longer exists.

## 10.3 Status

- ✅ Correction accepted, verified against firmware source independently
  (not taken on trust), implemented, live-tested against the real DB —
  done in the same session as the report.
- ⚠ §9.4's production check still needs re-running once real `app_id` rows
  exist; the lab pass is not evidence for production.
- Bridges (§6) and locks-row flash status (§9.6) — no change, still
  firmware's own tracking.
