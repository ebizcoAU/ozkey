OZLOCK – The Sovereign Residential Smart-Lock Platform (Updated)
Author: Truong Viet Phan (Vince Phan)
Date: 2026-08-08
Status: Product Specification – For Review
Version: 2.1

---

## STATUS REGISTER

Maintained from 2026-08-10. Purpose: a dated record of how each claim in
this specification matured from assertion, to implementation, to
hardware-verified fact. Kept deliberately honest — an unverified claim is
marked unverified, because a specification that overstates is worth less
than one that can be relied on, and because the progression itself is the
evidence of development.

### Revision history

| Rev | Date | Change | Raised by |
|---|---|---|---|
| 2.0 | 2026-08-08 | Initial "pure relay" specification | Vince Phan |
| 2.1 | 2026-08-10 | Status register added. Three claims corrected against the live implementation: plaintext `log` topic (§3.1), Sleepy End Device / battery (§1), internal contradiction on log storage (§3) | ozkey firmware review |
| 2.2 | 2026-08-14 | **Transport Reference Model added (§3A)** — how bytes actually move, as distinct from what the server may read. Records the BLE path as first-class, the bridge-publishes-for-Thread-locks fact, the REST/MQTT boundary, measured end-to-end latencies, and the clock-distribution asymmetry (Wi-Fi locks currently have no time source) | ozkey firmware |

### Claim verification status

| # | Claim | Status | Evidence |
|---|---|---|---|
| C1 | Commands are AES-256-GCM sealed end-to-end; server cannot read them | **VERIFIED** | ozkey-06 envelope, byte-verified against `envelope.dart`; hardware-verified 2026-08-10 (99 B envelope decoded field by field, ver/counter/nonce/tag all conform) |
| C2 | Server relays `envelope_hex` verbatim, never decrypts | **VERIFIED** | ozkey-13 S1–S9 cutover; `raw_value` and `buildCredentialFrame()` deleted from `ozlockserv` |
| C3 | Monotonic counter prevents replay | **VERIFIED** | `counter_floor` per bond; hardware-verified across reboot 2026-08-10 (U0 block reservation) |
| C4 | Credentials (PIN/RFID) never stored on server in plaintext | **VERIFIED** | ozkey-13 §4; server holds credential metadata only |
| C5 | Lock works offline via BLE and PIN | **VERIFIED** | PIN on MCU; BLE unlock independent of any network |
| C6 | Lock→app channel is sealed | **VERIFIED** | ozkey-17 U1; first production use 2026-08-10 |
| C7 | **Server stores no record of who opened which door** | ⚠️ **PARTLY TRUE — storage yes, wire no** | True of *storage*: `ozlockserv` **removed its `log` subscription 2026-07-31** (`ozlockserv/server.js:132-134`) and `GET /locks/:id/log` returns `410 Gone` (`:2436-2448`, XF-48 §9.4) — nothing on this tier consumes or records door events. Still false on the *wire*: `publishLog()` (`ozdoorlock_core.h:1836`) publishes `{device_id, mac, result, detail, ts}` in **plaintext** to `…/locks/<id>/log`, where `detail` distinguishes owner from member (`:3800`, `:4131`) and a revoke carries the bond's human label (`:3602`). Readable by anyone with broker subscribe rights. See §3.1. |
| C8 | Sleepy End Device | ❌ **INCORRECT — corrected in 2.1** | Firmware runs `rx_on=1 ftd=1` (Full Thread Device, receiver-on). Deliberate: it is why mesh relay is ~300 ms. |
| C9 | Multi-year battery life | ⚠️ **UNMEASURED** | No measurement exists in FTD/rx-on mode under realistic mesh traffic. Required before any volume commitment. |
| C10 | Self-hostable relay, open source | **VERIFIED** | Source is in-repo; licences per §5 |

**Open item blocking a fully truthful C7 — ⚠ REMEDIATION CORRECTED 2026-08-12.**
This previously read "seal `publishLog` behind the ozkey-17 uplink". **That fix
is wrong for this tier, and cannot be applied platform-wide.** The uplink is
sealed *app-to-app, opaque to the server* by construction — but `ozpmsserv`
(`server.js:103`, subscribed `:525`) and `ozlodgeserv` (`:89`/`:95`, subscribed
`:594`) both **actively consume** the `log` topic, because a property-management
product legitimately needs a door-audit trail. Sealing it would not harden those
tiers; it would break them.

The tiers have different threat models and therefore different fixes
(operator's ruling, 2026-08-12):

- **OZLOCK (residential, this document): stop publishing to `topicLog`
  entirely.** Not seal — *drop*. Nothing on this tier consumes it, so there is
  no payload to protect and no consumer to break. `txlogAppend()` already writes
  the transaction log to on-device LittleFS first and works offline, so the
  owner's own record is unaffected. This is the strongest available fix and also
  the cheapest: a tier guard, no crypto.
- **OZPMS / OZLODGE (commercial): keep publishing, fix the broker.** The
  exposure there is not that the server can read it — it is *meant* to. It is
  that the lab broker enforces no credentials at all (verified live 2026-08-08:
  fabricated username + wrong password published successfully — `ozkey-13.md:82`,
  `ozkey-15.md §8.1`, commit `9dbf422`). That is infra (EMQX ACLs), deliberately
  deferred per operator instruction, and tracked in those tiers' registers rather
  than here. Secondary and cheap: stop putting the bond's human label and
  owner/member in `detail` where a slot number would do.

Firmware work for this tier: accepted, queued, not started (`ozkey-23.md` §10).

---

Executive Summary
OZLOCK is the residential tier of the Sovereign Edge smart-lock platform. It is a complete, end-to-end smart-lock solution comprising lock hardware, bridge gateway, mobile app, and cloud relay service – designed from the ground up around a single organising principle: sovereignty. The owner controls their own data, their own network topology, and their own trade-off between battery life and responsiveness. No part of the design requires surrendering any of those choices to a vendor – including to us.

The Core Innovation: A Pure Relay That Cannot Read
The beauty of OZLOCK is that the server is simply a message relay. It cannot read any command, because the data is end-to-end encrypted between the app and the doorlock. Only the paired devices can decode the messages. The server sees that a sealed message was routed from an app to a lock, and its size — never its contents.

**Precision on what "knows nothing" means** (added rev 2.1). The server is blind to *content*, not to *routing*. It necessarily knows which app is paired to which lock, because that is how it delivers anything at all — a postal service reads the envelope. It does not, and cannot, know what was inside. Claiming total ignorance would be both false and unnecessary: content-blindness is already a far stronger guarantee than any competitor offers, and it is one we can actually demonstrate.

⚠️ **Known gap, rev 2.1 — ⚠ CORRECTED 2026-08-12 (see Status Register C7).** One channel does not yet meet this standard. `publishLog()` sends door events — `{device_id, mac, result, detail, ts}`, where `detail` distinguishes owner from member and a revoke carries the bond's human label — as **plaintext** to `ozkey/<site>/locks/<id>/log`.

The previous wording, *"which the server subscribes to and records"*, was **stale and overstated the exposure**. `ozlockserv` removed that subscription on 2026-07-31 and `GET /locks/:id/log` now returns `410 Gone`: *"This server stores no record of which lock opened, when, or by whom"*. On this tier the server does **not** see, subscribe to, or store door events. The residual gap is **wire-level only** — the lock still broadcasts them in the clear to any broker subscriber.

The fix on the residential tier is therefore **to stop publishing them at all**, not to seal them: nothing here consumes the topic, and the on-device LittleFS transaction log already preserves the owner's own record. (Sealing onto the ozkey-17 uplink — the earlier plan — would break OZPMS and OZLODGE, which do consume this topic by design. See the C7 open-item note above.) This document records the gap rather than papering over it, and now records the correction too.

This is the fundamental difference between OZLOCK and every white-label platform on the market.

Aspect	Tuya/TTLock	OZLOCK
What the server sees	Every command, every PIN, every unlock event – in plaintext or decryptable	Opaque sealed envelopes – it sees only that a message was sent
Transaction logs	Stored indefinitely in Tuya's cloud	Never stored – not even ephemerally
Credential storage	PINs, RFID values, fingerprint templates in cloud DB	Never stored – only the lock holds them
Who can read the data	Tuya employees, third-party contractors, governments	Only the app and the lock – no one else
Data sovereignty	Data is owned by Tuya, subject to PRC law	Data never leaves the endpoints
The server is not a data hoard. It is a message switch – like a postal service that delivers sealed letters without ever opening them. It knows who sent to whom, but never what was inside.

1. What Is OZLOCK?
OZLOCK is a complete smart-lock ecosystem for residential use:

Component	Description
Door Lock (ESP32-C6 N8)	Battery-powered, Thread or Wi-Fi, Full Thread Device (receiver-on-when-idle), battery life not yet measured — see Status Register C8/C9
Bridge (ESP32-C6 N16)	Thread border router + MQTT uplink, mains-powered, one per home (optional)
BANOI App	iOS/Android mobile app for commissioning, unlocking, managing members
OZLOCK Cloud Relay	MQTT broker + directory service, routes messages between app and lock
Open Protocol	MIT-licensed protocol specification, third-party implementations allowed
Open Source	GPLv3-licensed server, app, and firmware – self-hostable by anyone
The Two Configurations
Feature	OZLOCK Premium	OZLOCK Economy
Lock chip	ESP32-C6 N8	ESP32-C6 N8
Bridge	✅ Included (ESP32-C6 N16)	❌ No bridge required
Transport	Thread → bridge → Wi-Fi	Wi-Fi direct
Remote unlock	✅ ~1-5 seconds	❌ Not available (10-min sync)
BLE unlock	✅ Instant at door	✅ Instant at door
PIN sync	✅ Instant	✅ 0-10 minutes
Battery life	Multi-year	Multi-year
Cost	Sub-$100 AUD	~$80 AUD
Best for	Homeowners wanting remote unlock	Single-door, budget-conscious
2. The Trust Model – How Sovereignty Is Enforced
The Two Sovereignty Axes
Axis	What It Means	OZLOCK's Position
Admission sovereignty	Who decides which devices may interoperate?	Open – no consortium gatekeeping
Runtime sovereignty	Who controls data and functionality?	Open – local-first, self-hostable
How OZLOCK Enforces Runtime Sovereignty
Mechanism	What It Does	Why It Matters
Owner-generated root key	Key generated on phone, never transmitted	Vendor cannot revoke or hold hostage
End-to-end encryption	AES-256-GCM sealed envelopes	Cloud cannot read commands
Offline PIN verification	PIN stored on MCU	Lock works without cloud
BLE unlock	Direct phone-to-lock, no cloud	Instant, private, offline
Self-hosted cloud	Anyone can run their own relay	No vendor dependency
Open source	Code is public	Independently verifiable
MIT protocol	Anyone can build compatible hardware	No lock-in
3. The Server as a Pure Relay – The Architecture
System Diagram – Encrypted End-to-End
text
┌─────────────────────────────────────────────────────────────────┐
│                    OZLOCK – END-TO-END ENCRYPTION              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐                      ┌──────────────┐       │
│   │   BANOI App  │                      │   Door Lock  │       │
│   │   (Owner)    │                      │   (ESP32-C6) │       │
│   └──────┬───────┘                      └──────┬───────┘       │
│          │                                      │               │
│          │   ┌──────────────────────────────┐  │               │
│          │   │   AES-256-GCM SEALED ENVELOPE │  │               │
│          │   │   • app_id_hex (64 bytes)    │  │               │
│          │   │   • envelope (ciphertext)    │  │               │
│          │   │   • monotonic counter        │  │               │
│          │   │   • AAD bound to device_id   │  │               │
│          │   └──────────────┬───────────────┘  │               │
│          │                  │                   │               │
│          ▼                  ▼                   ▼               │
│   ┌─────────────────────────────────────────────────┐          │
│   │              OZLOCK Cloud Relay                 │          │
│   │              (PURE RELAY)                       │          │
│   │                                                 │          │
│   │   Stores only: device_id ↔ app_id pairing      │          │
│   │   Stores NOTHING else:                         │          │
│   │   • No transaction logs                        │          │
│   │   • No PINs                                    │          │
│   │   • No RFID values                             │          │
│   │   • No access history                          │          │
│   │   • No user names                              │          │
│   │                                                 │          │
│   │   CANNOT read envelopes – no keys              │          │
│   │   CANNOT forge commands – no app_id keys       │          │
│   │   CANNOT replay – counter prevents it          │          │
│   └─────────────────────────────────────────────────┘          │
│                                                                 │
│   ════════════════════════════════════════════════════════════  │
│                                                                 │
│   The server sees only:                                        │
│   • "A message of size N was routed from app A to lock L"      │
│   • "At timestamp T"                                            │
│                                                                 │
│   The server never sees:                                       │
│   • What the message said (unlock, PIN, revoke, etc.)         │
│   • The PIN value                                             │
│   • The RFID value                                            │
│   • Any bond secret or key material                           │
│                                                                 │
│   ⚠ REV 2.1 — NOT YET TRUE for one channel:                   │
│   • Door events reach the server in PLAINTEXT on the          │
│     `…/locks/<id>/log` topic (who-class, what, when).         │
│     See Status Register C7. Remediation specified.            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
The Blind Relay Commitment
(Corrected rev 2.1 — the previous table asserted "Transaction logs ❌ No" one row above "Connection metadata ✅ Yes", which was contradictory, and did not reflect the plaintext `log` channel that exists today.)

Data	Stored?	Why
Pairing (device_id ↔ app_id)	✅ Yes	Required for message routing — the envelope, not the letter
Credentials (PINs, RFID, bond secrets)	❌ No	Sovereignty – never stored, in plaintext or otherwise
Message content	❌ No — cannot be	Sealed; the server holds no key that opens it
Routing metadata (who→whom, size, timestamp)	✅ Yes	Delivery and abuse detection. This is the honest limit of a relay.
Door events (opened, by owner/member, when)	❌ No — not stored, not subscribed	**Corrected 2026-08-12.** `ozlockserv` unsubscribed 2026-07-31; `GET /locks/:id/log` returns `410 Gone`. **Residual gap is wire-level only** — still published in plaintext to the `log` topic, readable by any broker subscriber. Fix on this tier is to stop publishing it (see C7), not to seal it.
User identity / names	❌ No	Account-less model

**The standard we hold ourselves to:** the server may know that a letter was
delivered, to whom, and how big it was. It must never know what the letter
said. Row 5 is the one place we do not yet meet that standard, and it is
recorded here so it cannot be quietly forgotten.
Why the Server Cannot Read Commands
Layer	Mechanism
Key agreement	X25519 ECDH at pairing – both app and lock derive a shared pairing_secret
Per-direction keys	HKDF-SHA256 derives app→lock and lock→app keys
Envelope	AES-256-GCM with monotonic counter (nonce) and AAD binding to device_id
Server role	Relays envelope_hex verbatim, never decrypts, never parses
Lock verification	Opens envelope with its stored bond key, checks counter, executes frame
App verification	Opens lock→app envelopes for logs, confirms integrity
The server never holds the keys. It cannot read the payload even if it wanted to. A subpoena, a breach, or a rogue employee yields nothing but meaningless ciphertext.

3A. Transport Reference Model (added rev 2.2, 2026-08-14)
The diagram in §3 answers *what the server may read*. This section answers *how
bytes actually move*, which is a different question and has been re-derived from
scratch in enough design discussions to be worth writing down once. Everything
below was measured on the bench on 2026-08-14 unless marked otherwise.

The three paths

```
THREAD LOCK  (OZLOCK Premium)
  Tuya DL MCU --UART--> ESP32 module --Thread/802.15.4--> BRIDGE --MQTT--> broker --> ozlockserv --REST--> BANOI
                             ^
                             '------------------ BLE (direct, no server, no network) -----------------'

WI-FI LOCK   (OZLOCK Economy)
  Tuya DL MCU --UART--> ESP32 module --------Wi-Fi/MQTT--------> broker --> ozlockserv --REST--> BANOI
                             ^
                             '------------------ BLE (direct, no server, no network) -----------------'
```

Seven things this model makes explicit that the encryption diagram does not

1. **The last hop is REST, not MQTT.** The app speaks HTTP to `ozlockserv`
   (`/locks/:id/unlock`, `/locks/:id/settings`, `/auth/token`). Only *devices*
   speak MQTT. Confusing the two leads to proposals that route app traffic
   through the broker, which is not how any of this works.

2. **The broker and `ozlockserv` are separate systems.** Mosquitto is a dumb
   pipe; `ozlockserv` is the directory, queue and REST surface that bridges the
   MQTT world to the app's world. "The server" in casual conversation almost
   always means `ozlockserv`.

3. **A Thread lock has no MQTT session of its own — the bridge publishes on its
   behalf.** This is the single most misleading part of the wire. A message on
   `ozkie/<site>/locks/<lock-id>/heartbeat` is *named for the lock* but was
   published by the **bridge**, which republished the lock's Thread presence
   beacon verbatim. The lock itself has never held an MQTT connection. This is
   why Thread locks reported no liveness at all until ozkey-20 R3, and why
   MQTT-shaped code paths silently do nothing for the primary topology.

4. **BLE is first-class, not a fallback.** It carries provisioning, unlock at
   the door, member enrolment, bond revoke and rename, and it is the only path
   that works with **no server and no network**. A provisioned lock is
   deliberately not discoverable except during a 60 s window opened by a
   physical gesture (XF-52), so "the app cannot see the lock" is usually the
   design working, not a fault.

5. **The sealed envelope is end-to-end app-to-lock; every hop between is a
   courier.** `ozlockserv` queues `envelope_hex` without parsing it and the
   bridge relays it without decoding it. This is why the server needed *no*
   knowledge of the `set_name` verb to carry it — only a route. It is the
   mechanism behind claims C1/C2 in the status register.

6. **The lock is a co-processor, not the whole lock.** Our ESP32 sits beside a
   Tuya DL MCU on a UART. Keypad, RFID and the physical bolt belong to the MCU;
   we own radio, crypto, bonds and policy. Credential writes cross that UART as
   DP frames.

7. **Transport is inferred from the provision payload's shape, not declared.**
   `network_key` empty + `ssid` present means permanently Wi-Fi. The app never
   states what kind of lock it is commissioning.

Measured end-to-end latency, 2026-08-14

| Path | Measured |
|---|---|
| Remote command: app REST -> queue -> broker -> bridge -> Thread UDP -> applied on lock | **~1 s** (`set_name`, POST 20:52:22 -> applied 20:52:23) |
| BLE command at the door | sub-second, no server involved |
| Lock state change -> visible in cloud (heartbeat reconciliation) | within one `heartbeat_s` interval (60 s bench) |

Clock distribution — an asymmetry this model exposes

The three roles get time by completely different means, which is invisible
unless the transports are drawn out:

| Role | Time source | Worst-case staleness |
|---|---|---|
| **Bridge** | `ozlockserv` pushes `utc` on the bridge's command topic — on connect, then every 10 min | ~10 min. NTP was deliberately **removed** (bridge32-1.36): two unarbitrated writers, and UDP 123 is blocked on this network and on most hotel/office networks |
| **Thread lock** | The bridge's time beacon — daily, plus opportunistically whenever a child attaches | Up to 24 h if a multicast beacon is lost |
| **Wi-Fi lock** | ⚠️ **Nothing.** Its only sync was SNTP, which cannot answer here | Unbounded — runs on an NVS snapshot |

Two consequences worth carrying into any design discussion:

- **A restored clock is a guess, not a synchronisation.** `doorlock-1.74` now
  distinguishes them (`clock=live` / `NVS-only` / `UNKNOWN`) and asks for the
  time until a real source answers. Before that, a lock that booted from a stale
  snapshot believed it knew the time and never asked. The severe case is a
  **battery change**: NVS is flash, so a unit that sat in a box for weeks boots
  believing it is weeks ago — and temporary-credential expiry is judged against
  that clock, failing *permissive*.
- **The bridge does not persist its clock.** Bridge reboot while `ozlockserv` is
  unreachable leaves every Thread lock behind it with no time source at all. The
  standing rationale is "no server, no product", but that is not true here:
  stored PINs keep working offline, which is precisely when local expiry needs a
  clock. **Open — operator decision.**

Status of this model: paths, latencies and the bridge-republish behaviour are
hardware-verified. The Wi-Fi clock gap is verified and **open** — the fix
(a site-wide retained `ozkie/<site>/time` topic) is specified in `ozkey-32` §9
and not yet built.

4. What This Means for the Owner
Promise	How It's Kept
No one can see my PIN	PINs are sent as sealed DPID 21 frames; only the lock decrypts them
No one can see my usage pattern	Unlock commands are sealed; server cannot tell an unlock from a PIN grant
No one can forge a command	Each envelope is authenticated with AAD and counter; server can't forge
I can self-host	The relay is open source; I can run my own server and keep the same security
The vendor cannot disable my lock	The cloud has no kill switch; lock still works with BLE/PIN offline
No one can replay a command	Monotonic counter prevents replay attacks
5. The Self-Hosted OZLOCK Cloud
Why Self-Hosting Matters
Risk	Tuya/TTLock	OZLOCK
Cloud shutdown	Locks become useless	Lock works offline (BLE/PIN)
Data breach	Tuya's cloud – all locks compromised	Your cloud – only your locks affected
Geopolitical action	PRC government could disable locks	No foreign jurisdiction
Privacy	Tuya sees every unlock	You control who sees logs
Vendor lock-in	Can't leave Tuya without replacing locks	Open protocol – can self-host or switch providers
Who Can Self-Host
Operator	Why They Would
Door lock distributor	Offer OZLOCK as a complete solution, branded as their own, without relying on a foreign cloud
Smart-home integrator	Keep client data within the client's jurisdiction
Property manager	Run locks across multiple properties without a third-party cloud
Tech-savvy homeowner	Full control over their own data and system
Government/defence	Must keep data within national borders
Privacy-conscious user	No third-party access to access logs
Technical Requirements
Requirement	Minimum Specification
CPU	2 vCPUs
RAM	4 GB
Storage	20 GB SSD
Network	Public IPv4 address, port 1883 (MQTT) and 443 (HTTPS) open
OS	Ubuntu 22.04 LTS or Debian 12
Dependencies	Docker, Docker Compose
Database	MySQL 8 (containerised)
Broker	Mosquitto or EMQX (containerised)
Cost	~$10-20 USD/month (AWS Lightsail, DigitalOcean, Linode)
Deployment Options
Option	Description	Best For
Docker Compose	All components containerised, one command to start	Most users
Kubernetes	Enterprise-scale deployment	Large distributors
Bare metal	Direct install on physical server	Defence, government
Cloud marketplace	One-click deploy on AWS, Azure, DigitalOcean	General users
The OZLOCK Cloud Source Code
Component	Licence	Description
ozlockserv	GPLv3	Node.js server, directory service, MQTT routing
bridge32	GPLv3	Bridge firmware (ESP32-C6 N16)
doorlock	GPLv3	Door lock firmware (ESP32-C6 N8)
BANOI	GPLv3	Mobile app (Flutter)
Protocol	MIT	Protocol specification – allows third-party implementations
Anyone can take the source code, build the OZLOCK Cloud Relay, and operate their own network. This is the structural guarantee that prevents vendor lock-in.

6. What Makes OZLOCK Better Than Tuya/TTLock?
The Fundamental Difference: Who Owns the Cloud and What It Can See
Aspect	Tuya/TTLock	OZLOCK
Cloud owner	Tuya (third-party, PRC-domiciled)	You (self-hosted on your own VPS)
Data sovereignty	Tuya owns your data	You own your data
Jurisdiction	PRC law applies	Your country's law applies
Cloud dependency	Mandatory – lock won't work without it	Optional – works offline with BLE/PIN
Lock control	Tuya cloud has direct access	Only your cloud has access
Credentials	Stored in Tuya cloud (plaintext)	Never stored in plaintext
Geopolitical risk	Yes – PRC sanctions could disable locks	No – your cloud, your control
Server can read commands	✅ Yes (plaintext or decryptable)	❌ No (end-to-end encrypted)
Transaction logs	✅ Stored in Tuya cloud	❌ Never stored
Vendor lock-in	✅ Complete (proprietary protocol)	❌ None (open protocol)
The Decisive Difference – Encrypted End-to-End vs. Plaintext Cloud
What the Server Sees	Tuya/TTLock	OZLOCK
PIN values	✅ Yes – stored in cloud DB	❌ No – only the lock decrypts them
Unlock commands	✅ Yes – plaintext MQTT	❌ No – sealed envelopes
Access history	✅ Yes – every door event	❌ No – no transaction logs
Who opened the door	✅ Yes – linked to user account	❌ No – the server doesn't know
Fingerprint templates	✅ Yes – stored in cloud	❌ No – only on the lock
RFID values	✅ Yes – stored in cloud	❌ No – only on the lock
The server is a relay, not a surveillance system. This is the sovereignty promise delivered.

7. Comparison Summary
OZLOCK vs Tuya/TTLock – The Decisive Differences
Feature	Tuya/TTLock	OZLOCK
Cloud owner	Tuya (third-party)	You (self-hosted)
Data sovereignty	Tuya owns your data	You own your data
Jurisdiction	PRC law applies	Your country's law applies
Cloud dependency	Mandatory	Optional (BLE, PIN work offline)
Credentials	Stored in Tuya cloud (plaintext)	Never stored in plaintext
Server can read commands	✅ Yes	❌ No (E2E encrypted)
Transaction logs	✅ Stored in Tuya cloud	❌ Never stored
Geopolitical risk	High (PRC sanctions)	None
Vendor lock-in	Complete (proprietary protocol)	None (open protocol)
Source code	Proprietary	Open source (GPLv3)
Third-party hardware	Not allowed	Allowed (MIT protocol)
Self-hostable	❌ No	✅ Yes
Remote unlock	✅ Yes	✅ Yes (Premium)
BLE unlock	✅ Yes	✅ Yes
Offline unlock	❌ No	✅ Yes (BLE, PIN)
Member sharing	✅ Yes (cloud accounts)	✅ Yes (local bonds)
Audit trail	Tuya cloud (third-party)	Your own server
Distributor branding	❌ No	✅ Yes (self-hosted)
8. Conclusion
OZLOCK is the residential tier of the Sovereign Edge platform – a complete, end-to-end smart-lock solution that puts the owner in control of their own data, network, and security.

The beauty of OZLOCK is that the server is simply a message relay. It cannot read any command, because the data is end-to-end encrypted between the app and the doorlock. Only the paired devices can decode the messages. It knows that a sealed letter was delivered, and to whom; never what it said.

Stated with the precision this document now holds itself to (rev 2.1): content-blindness is verified and demonstrable. Total ignorance is not claimed, because a relay that knows nothing cannot deliver anything. And one channel — the plaintext door-event log — does not yet meet the content-blindness standard; it is recorded in the Status Register as C7 with its remediation specified rather than omitted. A specification that hides its own gaps is worth less than one that names them.

The Sovereignty Promise
Promise	How It's Kept
No one can see my PIN	PINs are sent as sealed DPID 21 frames; only the lock decrypts them
No one can see my usage pattern	Unlock commands are sealed; server cannot tell an unlock from a PIN grant
No one can forge a command	Each envelope is authenticated with AAD and counter; server can't forge
I can self-host	The relay is open source; I can run my own server and keep the same security
The vendor cannot disable my lock	The cloud has no kill switch; lock still works with BLE/PIN offline
No foreign jurisdiction	Self-hosted relay keeps data within your country's laws
The Business Opportunity
For door lock distributors, OZLOCK is a business opportunity: offer a complete, branded smart-lock solution without relying on a foreign cloud provider. For smart-home integrators, OZLOCK offers client data privacy and no vendor lock-in. For homeowners, OZLOCK offers the peace of mind that their data is their own, and their lock works even if the internet goes down.

Tuya and TTLock force you to trust their cloud, which is subject to PRC law and data sovereignty risks. OZLOCK gives you the keys – literally and metaphorically – to your own data.