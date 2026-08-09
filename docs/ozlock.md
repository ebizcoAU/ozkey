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

### Claim verification status

| # | Claim | Status | Evidence |
|---|---|---|---|
| C1 | Commands are AES-256-GCM sealed end-to-end; server cannot read them | **VERIFIED** | ozkey-06 envelope, byte-verified against `envelope.dart`; hardware-verified 2026-08-10 (99 B envelope decoded field by field, ver/counter/nonce/tag all conform) |
| C2 | Server relays `envelope_hex` verbatim, never decrypts | **VERIFIED** | ozkey-13 S1–S9 cutover; `raw_value` and `buildCredentialFrame()` deleted from `ozlockserv` |
| C3 | Monotonic counter prevents replay | **VERIFIED** | `counter_floor` per bond; hardware-verified across reboot 2026-08-10 (U0 block reservation) |
| C4 | Credentials (PIN/RFID) never stored on server in plaintext | **VERIFIED** | ozkey-13 §4; server holds credential metadata only |
| C5 | Lock works offline via BLE and PIN | **VERIFIED** | PIN on MCU; BLE unlock independent of any network |
| C6 | Lock→app channel is sealed | **VERIFIED** | ozkey-17 U1; first production use 2026-08-10 |
| C7 | **Server stores no record of who opened which door** | ⚠️ **NOT YET TRUE** | `publishLog()` sends `{device_id, result, detail, ts}` in **plaintext** to `…/locks/<id>/log`, and `ozlockserv` subscribes to it. See §3.1. Remediation specified, not yet built. |
| C8 | Sleepy End Device | ❌ **INCORRECT — corrected in 2.1** | Firmware runs `rx_on=1 ftd=1` (Full Thread Device, receiver-on). Deliberate: it is why mesh relay is ~300 ms. |
| C9 | Multi-year battery life | ⚠️ **UNMEASURED** | No measurement exists in FTD/rx-on mode under realistic mesh traffic. Required before any volume commitment. |
| C10 | Self-hostable relay, open source | **VERIFIED** | Source is in-repo; licences per §5 |

**Open items blocking a fully truthful C7:** seal `publishLog` behind the
ozkey-17 uplink (the sealed channel now exists and is proven), leaving only
routing metadata in the clear per ozkey-17 §6a's metadata/content split.

---

Executive Summary
OZLOCK is the residential tier of the Sovereign Edge smart-lock platform. It is a complete, end-to-end smart-lock solution comprising lock hardware, bridge gateway, mobile app, and cloud relay service – designed from the ground up around a single organising principle: sovereignty. The owner controls their own data, their own network topology, and their own trade-off between battery life and responsiveness. No part of the design requires surrendering any of those choices to a vendor – including to us.

The Core Innovation: A Pure Relay That Cannot Read
The beauty of OZLOCK is that the server is simply a message relay. It cannot read any command, because the data is end-to-end encrypted between the app and the doorlock. Only the paired devices can decode the messages. The server sees that a sealed message was routed from an app to a lock, and its size — never its contents.

**Precision on what "knows nothing" means** (added rev 2.1). The server is blind to *content*, not to *routing*. It necessarily knows which app is paired to which lock, because that is how it delivers anything at all — a postal service reads the envelope. It does not, and cannot, know what was inside. Claiming total ignorance would be both false and unnecessary: content-blindness is already a far stronger guarantee than any competitor offers, and it is one we can actually demonstrate.

⚠️ **Known gap, rev 2.1 (see Status Register C7).** One channel does not yet meet this standard. `publishLog()` currently sends door events — `{device_id, result, detail, ts}`, where `detail` distinguishes owner from member — as **plaintext** to `ozkey/<site>/locks/<id>/log`, which the server subscribes to and records. Until that channel is sealed, the server does see that a door was opened, by which class of holder, and when. The sealed lock→app channel needed to fix this was built and hardware-verified on 2026-08-10 (ozkey-17 U1); migrating the log topic onto it is specified and outstanding. This document records the gap rather than papering over it.

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
Door events (opened, by owner/member, when)	⚠️ Yes, currently	**Gap — see C7.** Plaintext on the `log` topic today. To be sealed onto the ozkey-17 uplink.
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