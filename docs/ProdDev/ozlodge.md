OZLODGE – Hospitality Smart-Lock Platform for Hotels, Motels and Short-Stay Accommodation
Author: Truong Viet Phan (Vince Phan)
Date: 2026-08-08
Status: Product Specification – For Review
Version: 1.1

---

## STATUS REGISTER

Maintained from 2026-08-10. Dated record of how each claim matured from
assertion to hardware-verified fact. See `ozlock.md`'s register for the
shared platform claims (C1–C10) that OZLODGE inherits; this register
covers hospitality-tier claims only.

### Revision history

| Rev | Date | Change | Raised by |
|---|---|---|---|
| 1.0 | 2026-08-08 | Initial hospitality specification | Vince Phan |
| 1.1 | 2026-08-10 | Status register added. Corrected: bridge economics (§12), lock sleep behaviour (§2), audit-trail framing reconciled with OZLOCK's content-blindness claim (§9) | ozkey firmware review |

### Claim verification status

| # | Claim | Status | Evidence |
|---|---|---|---|
| L1 | Sealed envelopes; hotel server cannot read credentials in transit | **VERIFIED** | Inherits OZLOCK C1/C2 |
| L2 | PIN stored on lock MCU, works offline | **VERIFIED** | DP 21/22 path, bench-verified |
| L3 | Digital passport = X25519 member bond on lock | **VERIFIED** | M3 member ceremony; phone-to-phone verified 2026-08-07 |
| L4 | Revocation is lock-side (DP 22 / DP 101) | **VERIFIED** | Hardware-verified; `REVOKE_OK` + roster confirms removal |
| L5 | Remote unlock sub-second via bridge | **VERIFIED** | Measured ~240–330 ms lock-side relay, 2026-08-09/10 |
| L6 | Lock "sleeps 5 s–15 min (configurable)" | ❌ **INCORRECT — corrected in 1.1** | Firmware is `rx_on=1 ftd=1`, receiver-on-when-idle. The configurable 60–600 s interval is the *heartbeat*, not a sleep state. |
| L7 | Multi-year battery at hospitality duty cycle | ⚠️ **UNMEASURED** | Inherits OZLOCK C9. Critical at 500-room scale — this is the maintenance contract. |
| L8 | Full audit trail on hotel's own server | **ACHIEVABLE, framing corrected** | Hotels legitimately require this. See §9 note — an on-prem audit trail is not a contradiction of content-blindness, but the distinction must be stated. |
| L9 | After-hours QR self-check-in | 🔵 **DESIGN ONLY** | Phase 5, not built. No implementation exists as of 2026-08-10. |
| L10 | PMS roster sync `/pms/rooms` | **BUILT** | Endpoint exists; upsert/reconcile modes implemented |

---

Executive Summary
OZLODGE is the hospitality tier of the Sovereign Edge smart-lock platform, purpose-built for hotels, motels, short-stay rentals, campuses, and mining accommodation. It delivers the same sovereignty promise as the residential product – end-to-end encryption, local lock authority, and no plaintext credentials on any server – but at commercial scale, with property management system (PMS) integration, automated guest check-in, and centralised front-desk control.

The hospitality market has unique requirements that consumer smart locks cannot meet: staff need to issue and revoke guest credentials instantly, guests need frictionless after-hours check-in, and hotel operators need full audit trails and integration with their existing PMS. OZLODGE addresses all of these while maintaining the core principle that the hotel owns its data – the cloud is a relay, not an authority.

The core differentiator: Unlike Tuya/TTLock where a third-party vendor controls every lock globally and stores guest PINs in their cloud, OZLODGE puts control in the hands of the hotel operator. The hotel's own on-premises server is the authority; the cloud only routes messages. Guest PINs are end-to-end encrypted and never stored in plaintext on any server. This is data sovereignty delivered as a product.

1. Target Market & Use Cases
Primary Sectors
Sector	Example	Scale
Hotels	Boutique to 500+ room hotels	50–500+ rooms
Motels	Roadside motels, independent operators	10–100 rooms
Short-stay rentals	Apartments, holiday homes	1–50 properties
University accommodation	Student housing, dormitories	100–5,000+ rooms
Mining camps	FIFO worker accommodation	100–2,000+ rooms
Extended-stay / serviced apartments	Corporate housing	50–500 rooms
Use Cases
Scenario	Requirement	OZLODGE Solution
Front-desk check-in	Issue room key instantly	MAOI app issues PIN or digital passport to guest
After-hours check-in	Guest arrives when front desk closed	QR self-check-in flow → PIN issued automatically
Guest room access	Open door during stay	PIN or digital passport (BLE unlock)
Staff access	Housekeeping, maintenance	Staff PINs with time-limited validity
Emergency access	Manager override	Master credentials on all locks
Check-out	Revoke guest access	Central revocation via PMS or front desk
Audit trail	Track who opened which door when	Full audit logs on hotel's on-prem server
2. Architecture Overview
High-Level System Diagram
text
┌─────────────────────────────────────────────────────────────────┐
│                    OZLODGE SYSTEM ARCHITECTURE                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐   │
│   │   Guest      │     │   Guest      │     │   Guest      │   │
│   │   App        │     │   App        │     │   App        │   │
│   └──────┬───────┘     └──────┬───────┘     └──────┬───────┘   │
│          │                    │                    │           │
│          └────────────────────┼────────────────────┘           │
│                               ▼                                │
│                    ┌─────────────────────┐                     │
│                    │   OZLODGE Cloud     │                     │
│                    │   (Routing Only)    │                     │
│                    └──────────┬──────────┘                     │
│                               │                                │
│                               ▼                                │
│                    ┌─────────────────────┐                     │
│                    │   OZLODGE Local     │                     │
│                    │   Server            │                     │
│                    │   (Hotel On-Prem)   │                     │
│                    └──────────┬──────────┘                     │
│                               │                                │
│          ┌────────────────────┼────────────────────┐           │
│          ▼                    ▼                    ▼           │
│   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐   │
│   │   Wi-Fi/     │     │   Wi-Fi/     │     │   Wi-Fi/     │   │
│   │   Thread     │     │   Thread     │     │   Thread     │   │
│   │   Bridge     │     │   Bridge     │     │   Bridge     │   │
│   └──────┬───────┘     └──────┬───────┘     └──────┬───────┘   │
│          │                    │                    │           │
│          └────────────────────┼────────────────────┘           │
│                               ▼                                │
│                    ┌─────────────────────┐                     │
│                    │   Thread Mesh       │                     │
│                    │   (Door Locks)      │                     │
│                    └─────────────────────┘                     │
│                                                                 │
│   ┌──────────────────────────────────────────────────────────┐  │
│   │   MAOI App (Front Desk / Staff)                         │  │
│   │   • Check-in / Check-out   • Issue PINs                 │  │
│   │   • Room assignment        • Staff management           │  │
│   │   • Audit logs             • Emergency override         │  │
│   └──────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
Component Descriptions
Component	Role	Owner
OZLODGE Local Server	The authority for the hotel. Stores rooms, credentials (encrypted), audit logs. Runs on hotel on-premises hardware.	Hotel
OZLODGE Cloud	Routing-only relay between Guest App and Local Server. No lock control, no credential storage.	Sovereign Edge (hosted)
OZLODGE Guest App	Guest's mobile app. Remote unlock, BLE unlock, QR self-check-in, view booking details.	Guest
MAOI App	Front-desk staff app. Check-in/out, issue PINs, manage rooms, audit logs.	Hotel staff
Bridge (ESP32-C6 N16)	Thread border router + MQTT uplink. One per building or floor.	Hotel
Door Lock (ESP32-C6 N8)	Thread-connected, battery-powered. Full Thread Device, receiver-on-when-idle (rx_on=1) — the 60–600 s configurable value is the heartbeat interval, NOT a sleep state. See Status Register L6/L7.	Hotel
Cloud Options
Deployment Option	Who Hosts	Data Location	Use Case
Cloud (Singapore)	Sovereign Edge	Singapore	Small hotels, motels (routing only)
On-Premises	Hotel's own server	Hotel premises	Large hotels, defence, government
Hybrid	Hotel + cloud	Hotel + Singapore	Flexibility, disaster recovery
3. Hotel Modes – Two Deployment Options
Mode 3: OZKEY (On-Premises Local Server)
This is the primary hotel deployment.

Feature	Specification
Server	OZKEYSERV (on-premises, :3200)
Data storage	Hotel's own MySQL database
Authority	Hotel's local server
Cloud role	Routing only (OZLODGE Cloud)
Audit logs	Stored on hotel premises
Credentials	Stored encrypted on local server
Internet dependency	Minimal – local server works without internet
Use case	Hotels, motels, campuses, mining camps
Mode 4: OZPMS (Cloud-Hosted, Multi-Property)
This is the same server codebase, deployed in the cloud for property management companies. (See separate OZPMS specification.)

4. Room Roster Sync – The PMS Integration
The hotel's room roster (rooms, floors, buildings, room types) is the foundation of OZLODGE. The local server maintains a mirror of the PMS room data.

POST /pms/rooms – Roster Sync Endpoint
Mode	Description
upsert	Insert or update listed rooms; rooms not listed are untouched
reconcile	Payload is the complete roster; rooms absent are marked active=0
Room Schema
json
{
  "id": "rm_a1b2",          // PMS stable row id – the join key
  "room_no": "101",         // Display label; renameable without orphaning the lock
  "name": "Phòng Đôi 101",
  "type": "Đôi",            // Room-type label only
  "floor": 1,
  "capacity": 2,
  "lock_device_id": "ozk-…" // Optional; the binding, carried in-band
}
The Join Key: PMS id
The PMS row id is the stable join key. room_no is a mutable display label – renaming "101" to "Suite A" does not orphan the bound lock or in-flight credentials. This is critical for hotels that rename rooms during refurbishment.

Write Authentication
All /pms/* writes require a shared-secret header:

text
X-OZKEY-Secret: <configured secret>
This ensures only authorised MAOI tablets can modify room data. The cockpit dashboard reads data but does not write in normal operation.

5. The MAOI App – Front Desk Control
MAOI is the commercial front-desk application for hotel staff. It runs on iPad, enabling staff to manage room access without accessing the physical lock.

MAOI Features
Feature	Description
Check-in	Assign guest to room, issue PIN or digital passport
Check-out	Revoke guest credentials, prepare room for next guest
Room status	View room availability, occupancy, lock status
Issue PIN	Generate time-bound PIN for guest (4-8 digits)
Issue RFID	Issue RFID card or key fob
Staff access	Issue staff PINs with time-limited validity
Master credentials	Emergency override for all locks
Audit log	View all door access events
Room roster sync	Push room definitions from PMS to server
Authority Model
text
MAOI originates · OZKEYSERV records · Cockpit observes
System	Role	Writes in normal operation
MAOI (tablet)	Authority for operations	Room defs, bindings (pair/unpair), credential issuance, housekeeping
OZKEYSERV :3200	System of record + router	Only server-minted facts (device_id, mac_token, credential_id, door logs)
Cockpit :3300	Assisting tool	Nothing normally – monitor/audit/sync-status; fallback writer only in declared emergency
6. Guest Experience
Guest Onboarding Flow
text
1. Guest books room via Booking.com / hotel website
2. Booking enters PMS → OZLODGE Local Server
3. Guest receives confirmation email with:
   a. Room number
   b. Check-in instructions
   c. Link to download OZLODGE Guest App (optional)
4. Guest arrives at hotel:
   a. Front desk: Staff checks in guest via MAOI → issues PIN or digital passport
   b. After-hours: Guest scans QR code at entrance → self-check-in flow
Unlock Methods
Method	Latency	Network Required	Use Case
PIN keypad	Instant	No	Primary method. Guest types PIN at door.
BLE (digital passport)	Instant	No	Guest uses OZLODGE App to open door via BLE
RFID card	Instant	No	RFID key card (traditional)
Remote unlock (staff)	Sub-second	Yes (bridge)	Staff lets guest in remotely
Assisted unlock	60s window	Yes	Owner lets visitor in while away
Guest App Features (Optional)
Feature	Description
View booking	See room number, check-in/out dates
Digital passport	BLE unlock via app
Remote unlock	Open door from anywhere (if enabled)
Guest PIN	View PIN issued by front desk
Check-out	Request check-out, extend stay
Support	Contact front desk
7. After-Hours Self Check-In – The QR Flow
One of the most powerful features of OZLODGE is the autonomous after-hours check-in.

The Problem
Guests arrive late at night. Front desk is closed. No staff to issue a key. The guest is stranded.

The OZLODGE Solution
text
Hotel displays QR code (outside entrance, 24/7)
        ↓
Guest scans QR with OZLODGE App (or phone camera → app link)
        ↓
App combines QR data + GPS proximity (~100m radius)
        ↓
Identifies hotel ID → checks against existing booking
        ↓
OZLODGE App → OZLODGE Cloud → OZLODGE Local Server
        ↓
Local server:
  → Generates unique PIN for this booking
  → Sends command to door lock (via bridge → Thread) to program PIN
  → PIN is stored on lock MCU (DPID 21)
  → PIN returned to Guest App (in-app, not SMS)
        ↓
Guest uses PIN to open door
        ↓
PIN is valid only within booking period (auto-expires)
What This Solves
Problem	Solution
No staff at reception	Fully autonomous check-in
Guest doesn't have app	QR can open app download page first, then check-in
SMS/email dependency	PIN delivered in-app, not over SMS
Security	QR + GPS proximity verifies guest is at hotel
PIN exposure	PIN delivered in-app (encrypted), not SMS
Timely issuance	PIN only issued on check-in day, valid within booking period
How It Compares to SMS PIN Delivery
Factor	SMS PIN	QR Self Check-In
Delivery channel	SMS (visible to carrier)	In-app (TLS encrypted)
Risk	Interception by carrier/email provider	No third-party visibility
Guest friction	Low – any phone can receive SMS	Requires app (but QR leads to download)
Security	Good	Better (app auth + proximity)
Cost	SMS cost per guest	No SMS cost
8. PIN vs Digital Passport – Two Credential Types
Method 1: PIN (Primary)
Aspect	Description
What it is	4-8 digit code stored on lock MCU
How guest receives it	In-app (MAOI → server → guest app) or SMS fallback
How guest uses it	Types PIN on keypad at door
Works offline	✅ Yes – PIN stored on MCU
Expiry	Time-bound (booking period)
Revocation	Server sends delete frame (DPID 22) to lock
Security	Good – 6-digit PIN with limited expiry
Guest friction	Very low – type 6 digits
Method 2: Digital Passport (BLE)
Aspect	Description
What it is	X25519 bond (member credential) stored on lock
How guest receives it	In-app (M3 member ceremony)
How guest uses it	Opens app → BLE unlock
Works offline	✅ Yes – bond stored on lock
Expiry	Time-bound (booking period)
Revocation	Server sends DPID 101 (bond_revoke)
Security	Better – app auth + biometric
Guest friction	Low – tap "Open Door" in app
Comparison
Feature	PIN	Digital Passport
Requires app	No	Yes
Requires BLE	No	Yes
Works with any phone	Yes	No (must have app)
Audit trail	PIN slot used	Which app_id opened door
Phishing resistance	Good	Better
Guest friction	Very low	Low
Staff setup	Issue PIN in MAOI	Issue passport in MAOI
Recommendation: Hybrid Approach
Scenario	Recommended Method	Why
Guest has app	Digital Passport (BLE)	More secure, better UX
Guest doesn't have app	PIN	Low friction, no app required
After-hours check-in	PIN (via QR flow)	Reliable, no staff needed
Fallback	PIN	Always works, even offline
9. Security Architecture
Core Security Principles
Principle	Implementation
End-to-end encryption	All commands between app and lock are sealed with AES-256-GCM
No plaintext credentials	Server never stores PINs, RFID values, or bond secrets in plaintext
Lock is the authority	Credentials are stored on the lock's bond table and MCU, not in the cloud
Revocation is local	DPID 22 (delete PIN) and DPID 101 (bond_revoke) remove credentials from the lock itself
Data sovereignty	All audit logs and metadata stored on hotel's on-premises server
The OZLODGE Cloud – Routing Only
Aspect	OZLODGE Cloud	Tuya/TTLock
Cloud can control locks	❌ No – routing only	✅ Yes – direct cloud control
Cloud stores credentials	❌ No – sealed envelopes only	✅ Yes – plaintext PINs in DB
Data sovereignty	✅ Hotel owns data	❌ Tuya owns data
Geopolitical risk	❌ No	✅ Yes (PRC jurisdiction)
Offline operation	✅ Yes (BLE, PIN)	❌ No (cloud required)
Security Risks & Mitigations
Risk	Mitigation
Compromised bridge	Only locks on that bridge affected; keys remain secure
Compromised staff phone	Staff credentials can be revoked centrally
Guest phone theft	PIN still works (something they know); digital passport can be revoked
Cloud breach	Cloud has no keys, no plaintext credentials, no lock control
Hotel server compromise	Credentials are encrypted at rest; seals prevent decryption without keys
Geopolitical action	Hotel's on-prem server is unaffected
10. Staff & Operational Features
Housekeeping Access
Feature	Description
Staff PINs	Time-limited PINs for housekeeping, maintenance
Access schedule	Staff can open assigned rooms during working hours
Audit trail	Staff access logged (who, when, which room)
Revocation	Staff PINs can be revoked at any time
Master Credentials
Feature	Description
Manager override	Master PIN or RFID to open any lock in emergency
Emergency unlock	Remote unlock via MAOI (fire, police, medical)
Audit trail	Emergency access logged for compliance
Room Status Lifecycle
text
Unpaired (grey) → Pair → Available (green) → Issue key → PendingUpdate (red)
    → Heartbeat flush → Occupied (blue)
When a guest checks out, their PIN is revoked and the room returns to Available.

11. Comparison: OZLODGE vs. Competitors
OZLODGE vs Tuya/TTLock (White-label)
Feature	OZLODGE	Tuya/TTLock
Cloud owner	Hotel / Sovereign Edge	Tuya (third-party)
Credentials stored on cloud	❌ No (sealed envelopes)	✅ Yes (plaintext)
Guest PIN storage	Lock MCU	Tuya cloud
Revocation	Lock-side (DPID 22/101)	Cloud deletion
Offline operation	✅ Yes (BLE, PIN)	❌ No (cloud required)
Data sovereignty	✅ Hotel	❌ Tuya
Geopolitical risk	❌ No	✅ Yes (PRC)
PMS integration	✅ Yes (via /pms/rooms)	❌ Limited
OZLODGE vs Traditional Hotel Locks
Feature	OZLODGE	Traditional RFID
PIN access	✅ Yes	❌ No
Mobile BLE unlock	✅ Yes	❌ No
Remote unlock	✅ Yes (staff via MAOI)	❌ No
After-hours check-in	✅ QR self-check-in	❌ No
Audit trail	✅ Full digital audit	❌ Limited
Guest self-service	✅ Yes	❌ No
12. Key Differentiators & Commercial Value
What OZLODGE Does That Tuya Cannot
Capability	OZLODGE	Tuya/TTLock
Hotel owns its data	✅ Full data sovereignty	❌ Tuya owns data
No cloud credential storage	✅ Yes (sealed envelopes)	❌ Plaintext PINs in cloud
Offline PIN unlock	✅ Yes (stored on MCU)	❌ No (cloud required)
After-hours QR self-check-in	✅ Yes	❌ No
PMS integration	✅ Yes	❌ Limited
Local server authority	✅ Yes (on-prem)	❌ Cloud only
Geopolitical independence	✅ Yes	❌ PRC jurisdiction
Commercial Opportunity
Scenario	Revenue (AUD)
500 rooms × $100/lock (hardware)	$50,000 (one-time)
500 rooms × ~$20/bridge (tested density 8–16 locks per bridge → ~40 bridges)	~$800 (one-time). Corrected rev 1.1: prior figure of $5/bridge was wrong — bridges are ~$20 in small quantity, $12–15 AUD at 1,000 units.
500 rooms × $1/room/month (SaaS)	$6,000/year (recurring)
500 rooms × $100/year (support contract)	$50,000/year (recurring)
Total (500-room hotel)	$50K initial + $56K annual
13. Implementation Roadmap
Phase	Scope	Duration
Phase 1	OZLODGE Local Server (on-prem, hotel mode)	Complete
Phase 2	MAOI App (front-desk staff)	Complete
Phase 3	Guest App (BLE unlock, PIN display, QR self-check-in)	In progress
Phase 4	PMS integration (roster sync, booking integration)	In progress
Phase 5	After-hours QR self-check-in flow	Planned
Phase 6	Digital passport (BLE member bond)	Complete
Phase 7	Pilot with 50-500 rooms	Q4 2026
14. Conclusion
OZLODGE is the hospitality tier of the Sovereign Edge platform, delivering secure, data-sovereign smart-lock capabilities to hotels, motels, short-stay rentals, and commercial accommodation operators.

The key differentiator is who owns the data and the authority: the hotel itself. Unlike Tuya/TTLock where a third-party vendor controls every lock globally and stores guest PINs in their cloud, OZLODGE puts the hotel's own on-premises server in control. The cloud is a routing-only relay; it cannot control locks, cannot read credentials, and cannot compromise guest privacy.

For hotel guests, OZLODGE provides frictionless access: PINs work offline, BLE unlock is instant, and after-hours QR self-check-in means no more waiting for a front-desk staff member at 11pm.

For hotel operators, OZLODGE provides the control they need: instant check-in/out, central revocation, full audit trails, and integration with their existing PMS – all while keeping guest data within their own control.

This is the hospitality scale-up of the sovereignty model. What works for a single homeowner works for a 500-room hotel – the architecture is the same, only the deployment tier differs. OZLODGE is the platform for data-sovereign hospitality management in Australia and beyond.