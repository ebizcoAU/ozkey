OZPMS – Enterprise Property Management Platform for 100,000+ Properties
Author: Truong Viet Phan (Vince Phan)
Date: 2026-08-08
Status: Product Specification – For Review
Version: 1.1

---

## STATUS REGISTER

Maintained from 2026-08-10. Dated record of claim maturity. Inherits the
shared platform claims (C1–C10) from `ozlock.md`'s register; this register
covers enterprise-tier claims only.

**Two corrections in rev 1.1 are material to procurement** and are flagged
here rather than buried: the silicon-origin claim (P4) was factually
inverted, and the headline commercial feature (P6) is very likely unlawful
in the target jurisdiction as described. Both are recorded rather than
silently edited, because a reviewer finding them independently after we had
quietly changed them would be far more damaging than us naming them.

### Revision history

| Rev | Date | Change | Raised by |
|---|---|---|---|
| 1.0 | 2026-08-08 | Initial enterprise specification | Vince Phan |
| 1.1 | 2026-08-10 | Status register added. Corrected: Espressif jurisdiction (§9), rent-arrears lockout legality (§5), support-contract arithmetic (§10), Phase 1 status (§11), lock sleep behaviour (§3) | ozkey firmware review |

### Claim verification status

| # | Claim | Status | Evidence |
|---|---|---|---|
| P1 | Sealed envelopes; PM cloud cannot read credentials | **VERIFIED** | Inherits OZLOCK C1/C2 |
| P2 | Revocation is lock-side (DP 101) | **VERIFIED** | Hardware-verified 2026-08-10 |
| P3 | Tenant holds their own X25519 key | **VERIFIED** | Bond ceremony; key generated on phone, never transmitted |
| P4 | "Non-Chinese hardware — ESP32-C6 (Espressif, **Taiwan**)" | ❌ **FACTUALLY WRONG — corrected in 1.1** | **Espressif Systems is headquartered in Shanghai, PRC**, and is listed on the Shanghai STAR Market. TSMC (Taiwan) fabricates the die; the *vendor* is Chinese. Asserting the opposite in a defence compliance table is a procurement-credibility risk. See §9. |
| P5 | Wi-Fi direct locks, 60–600 s wake, multi-year battery | ⚠️ **PARTIALLY UNVERIFIED** | Interval is implemented; battery life at that duty cycle is unmeasured (inherits C9). Note this tier's transport (Wi-Fi direct, no bridge) diverges from the Thread stack the residential/hospitality tiers use — two transport paths to maintain. |
| P6 | Rent-arrears lockout as "the critical commercial feature" | ⚠️ **LEGAL RISK — reframed in 1.1** | Denying a residential tenant access without a tribunal order is unlawful under Australian residential tenancy law in every state. Target buyers (public housing, defence housing) are the most exposed to this scrutiny. Lawful alternatives specified in §5. |
| P7 | LoRa option | 🔵 **DESIGN ONLY** | No LoRa implementation exists. No hardware evaluated. |
| P8 | Audit logs on PM-owned cloud | **ACHIEVABLE** | Consistent with the metadata/content split — see OZLOCK C7 note |
| P9 | Phase 1 "OZPMS Cloud Server (pure relay, sealed envelopes), 4–6 weeks" | ✅ **SUBSTANTIALLY COMPLETE** | Delivered by ozkey-13 (S1–S9) + ozkey-17. Not 4–6 weeks of remaining work. |
| P10 | 1,000,000-property scaling tier | 🔵 **PAPER DESIGN** | No load testing at any tier has been performed |

---

Executive Summary
OZPMS is the enterprise property management tier of the Sovereign Edge platform, designed for large-scale property portfolios including public housing, defence housing, private property management firms, community housing providers, and institutional build-to-rent operators managing 10,000–100,000+ properties across Australia.

OZPMS addresses a fundamental gap in the smart-lock market: no existing platform offers property managers the combination of scale, security, data sovereignty, and legitimate control required for institutional property management. White-label platforms (Tuya, TTLock) are cloud-centric, vendor-controlled, and expose tenant data and access to third-party operators. OZPMS reverses this: the property management company owns its own cloud infrastructure, controls its own data, and manages the credential lifecycle lawfully — issuance, time-bounded expiry, non-renewal, and tribunal-ordered revocation (see §5, rev 1.1: arrears are a tribunal matter, not a lock matter) – while tenants retain privacy and security through end-to-end encryption and local lock authority.

The core differentiator: OZPMS is a "mini Tuya" owned by the property management company itself. Unlike Tuya/TTLock where a third-party vendor controls every lock globally, OZPMS puts control in the hands of the property manager – the party that has a legitimate legal relationship with the tenant.

1. Target Market & Use Cases
Primary Sectors
Sector	Example	Scale
Public housing	State/territory housing authorities	50,000–100,000+ properties
Defence housing	Military family accommodation	10,000–20,000 properties
Private property management	Large PM firms (Ray White, LJ Hooker, etc.)	10,000–50,000+ properties
Community housing	NGO-managed social housing	5,000–20,000 properties
Build-to-rent	Institutional residential portfolios	5,000–50,000+ apartments
Use Cases
Scenario	Requirement	OZPMS Solution
Tenant move-in	Quick, secure access provisioning	PM issues digital passport (bond) via OZPMS App
Rent arrears	Arrears management (NOT lockout — see §5, rev 1.1)	Arrears are a tribunal matter, not a lock matter. OZPMS records and reports; it does not seize access.
End of lease	Full access revocation; new tenant onboarding	PM revokes old bond; issues new bond to incoming tenant
Maintenance access	Temporary access for tradespeople	PM issues time-bound PIN or temporary bond
Emergency access	Immediate entry (fire, police, medical)	PM can unlock remotely or issue emergency PIN
Audit & compliance	Track who accessed which door, when	All access logs stored on PM-owned cloud
Tenant self-service	Remote unlock, guest access	OZPMS App provides remote unlock and guest PIN issuance
2. Architecture Overview
High-Level System Diagram
text
┌─────────────────────────────────────────────────────────────────┐
│                    OZPMS SYSTEM ARCHITECTURE                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐   │
│   │   Tenant     │     │   Tenant     │     │   Tenant     │   │
│   │   OZPMS App  │     │   OZPMS App  │     │   OZPMS App  │   │
│   └──────┬───────┘     └──────┬───────┘     └──────┬───────┘   │
│          │                    │                    │           │
│          └────────────────────┼────────────────────┘           │
│                               ▼                                │
│                    ┌─────────────────────┐                     │
│                    │   OZPMS Cloud       │                     │
│                    │   (PM-Owned)        │                     │
│                    │   • Identity        │                     │
│                    │   • Sync authority  │                     │
│                    │   • Message routing │                     │
│                    │   • Audit storage   │                     │
│                    └──────────┬──────────┘                     │
│                               │                                │
│          ┌────────────────────┼────────────────────┐           │
│          ▼                    ▼                    ▼           │
│   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐   │
│   │   Property 1 │     │   Property 2 │     │   Property N │   │
│   │   Wi-Fi Lock │     │   LoRa Lock  │     │   Wi-Fi Lock │   │
│   └──────────────┘     └──────────────┘     └──────────────┘   │
│                                                                 │
│   ┌──────────────────────────────────────────────────────────┐  │
│   │   OZPMS Web App (Property Manager Dashboard)             │  │
│   │   • Tenant management   • Rent roll                      │  │
│   │   • Lock provisioning   • Access control                 │  │
│   │   • Credential lifecycle • Audit logs                     │  │
│   └──────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
Component Descriptions
Component	Role	Owner
OZPMS Cloud Server	Central authority for the PM company. Routes messages, stores metadata, manages sync, audits	PM company
OZPMS App (Tenant)	Tenant's mobile app. Remote unlock, BLE unlock, guest PIN issuance, view access history	Tenant
OZPMS Web App (PM)	Property manager dashboard. Tenant management, rent roll, lock provisioning, lockout, audits	PM staff
Door Lock	Wi-Fi or LoRa connected. 60–600 s configurable heartbeat interval (this is a report cadence, not a sleep state — current firmware is receiver-on-when-idle; see Status Register P5). LoRa is design-only.	Property
OZPMS Database	MySQL/PostgreSQL on PM-owned cloud. Stores metadata, audit logs, tenant records	PM company
Cloud Options
Deployment Option	Who Hosts	Data Location	Use Case
PM-Cloud (AWS/Azure)	PM company (via cloud provider)	Australian region (AWS/ Azure datacentre)	Most private PM firms
On-Premises	PM company's own servers	PM company's own facility	Defence housing, government
Hybrid	PM company + cloud provider	Australian region	Flexibility, disaster recovery
3. Lock Hardware & Connectivity
Wi-Fi Direct Locks (Primary Configuration)
Specification	Value
Chip	ESP32-C6 N8
Network	Wi-Fi 6 (2.4GHz)
Wake cycle	60–600 seconds (configurable)
Power	4× AA batteries (multi-year)
Cost	Sub-$100 AUD per lock
No bridge required	Each lock connects directly to the PM's cloud
Rationale: At 100,000+ properties, a bridge-per-door is economically and logistically infeasible. Wi-Fi direct is the simplest, most reliable path.

LoRa Option (For Remote/ Large Properties)
Specification	Value
Network	LoRaWAN (public or private)
Range	1–10 km (line of sight)
Power	Very low (battery life extended)
Bandwidth	Low (kbps – sufficient for credentials)
Latency	Minutes (scheduled wake)
LoRa is suitable for:

Defence housing on large bases

Public housing estates

Rural/remote properties

Properties with poor Wi-Fi coverage

Wi-Fi vs LoRa Comparison
Feature	Wi-Fi	LoRa
Range	50–100 m	1–10 km
Bandwidth	High (Mbps)	Low (kbps)
Cost	Low	Low-medium
Infrastructure	Wi-Fi router needed	LoRa gateway needed
Latency	10 min (sleep)	10 min (sleep)
OTA updates	Feasible	Difficult
4. Tenant Experience
Onboarding Flow
text
1. Tenant signs lease → PM creates tenant account in OZPMS
2. Tenant receives SMS/email: "Download OZPMS App"
3. Tenant registers → App generates X25519 keypair
4. PM company pairs tenant's app_id with property's lock
5. Lock stores bond #N (member bond) – tenant can open door
6. Tenant receives PIN (fallback credential) – either:
   a. SMS/email, or
   b. In-app display (more secure)
Unlock Methods
Method	Latency	Network Required	Use Case
BLE at the door	Instant	No	Arriving home, daily access
Remote unlock (app)	0–10 min	Yes	Letting in visitors, tradespeople
PIN keypad	Instant	No	Fallback, visitors without app
Digital passport (BLE)	Instant	No	Premium user experience
Tenant App Features
Feature	Description
Remote unlock	Tap "Open Door" – command queued, lock opens on next wake
BLE unlock	Walk to door → phone automatically unlocks via BLE
Guest PIN	Generate time-bound PIN for visitors
Access history	View recent door opens (who, when)
Property details	View lease info, rent due, maintenance requests
Rent payment	Pay rent via integrated payment gateway
Support	Contact PM, report maintenance issues
5. Property Manager Experience
OZPMS Web App Dashboard
Feature	Description
Tenant management	View all tenants, leases, contact details
Rent roll	Track rent payments, arrears, due dates
Lock provisioning	Pair locks to properties, issue tenant credentials
Access control	Credential lifecycle — expiry, non-renewal, tribunal-ordered revocation (see §5)
Temporary access	Issue time-bound PINs or bonds for tradespeople
Audit logs	View all door access events (who, when, which door)
Maintenance	Track repairs, schedule access for contractors
Reporting	Compliance reports, security audits
Access Lifecycle Management (Arrears and Lease End)
**REWRITTEN rev 1.1 — see Status Register P6.**

The previous version of this section described a "Lockout" workflow in
which a property manager could remotely revoke a tenant's door access after
2–4 weeks of rent arrears, and called it "the critical commercial feature."

**That workflow, as described, is unlawful in Australia.** Under the
residential tenancy legislation of every state and territory, a landlord or
agent who denies a tenant access to their home without a tribunal or court
order has committed an illegal eviction. Penalties attach. Two to four
weeks of arrears is far below the threshold for lawful termination, and no
Australian jurisdiction permits self-help lockout at *any* arrears
threshold — the process runs through the tribunal, not the lock.

This matters commercially, not just ethically. OZPMS's named target buyers
are **public housing authorities and defence housing** — precisely the
organisations whose legal and compliance teams will scrutinise this most
closely, and for whom an unlawful-eviction capability is a procurement
disqualifier rather than a feature.

### The lawful capabilities, which use the same mechanism

The underlying technical capability is sound and genuinely valuable. It
simply must be bound to lawful triggers:

| Capability | Trigger | Lawful basis |
|---|---|---|
| **Credential expiry** | Lease end date reached | Access was granted for the term of the tenancy; it lapses rather than being seized |
| **Non-renewal** | Fixed-term lease not renewed | No new credential is issued; nothing is taken away |
| **Vacant-possession revocation** | Tribunal order for possession, executed | The order is the authority; OZPMS executes it and logs that it did |
| **Tenant-initiated** | Tenant hands back / self-revokes | Tenant's own action |
| **Emergency access** | Fire, police, medical, welfare check | Statutory and duty-of-care grounds |
| **Contractor access** | Scheduled maintenance, proper notice given | Time-bound credential, notice requirements satisfied |

```
1. Lease is signed → tenant bond issued, time-bound to the lease term
2. Lease term approaches end → PM either renews (reissue) or does not
3. If not renewed → credential expires on its own date; nothing is revoked
4. If possession is ordered by tribunal → PM executes revocation,
   system records the order reference in the audit log
5. Tenant vacates → bond revoked (DP 101), new tenant onboarded
```

**Design consequence:** time-bounding credentials to the lease term is not
merely a legal accommodation — it is better engineering. Access that lapses
by default is safer than access that persists until someone remembers to
remove it, and it removes the need for a "lockout" button entirely.

**Recommendation:** the dashboard should have no control labelled
"Lockout." Revocation outside expiry should require an order reference to
be recorded, which both keeps the operator lawful and produces exactly the
audit trail that makes the product defensible in a tribunal.

*(This section states a commercial and product-risk position, not legal
advice. Australian tenancy law is state-based and the specifics vary;
qualified legal review should be obtained before any of this is put to a
housing authority.)*

End of Lease Workflow
text
1. Tenant moves out → PM processes vacate
2. PM revokes tenant's bond (DPID 101)
3. Lock removes tenant's credential on next wake
4. PM prepares for new tenant:
   a. Issues new bond to incoming tenant
   b. Issues new PIN
5. All credentials replaced → previous tenant cannot access
6. Security Model
Core Security Principles
Principle	Implementation
End-to-end encryption	All commands between app and lock are sealed with AES-256-GCM
No plaintext credentials	Server never stores PINs, RFID values, or bond secrets
Lock is the authority	Credentials are stored on the lock's bond table, not in the cloud
Revocation is local	DPID 101 (bond_revoke) removes credentials from the lock itself
Data sovereignty	All data (audit logs, metadata) stored on PM-owned cloud
The OZPMS Cloud – A "Mini Tuya" with Integrity
Aspect	Tuya/TTLock	OZPMS
Cloud owner	Tuya (third-party)	PM company
Credentials stored	Plaintext (in Tuya DB)	Never stored (sealed envelopes only)
Access logs	Stored by Tuya	Stored by PM company
Lock control	Tuya cloud has direct access	PM company has direct access
Data sovereignty	Third-party control	PM company owns its data
Geopolitical risk	Yes (PRC jurisdiction)	No (Australian jurisdiction)
Security Risks & Mitigations
Risk	Mitigation
PM company abuses access	The PM company has a legal relationship with the tenant. This is the same risk as a hotel having a master key.
Cloud breach	Sealed envelopes protect credential material. Server never has plaintext keys.
Tenant phone theft	PM can revoke bond remotely. Thief cannot open door after next wake.
Lock offline	Tenant still has PIN (stored on MCU). Works offline.
Rogue employee	PM can revoke employee access centrally.
7. Comparison: OZPMS vs. Alternative Platforms
OZPMS vs Tuya/TTLock (White-label)
Feature	OZPMS	Tuya/TTLock
Cloud owner	PM company	Tuya (third-party)
Credentials stored on cloud	❌ No (sealed envelopes)	✅ Yes (plaintext)
Tenant bond	Lock's bond table	Cloud account
Revocation	Lock-side (DPID 101)	Cloud deletion
Offline operation	✅ Yes (BLE + PIN)	❌ No (cloud required)
Data sovereignty	✅ PM company	❌ Tuya
Geopolitical risk	❌ No	✅ Yes (PRC)
OZPMS vs Proprietary PM Solutions
Feature	OZPMS	Proprietary
Open platform	✅ Open protocol	❌ Proprietary
Lock hardware	✅ Standard ESP32-C6	❌ Vendor-locked
Scale	✅ 100,000+	❌ Limited
Data sovereignty	✅ PM-owned cloud	❌ Vendor cloud
Cost	✅ Sub-$100/lock	❌ $200-400/lock
8. Scaling Considerations
OZPMS Cloud Server Architecture
Component	10,000 Properties	100,000 Properties	1,000,000 Properties
API nodes	2	8	50+
Database	Single MySQL	MySQL Cluster	Sharded PostgreSQL
Message queue	Redis	Redis Cluster	Kafka
MQTT broker	EMQX (single)	EMQX (cluster)	EMQX (multi-region)
Storage	100 GB	1 TB	10 TB
Monthly cost	$1,000	$10,000	$100,000+
Per-Property Cost Model
Component	Cost (AUD)
Lock hardware	~$100
PM cloud infrastructure	$0.10/property/month
Tenant app support	$0.05/property/month
Total (per property, monthly)	~$0.15/month
Total (100k properties, monthly)	~$15,000/month
9. Regulatory & Compliance
Australian Privacy Act 1988
Requirement	OZPMS Compliance
Data minimisation	Only necessary metadata stored (not credentials)
Security safeguards	End-to-end encryption, sealed envelopes
Access controls	PM staff have controlled access via OZPMS Web App
Breach notification	OZPMS Cloud can audit and log breaches
Data sovereignty	Data stored in Australia (AWS/Azure AU regions or on-prem)
Defence Housing Requirements
(Corrected rev 1.1 — the previous "Non-Chinese hardware ✅ ESP32-C6 (Espressif, Taiwan)" row was factually wrong and asserted the opposite of the truth. See Status Register P4.)

Requirement	OZPMS Compliance
On-premises hosting	✅ Option for on-prem OZPMS Cloud
Australian jurisdiction	✅ Data and cloud infrastructure stay in Australia
No PRC cloud dependency	✅ No third-party cloud; PM company hosts its own relay
Auditable firmware	✅ Open source, independently buildable and verifiable
Non-Chinese silicon	⚠️ **Not met by the standard tier.** Espressif Systems is a **Shanghai-headquartered PRC company** (Shanghai STAR Market listed). TSMC in Taiwan fabricates the die, but the SoC vendor is Chinese. Do not claim otherwise.
Non-Chinese silicon (defence tier)	✅ EFR32MG24 — Silicon Labs (US). This is the correct answer for a hard non-PRC-silicon requirement.
Secure boot	✅ Defence tier (EFR32MG24)
Audit trail	✅ Full access logs stored locally

**Why this correction matters commercially.** A defence or government
evaluator will verify vendor domicile in minutes. Asserting "Espressif,
Taiwan" in a compliance table would not merely be corrected — it would cast
doubt on every other claim in the document, including the ones that are
true and verifiable. The honest position is still strong: OZPMS removes PRC
*jurisdiction over data*, PRC *cloud dependency*, and *proprietary
firmware* — three of the four axes — and offers a US-silicon tier for the
fourth. That is a better argument than an unsupportable one.
10. Key Differentiators & Commercial Value
What OZPMS Does That Tuya Cannot
Capability	OZPMS	Tuya/TTLock
Tenant controls own key	✅ X25519 bond on lock	❌ Cloud account
PM company owns data	✅ Full data sovereignty	❌ Tuya owns data
Offline BLE unlock	✅ Yes	❌ No (cloud required)
Lawful credential lifecycle (expiry / tribunal-ordered)	✅ Yes, with order reference recorded	❌ Unclear
Geopolitical independence	✅ Yes	❌ PRC jurisdiction
Audit trail on own server	✅ Yes	❌ Tuya's cloud
Open protocol	✅ Yes	❌ Proprietary
Sub-$100 per lock	✅ Yes	❌ $150-300+
Commercial Opportunity
Scenario	Revenue (AUD)
100,000 properties × $100/lock (hardware)	$10,000,000 (one-time)
100,000 properties × $0.15/month (SaaS)	$180,000/year (recurring)
Support contract — per PM ORGANISATION, not per property (corrected rev 1.1; the prior figure implied $1,000/property/year against a $100 lock and $0.15/month infrastructure, which no buyer would accept)	Indicative $50k–250k/year per large portfolio
Total (100k properties)	~$10M initial hardware + ~$180k/year infrastructure + support contracts negotiated per organisation. The prior "$110M+ initial + $100M+ annual" figure was an arithmetic error and is withdrawn.
11. Implementation Roadmap
Phase	Scope	Duration
Phase 1	OZPMS Cloud Server (pure relay, sealed envelopes)	✅ SUBSTANTIALLY COMPLETE — delivered by ozkey-13 (S1–S9) and ozkey-17. Corrected rev 1.1.
Phase 2	OZPMS Tenant App (remote unlock, BLE, PIN)	4-6 weeks
Phase 3	OZPMS Web App (tenant management, lockout, audit)	6-8 weeks
Phase 4	LoRa integration (if required)	2-4 weeks
Phase 5	Pilot with 1,000 properties	4-8 weeks
Phase 6	Scale to 100,000+ properties	Ongoing
12. Conclusion
OZPMS is the enterprise-grade, commercially scaled tier of the Sovereign Edge platform. It delivers the same sovereignty promise as the residential and hospitality products – end-to-end encryption, local lock authority, and no plaintext credentials on any server – but at a scale of 100,000+ properties.

The key differentiator is who owns the cloud: the property management company itself. Unlike Tuya/TTLock where a third-party vendor controls every lock globally, OZPMS puts control in the hands of the property manager – the party that has a legitimate legal relationship with the tenant.

For tenants, OZPMS provides a modern, secure smart-lock experience with privacy protections their data is not accessible to a third-party vendor. For property managers, OZPMS provides lawful, auditable control of the credential lifecycle — issuance, expiry, non-renewal and tribunal-ordered revocation — together with the evidence trail that makes each of those defensible if challenged. It deliberately does NOT provide a self-help lockout capability, because that would be unlawful in the jurisdictions this product targets and would disqualify it in exactly the procurement processes it is built for (rev 1.1).

This is the scale-up of the sovereignty model. What works for a single homeowner works for 100,000+ properties – the architecture is the same, only the deployment tier differs. OZPMS is the platform for large-scale, data-sovereign property management in Australia.