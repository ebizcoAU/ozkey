# SOVEREIGN EDGE

### A Decentralised, Privacy-First Smart Infrastructure Platform

**Local-First Networking, Owner-Configurable Power, Open Software, and an Ethical Manufacturing Model**

Truong Viet Phan (Vince Phan)
Systems Architect | Director, eBizco Australia Pty Ltd
**Version 3 — July 2026**

---

> **On this revision.** Version 3 is a substantial rewrite, not an update. Three
> things changed between v2 and v3: the platform's lock silicon moved from the
> ESP32-H2 to the ESP32-C6; native Matter support was removed from the roadmap
> entirely, on grounds set out in §1; and the software layer moved to an open
> licence with a support-and-SLA commercial model. Sections describing the
> credential system have also been restated in the correct tense — v2 described
> parts of it as operational that are designed and partially built. §11 states
> the implementation status of every major claim in this paper.

---

## Executive Summary

Consumer smart-home hardware today is built around a business model that trades
user privacy and long-term reliability for platform lock-in: proprietary cloud
dependency, mandatory bridge devices, and battery lifespans measured in months.
This paper presents **Sovereign Edge**, a smart-lock platform engineered around
a single organising principle — sovereignty. The owner controls their own data,
their own network topology, their own trade-off between battery life and
responsiveness, and their own choice of who supports the system. No part of the
design requires surrendering any of those choices to a vendor, including to us.

Version 3 makes that principle sharper by identifying something the smart-home
industry consistently conflates: sovereignty has **two independent axes**.
*Runtime sovereignty* asks who sees and controls your data while the system
operates. *Admission sovereignty* asks who decides whether your device is
permitted to exist and interoperate at all. Tuya-style platforms are open to
manufacturers and closed at runtime. Matter is open at runtime and closed to
manufacturers, gated behind a consortium's paid certification regime — and it
additionally requires the *customer* to buy a $150–300 AUD controller from one
of the four companies governing that consortium. **Neither delivers both, and
Sovereign Edge ships neither.** §1 develops this in full; it is the paper's
central argument and the reason native Matter support was removed rather than
deferred. Declining Matter does not mean declining the ecosystems: a customer
running Home Assistant can expose these locks to Apple Home over the HomeKit
Accessory Protocol, with no certification involved anywhere in the path (§4.6).

The platform runs on two silicon tiers. Residential, hospitality and PMS
deployments share identical lock hardware — an Espressif **ESP32-C6** module
paired with a small, mains-powered ESP32-C6 bridge that supplies the Wi-Fi
uplink and acts as Thread border router for the lock mesh. Defence and
government buyers step up to a Silicon Labs **EFR32MG24** lock module with a
PSA Certified Level 3 secure element and non-Chinese silicon origin, for
procurement contexts where the secure element itself is what is being
purchased. A firmware architecture holding the lock asleep for the
overwhelming majority of its runtime, with an owner-configurable poll interval,
targets multi-year battery life on four AA cells.

Remote unlock runs over a bridge-held MQTT connection to a broker that stores
nothing but the pairing relationship between a lock and an app — never a
transaction log — with the payload end-to-end encrypted so the broker cannot
read what a command says or does. It necessarily observes *that* one occurred,
and §4.1 states that limit precisely rather than hiding behind the word
"blind": the metadata retained, its retention period, and the self-hosted mode
that removes the exposure altogether. Owners who prefer no
Sovereign Edge infrastructure at all can point the same bridge at their own
broker — a Home Assistant Mosquitto instance, or anything else that speaks
MQTT — with no firmware change and no account.

**The software is open.** Server, app and protocol are published under an open
licence. What the company sells is hardware and **support** — tiered SLAs
delivered by a small team whose reach is extended by AI assistance. Because the
software is open, that support is a competitive service rather than a
monopoly: a customer who is unhappy can self-host, or hire someone else, and
the system keeps working. Support revenue that can only be retained by
continuing to earn it is the intended structure, not an accident of it.

Manufacturing is anchored in a training-and-employment model — the **Sanctuary
Enterprise** — that recruits and trains underemployed workers in Vietnam to
internationally benchmarked hardware standards, paid well above prevailing
local factory wages, funded transparently through an Australian parent company.

Launch volume is **20,000 locks across five mechanical form factors and 5,000
bridges**, targeting the Australian consumer, hospitality/PMS, and
defence/government markets.

---

## 1. Two Kinds of Sovereignty

### 1.1 The distinction the industry does not make

"Open" is treated as a single property in smart-home marketing. It is not. A
platform can be open in one of two quite different senses, and being open in
one says nothing about the other.

**Runtime sovereignty** — who sees, stores and controls the data while the
system operates. Does a door event reach a vendor's server? Does the system
keep working when that server is unreachable? Can the vendor read what passes
through their infrastructure? Can they disable a device they have already sold?

**Admission sovereignty** — who decides whether a device is permitted to exist
and interoperate. What does a manufacturer have to pay, join, or be approved by
before their product is allowed to participate in the ecosystem? Is that
decision made by a market, or by a body?

These are independent. A platform can be excellent on one axis and severely
closed on the other. The two dominant options in this market are each an
example.

### 1.2 Tuya: open to builders, closed at runtime

Tuya-based white-label devices dominate the low-cost smart-home market, and the
reason is admission openness. Anyone can buy a Tuya module for a few dollars,
integrate it in weeks, and ship. There is no membership, no certification body,
no five-figure entry fee. The barrier to becoming a smart-home manufacturer is
close to zero, which is precisely why the market is saturated with such
devices.

That openness is purchased by closing the runtime. Device traffic transits
Tuya's cloud. Events are stored on infrastructure the manufacturer does not
control and the end user has no visibility into. Functionality degrades or
fails when that cloud is unreachable. The manufacturer gets a cheap path to
market; the customer pays in data and in dependency.

**Tuya's trade: manufacturer freedom, purchased with the customer's runtime
sovereignty.**

### 1.3 Matter: open at runtime, closed to builders

Matter deserves genuine credit on the axis where it performs. It is local-first
by design — commands travel over the LAN or Thread mesh with no cloud round
trip required for local control. It works with the internet down. There is no
mandatory telemetry to the standards body. The SDK is open source and the
specification is published. Multi-admin means a user can share a device across
ecosystems rather than being locked to one vendor's app.

On runtime sovereignty, Matter is close to the opposite of Tuya, and any
analysis that describes Matter as "cloud-centralised" is simply wrong.

The closure is at admission. A Matter device cannot be commissioned unless its
Device Attestation Certificate chains to a Product Attestation Authority
approved by the Connectivity Standards Alliance and listed in the Distributed
Compliance Ledger. Obtaining that requires CSA membership, currently around
**USD $7,000 per year at the minimum Adopter tier**, plus roughly **USD
$2,000–3,000 per product model** in certification fees and **USD $7,000–10,000
per model** in authorised lab testing. First-year cost for a single product is
approximately **USD $20,000**, with the membership recurring annually.

Operating one's own Product Attestation Authority does not escape this. A
self-managed PAA must still be listed in the DCL, which still requires CSA
membership and approval. It is a different way to pay the same gatekeeper, with
a hardware security module bill added.

The governing Promoter members of that body are Apple, Google, Amazon and
Samsung.

**Matter's trade: customer runtime sovereignty, purchased with manufacturer
admission sovereignty.**

### 1.4 The hub does not disappear — it relocates, upward

There is a second cost to Matter that is rarely stated, and it falls on the
customer rather than the manufacturer.

A Matter *Bridge* exposes devices **to** a controller. It is not itself a
controller. Matter's local-first architecture requires something in the home to
act as controller and Thread border router — in practice an Apple TV or
HomePod, a Google Nest Hub, or an Amazon Echo. That is a **$150–300 AUD**
purchase, and most Australian households do not own one. Smart televisions
generally do not qualify: AirPlay or Google TV support does not make a set a
Matter controller *and* a Thread border router. Some high-end Samsung sets ship
a SmartThings hub with Thread, but they are the exception, not the rule.

The consequence for market reach is decisive. **Matter does not expand the
addressable market; it restricts the product to households that have already
bought into an ecosystem.** Hub penetration is low in Vietnam and, while better
in Australia, is nowhere near universal. A manufacturer who makes Matter the
primary path has chosen to sell only to people who have already spent $150–300
with Apple, Google or Amazon.

Version 2 of this paper claimed "no mandatory hub tax" and cited Matter as the
reason. That was exactly backwards. Matter does not remove the hub — it
**mandates a more expensive one, made by someone else**:

| Path | Hub required | Cost to customer | Who owns it |
|---|---|---|---|
| Matter | Apple TV / HomePod / Nest Hub / Echo | $150–300 AUD | Apple, Google, or Amazon |
| Sovereign Edge | Sovereign Edge bridge | $50 AUD | The customer, outright |

Both paths require a box. Ours costs a third to a sixth as much, is bought
once, carries no subscription, and can be pointed at infrastructure the
customer chooses. The honest claim is therefore not that we have abolished the
hub, but that we have made it **cheap, single-purpose, and free of the
ecosystem it would otherwise drag in behind it**.

This is also why Matter's value to a manufacturer is best understood as
**channel and credibility** — a retail badge, a tender checkbox, reduced buyer
anxiety — rather than reach. Those are real benefits, and this paper does not
pretend otherwise: a certification mark is a genuine signal to a buyer with no
other way to evaluate a supplier, and declining it costs us something real.

Two things follow, and both are stated rather than glossed. **A buyer who needs
Matter should buy a Matter product**, and we will say so during a sale rather
than argue them out of a requirement they have already settled. And because we
decline the mark, the burden of demonstrating trustworthiness falls on us by a
different route: **published source, a published protocol, and independently
auditable firmware** (§7). We would argue that is a stronger assurance than a
certificate for a buyer willing to look, and a weaker one for a buyer who is
not. That trade is deliberate.

### 1.5 Steelmanning the gate

The attestation requirement is not arbitrary rent-seeking, and this paper does
not claim it is. Device attestation exists so that counterfeit or compromised
hardware cannot silently join a household's network and misrepresent itself as
a trusted device. In a category that includes door locks, that is a legitimate
and serious security goal. A standards body that admitted anyone would provide
weaker guarantees than one that does not.

Two observations survive that concession.

First, the mechanism chosen to achieve device trust is **corporate
membership**, not technical merit. Nothing about a $7,000 annual fee
establishes that a device is secure; it establishes that its manufacturer is
solvent and willing to pay. The security goal could be met by attestation
schemes that do not also gate participation on the ability to pay a
consortium — signed firmware and published, independently auditable source
being the obvious alternative, which is the path this paper adopts in §7.

Second, whatever the intent, the **effect** is structural. A $20,000 admission
cost plus $7,000 annually is a rounding error for the four companies who govern
the body and a serious barrier for an independent manufacturer. The
interoperability layer of the smart home is therefore reachable only by firms
above a certain size.

**Standards with five-figure admission fees are not neutral infrastructure.
They encode who is permitted to participate.**

We note explicitly that at Sovereign Edge's launch volume of 20,000 units, the
first-year Matter cost would amortise to roughly **AUD $1.50 per lock**. The
decision recorded in §1.6 is therefore *not* primarily a cost decision. We could
afford it. The objection is to what the payment purchases: the annually
renewable permission of a body we do not control, for a product whose entire
proposition is the absence of exactly that relationship — and, per §1.4, a
requirement that our customers buy a $150–300 controller from one of the four
companies governing that body.

### 1.6 Our position: no lock, no bounty, no hold-up

Economics has a precise name for the pattern this platform exists to avoid: the
**hold-up problem**. One party makes an investment that is specific to a
relationship — a homeowner fitting locks to every door, an operator deploying
across a portfolio — and once that investment is sunk and cannot be moved, the
counterparty is in a position to change the terms. Subscription fees appear
where none existed. Features move behind paywalls. Servers are retired. The
customer's own prior investment is the leverage used against them.

This is not a hypothetical risk, and it is not rare. The following are
documented cases in which working hardware that customers had already paid for
was disabled, degraded, or held to new terms by the vendor.

| Case | Date | What happened |
|---|---|---|
| **Nest / Revolv** [1][2] | Announced Feb 2016, effective 15 May 2016 | Google's Nest acquired Revolv in 2014, then announced that "as of May 15, 2016, your Revolv hub and app will no longer work" — **deliberately disabling a USD $300 hub that had been sold with a "free lifetime service subscription."** The lifetime lasted roughly two years. The hardware was not faulty; it was switched off. The FTC opened an investigation, and in its closing letter stated it was concerned that reasonable consumers would not expect the hubs to be rendered unusable, and that doing so unilaterally would cause substantial consumer injury they could not reasonably avoid. The investigation closed after Nest offered full refunds. |
| **LockState** [3][4] | 8 August 2017 | A firmware update intended for the RemoteLock 7000i was **mistakenly pushed to 6000i models, bricking at least 500 locks** — the $469 RemoteLock 6i, sold to Airbnb hosts through a partnership with Airbnb. Around 200 Airbnb hosts were directly affected. The locks could not be recovered remotely; units had to be physically returned for reflashing, quoted at 5–7 days. Physical keys still worked. **The most directly relevant case to this platform**, and the reason for the commitments in §4.1. |
| **Lowe's Iris** ‡ | March 2019 | The retailer shut down its Iris smart-home platform; most connected devices lost functionality, with prepaid cards offered as compensation. |
| **Best Buy Insignia** ‡ | November 2019 | Best Buy discontinued its Insignia Connect line and the app behind it, removing smart functionality from products still in service. |
| **Charter / Spectrum home security** ‡ | February 2020 | The service was discontinued; because the equipment used proprietary configurations, much of it could not be repurposed with another provider. |
| **Wink** [5][6] | Announced 7 May 2020, effective 13 May 2020 | Wink gave customers **one week's notice** that a **$4.99/month subscription** would become mandatory, and that non-subscribers would lose app control, voice control, API access and all automations — on hubs they owned outright, including control of third-party devices connected through them. Customers publicly characterised it as extortion; the company cited one-time device revenue being insufficient to sustain operations. Under pressure the deadline was pushed to 20 May and then to 27 July. **The textbook hold-up: terms changed after the customer's investment was sunk, with the customer's only leverage being public complaint.** |
| **Insteon** ‡ | April 2022 | Servers were shut down without notice; customers discovered their systems had stopped working. Service was later restored after a group of users acquired the assets — a rescue that depended on volunteers and cannot be relied upon. |

In every case the hardware was fine. The relationship was the point of failure.

**Sources.**
[1] FTC closing letter to Nest Labs, 7 July 2016 — `ftc.gov/system/files/documents/closing_letters/nid/160707nestrevolvletter.pdf`
[2] CBC News, "Nest's move to stop supporting Revolv smart hub leaves customers with costly 'brick'", 5 April 2016
[3] BleepingComputer, "Botched Firmware Update Bricks Hundreds of Smart Door Locks", August 2017
[4] The Register, "Firmware update blunder bricks hundreds of home 'smart' locks", 11 August 2017
[5] Consumer Reports, "Wink Tells Users: Pay $5 a Month or We'll Disable Your Smart Home Hub", May 2020
[6] 9to5Mac, "Wink accused of extortion over mandatory subscription policy", 13 May 2020

> ‡ **Not yet independently verified.** The four cases with numbered citations
> above have been checked against primary or contemporaneous reporting. The
> three marked ‡ are recounted from general knowledge and **must be verified
> and cited before external distribution, or removed.** A paper that criticises
> other vendors' conduct cannot afford an inaccurate citation, and the four
> verified cases carry the argument on their own if the others cannot be
> substantiated in time.

Sovereign Edge's design commitments follow directly:

- **No lock.** The protocol, server and app are open source (§7). The bridge's
  broker address is a configuration value, not a hard-coded endpoint. A
  customer can move to their own infrastructure at any time without our
  cooperation or permission.
- **No bounty.** There is no subscription required for the product to function.
  No feature that worked at purchase is later moved behind a payment. Support is
  sold as a service that must be re-earned, not as a ransom on functionality
  already bought.
- **No hold-up.** Because the software is open and the transport is a
  commodity, the customer's sunk investment is not leverage we hold. We
  deliberately do not occupy a position from which we could change the terms
  after the fact — including on ourselves, since the same openness means a
  future owner or board of this company cannot do so either.

**Native Matter support is removed from the roadmap.** Not deferred. A product
whose central claim is freedom from permission structures cannot itself depend
on annually renewed permission from a consortium of the four largest firms in
the sector. §4.6 sets out what replaces Matter's portability guarantee, and why
we regard the replacement as stronger.

---

## 2. Hardware Architecture

### 2.1 Mechanical platform and the five form factors

The lock's mechanical body — housing, handle, and clutch assembly — is sourced
from established hardware manufacturers and paired with a fully custom
electronics stack. Separating the mechanical shell from the digital brain
allows the mechanical supply chain to be treated as a commodity input while all
differentiated engineering value sits in the custom motherboard and firmware.

The launch range comprises **five mechanical form factors** sharing one
electronics platform, one firmware image and one app. Door hardware in the
Australian and Southeast Asian markets is not uniform — retrofit deadbolts,
lever-handle mortise sets, aluminium sliding doors and gate applications each
demand different mechanics while presenting identical requirements to the
electronics. Serving that variety from a single electronic platform is what
makes a 20,000-unit run economic; it pools volume across every segment rather
than fragmenting it.

> **Editorial note for completion:** the five form factors should be itemised
> here in a table (name, door type, target market, mechanical supplier). This
> was not specified at time of writing.

### 2.2 Lock electronics — the ESP32-C6

The production doorlock is built on a **modified Seeed Studio XIAO ESP32-C6**
carrier, module specified at **N8 (8 MB flash)**, retaining the 21.0 × 17.5 mm
outline. USB-C, charger and buck circuitry are removed and the regulator
replaced, with four grouped connectors — battery, doorlock MCU, programming,
and camera SPI — relocated to the freed bottom edge. Power is **four AA cells
at 6 V nominal**.

The move from the ESP32-H2 specified in version 2 to the C6 is deliberate and
worth explaining, because on its face it looks like accepting a more expensive
part.

The H2 carried Thread and BLE but no Wi-Fi, which v2 presented as an
attack-surface advantage. In practice the C6's tri-radio capability is what
allows **one lock SKU to serve every deployment mode**: a Thread-mesh
installation behind a bridge, and a standalone Wi-Fi installation with no
bridge at all for single-door customers who will not buy additional hardware —
subject to the important limitation that the standalone mode cannot perform
remote unlock, set out in full in §4.5.

Two silicon platforms would otherwise be needed to cover both, fragmenting the
volume that makes the BOM achievable in the first place. The C6 also permits
firmware convergence — the lock, the bridge and the bench tooling all run the
same core — and it is the enabling part for the video variant on the roadmap
(§11.2). The H2 could support none of these.

The attack-surface argument is preserved by configuration rather than by
component: in Thread deployments the Wi-Fi radio is not provisioned and is
never brought up. A radio that is never enabled presents materially the same
exposure as one that is absent, and unlike absence it can be enabled later if
the customer's deployment changes.

**Hardware review findings incorporated (R1–R5).** Five revisions from design
review are recorded here because two are safety-relevant to any party
reproducing this work:

- **R1.** UART0 RX moved to GPIO17. The ESP32-C6 ROM bootloader's
  serial-download interface is fixed at GPIO16/17 and cannot be remapped —
  remapping applies to application code, but download runs in ROM before any
  application exists. With USB-C omitted this is the board's only programming
  and recovery path; as originally drawn the board would have been
  unflashable with no fallback.
- **R2.** Production firmware must build with USB CDC disabled so that the
  serial console reaches the UART0 pins on the programming header.
- **R3.** The linear regulator consumes approximately 45% of every mAh and
  stops regulating around 3.6–3.8 V, stranding the tail of the discharge
  curve. For the prototype run the LDO is retained deliberately — the board
  already carries roughly ten departures from the proven reference design, and
  a switching inductor on a 21 × 17.5 mm outline is another first-run failure
  mode. A buck converter is to be evaluated for revision 2 by fitting an
  external module inline on the 6 V lead, requiring no respin. **The real
  end-of-life cutoff voltage is to be logged during bench validation**; §3
  treats this as an open figure rather than an assumption.
- **R4.** A production pin profile distinct from the bench profile is required
  before first power-on, with no LCD or touch peripheral.
- **R5.** The camera connector was widened to seven pins to carry a data-ready
  interrupt, since an SPI slave cannot initiate a transfer and polling a video
  path wastes battery and adds latency.

### 2.3 The bridge module

Remote connectivity is handled by a separate, physically distinct
mains-powered device rather than by adding always-on radio duty to the lock.
The bridge performs two roles simultaneously: **Thread border router** for the
lock mesh, and holder of the sole Wi-Fi/MQTT connection to the broker. Because
it is mains-powered it has no battery budget to protect and can maintain a
persistent connection the lock never has to.

The bridge is built on the **ESP32-C6-WROOM-1** module — a shielded,
pre-certified module with flash in package. This choice is compliance-driven
and is discussed in §9. Development and demonstration work uses an off-the-shelf
Waveshare ESP32-C6 board; the production bridge is a custom PCB, because a
development board built around a bare unshielded die carries no certification
to inherit and no design control (§9.2).

The bridge presents a **male USB-A connector** and draws power from any USB
source — a wall adapter, or a home router's USB port, which has been
bench-confirmed. No mains-connected power supply ships in the box; a short USB
extension is included instead. This removes an entire regulatory category from
the product (§9.3) and eliminates a component that would otherwise carry
electrical-safety liability.

Because the bridge is shared across every lock on its Thread mesh, the
economics improve with door count: a **4:1 lock-to-bridge ratio** is the launch
planning assumption, matching the 20,000/5,000 run split.

### 2.4 Defence and government tier — EFR32MG24

The upper tier is populated with a **Silicon Labs EFR32MG24** daughterboard,
whose PSA Certified Level 3 secure element and non-Chinese silicon origin allow
sales material to answer defence and government procurement security
questionnaires directly, shortening review cycles that would otherwise stall.

This tier exists because in defence and government procurement the secure
element is frequently the item actually being purchased — the buying criterion
is the certification of the key store, not the feature set of the lock. Buyers
in this segment are also the least price-sensitive and the most documentation-
sensitive of any served by this platform.

The modular daughterboard architecture — a standardised edge connector on the
core motherboard accepting either silicon platform — gives three practical
advantages: supply-chain resilience, since a semiconductor shortage can be
absorbed by swapping the daughterboard rather than retooling the lock; market
segmentation through a component swap rather than a separate product line; and
lifecycle longevity, since a superseded wireless standard or cryptographic
algorithm is a daughterboard replacement rather than a discarded lock.

> **Status:** the EFR32MG24 tier is specified and committed as a product
> direction. No hardware has been built at time of writing. §11 records this.

---

## 3. Power and Battery

The primary failure point of existing smart locks is battery life driven by
continuous network polling. Sovereign Edge's firmware targets a strict **Sleepy
End Device** model: the processor and radio remain in a collapsed deep-sleep
state for the overwhelming majority of operating time, waking only on a
hardware touch interrupt or a scheduled Thread poll.

- **Interrupt-driven wake** — a capacitive touch sensor triggers wake via a
  hardware GPIO interrupt rather than continuous polling; the processor
  authenticates and actuates the motor before returning to sleep.
- **Sleepy End Device Thread polling** — the radio checks in with its Thread
  parent on a scheduled interval, clearing queued messages before re-entering
  sleep. This interval is owner-configurable, not a fixed constant: the product
  default is 5 seconds, chosen to make remote unlock feel effectively instant,
  extendable to 15 minutes by owners who prefer battery life over latency.
- **Fully event-driven application logic** — no blocking loops or busy-waiting.

### 3.1 Honest status of the power model

Version 2 presented a battery model with a table of figures. That model is
**carried forward as a design target and is not yet valid for the shipping
firmware**, for three specific reasons that are stated here rather than left in
a footnote:

1. **The figures were derived for the ESP32-H2**, using that part's datasheet
   deep-sleep current. The platform now runs on the C6. The figures require
   re-derivation against C6 datasheet values and then bench measurement.
2. **The current firmware is not a Sleepy End Device.** It is configured as a
   Thread Full Thread Device with the radio receiving when idle. That is a
   fundamentally different power profile and the modelled figures do not apply
   to it. Implementing the SED mode is committed engineering work, not a
   completed state.
3. **The regulator strands part of the pack** (R3). An LDO that stops
   regulating around 3.6–3.8 V leaves usable capacity in the cells that the
   model's flat 80% derating does not correctly represent.

For reference, the v2 model — 4× AA lithium primary cells at 3,000 mAh
nominal, derated to 2,400 mAh usable, against 20 unlock events per day and a
fixed daily engineering margin — produced approximately 2.8 years at the
5-second default and approximately 4.0 years at the 15-minute setting, with the
relationship sharply nonlinear: most of the available battery life is recovered
by moving from 5 seconds to 30 seconds, at a latency cost few owners would
notice.

Two assumptions inside that margin remain worth naming. Any status LED must be
momentary — lit only during an active operation, never held on as a
connectivity indicator, since a continuously lit LED can draw upward of 1 mA,
several times the entire daily budget. And the poll figure counts only the
MAC-layer data-poll exchange; it does not separately account for periodic
parent re-attachment, MLE keepalives, or route churn, which are negligible at a
15-minute interval but may not be at 5 seconds.

**No battery figure in this paper should be quoted as achieved.** They are
design targets the hardware and firmware still have to earn. A revised model
with measured C6 figures will be published when bench validation on production
silicon is complete.

---

## 4. Network Architecture: Two Modes, Owner's Choice

Sovereign Edge supports two network modes rather than an expanding menu,
because every additional mode is another thing to secure, test and explain.

| Mode | How it works | Best for |
|---|---|---|
| **Mode 1 — Self-hosted** | The bridge is pointed at the owner's own MQTT broker — a Home Assistant Mosquitto instance, or any MQTT server. No Sovereign Edge account, server or cloud is involved at any point. The open-source server component may optionally be run by the owner for grant management and audit. **Locks self-advertise via MQTT Discovery**, so they appear automatically as Home Assistant entities, and full lock state is published (§4.7). | Owners and operators who want zero vendor dependency, technically capable institutions, and anyone integrating with Home Assistant |
| **Mode 2 — Hosted relay** | The bridge holds a persistent MQTT connection to a Sovereign Edge broker that relays live commands and stores only the pairing relationship between lock and app — never a transaction log. Payloads are end-to-end encrypted; the broker cannot read a command's contents, though it necessarily observes that one occurred (§4.1). | Owners who want remote unlock working out of the box, and operators who prefer a managed service with an SLA |

The two modes run **identical firmware and identical application code**. The
difference is one configuration value: the broker address. There is no
"community edition" with features removed, and no technical work is required to
move between modes — which is what makes the claim in §1.5 structural rather
than promissory.

### 4.1 The broker commitment

The broker's role is deliberately narrow. It holds a persistent session per
bridge so that a command sent while the bridge or lock is briefly offline is
queued and delivered on reconnect — solving a store-and-forward problem a bare
peer-to-peer connection cannot — and it relays end-to-end encrypted payloads it
cannot read.

What it does not do is log lock or unlock events. The pairing table is the only
durable state it keeps. For institutional deployments that legitimately require
an audit trail, that trail lives on the operator's own server layer — their
PMS, their infrastructure — not on a Sovereign Edge broker.

> **Current implementation gap, disclosed.** The server component as presently
> built does persist credential material in cleartext, including PIN values, and
> writes human-readable audit lines. This does not meet the commitment stated
> above. Closing it is the current top engineering priority and is tracked in
> §11. The commitment describes the architecture's design and destination; it
> does not describe the state of the code at the date of this paper.

#### What the hosted broker holds, precisely

"Stores nothing but the pairing relationship" is too loose to be audited
against. The following is the complete intended inventory for the hosted
service, stated so that a buyer can hold us to it rather than to a slogan.

| Data | Why it exists | Retention |
|---|---|---|
| Pairing table — which lock belongs to which account | Message routing | Life of the pairing |
| Account identity and billing record | Contract administration | Life of the account, then per statutory requirement |
| Undelivered message queue | Store-and-forward while a bridge is offline | Deleted on delivery; expires if undelivered |
| Connection metadata — source IP, connect/disconnect times | Inherent to operating a TCP service | **7 days**, then deleted |
| Authentication failures and security events | Abuse and intrusion detection | 90 days |
| Aggregate service health metrics | Operations | Indefinite, no per-device identifiers |

**Not held, at any retention:** which lock opened, when it opened, who opened
it, credential values, PIN or RFID data, names of credential holders, or
payload contents.

#### Metadata: the honest limit of "blind relay"

End-to-end encryption protects the *content* of a command. It does not conceal
that a command occurred. A hosted broker necessarily observes that a message of
a certain size was routed to a particular lock at a particular moment, and from
which IP address the bridge connected.

**For a door lock, that is not a trivial residue.** Timing alone can reveal
occupancy patterns — when a household leaves, returns, or admits a visitor at an
unusual hour — without any payload ever being decrypted. It would be dishonest
to call the broker "blind" without saying so, and this paper says so.

Three responses, in increasing order of strength:

1. **Connection metadata is retained for seven days and is never used for
   analytics, profiling, or product development, and is never sold, shared, or
   provided to third parties except under valid legal compulsion.** Message
   routing events are not written to durable storage at all.

   This is not offered as a promise to be taken on faith. It is **binding in
   the published privacy policy**, which restates the table above verbatim so
   that a departure from it is a breach rather than a change of practice. The
   broker configuration and the server source are **published**, so the
   retention behaviour is inspectable rather than asserted. We will publish an
   annual **transparency report** covering the number of legal-compulsion
   requests received, complied with, and refused. And any material change to
   the table above will be **notified to account holders in advance of taking
   effect**, so that a customer who objects can move to Mode 1 before the change
   applies rather than after — which is the specific courtesy Wink's customers
   were not given (§1.6).
2. **The bridge, not the lock, holds the connection.** One connection serves
   every lock behind it, so per-lock timing granularity is already reduced for
   multi-door households.
3. **Mode 1 removes the exposure entirely.** An owner who runs their own broker
   gives us no metadata because we are not in the path. This is the structural
   answer, it requires no trust in our retention policy, and it is available to
   every customer without penalty. Any buyer for whom metadata exposure is
   genuinely unacceptable should run Mode 1, and we will say so during the sale
   rather than after it.

The accurate claim is therefore narrower than "blind relay," and we state it in
the narrow form: **the broker cannot read what a command says or does. It
necessarily observes that one occurred.** The remedy for customers who need more
than that is not a stronger promise from us; it is Mode 1.

#### No remote disable

There is no kill switch. No firmware path exists by which the company can
render a fielded lock inoperable, and none will be added. Locks continue to
function over BLE at the door, and bridges over the local network, with no
server reachable at all (§4.3) — so withdrawal of the hosted service degrades
convenience and cannot remove access.

This commitment is written because of a specific precedent rather than an
abstract concern. On **8 August 2017 LockState pushed a firmware image intended
for one lock model to a different model, bricking at least 500 locks**, around
200 of them in Airbnb properties, leaving hosts unable to admit guests and
requiring the units to be physically returned (§1.6). Nobody intended it: the
wrong file went to the wrong device list. That is exactly the point — **a lock
that can be disabled remotely will eventually be disabled remotely, by error
long before by malice.**

Three mitigations follow, and note that the second is the one that would have
saved LockState's customers:

1. A residential owner can always reflash (§7.4).
2. **The local and BLE paths do not depend on any update having succeeded.** A
   lock with corrupt application firmware still opens to a key and, by design
   here, still opens at the door. LockState's customers retained physical keys
   for this reason; a design that removes the mechanical fallback removes the
   last thing that works when software fails.
3. Firmware source is public, so a bad image is diagnosable — and a fix
   buildable — by someone other than us.

**The strength of this guarantee differs by tier, and the difference must be
stated rather than averaged.**

On **residential and hospitality** units it is **structural**. The secure-boot
eFuse is not burned, so the owner can reflash (§7.4). Even a deliberately
hostile update is recoverable by the owner or by anyone they hire. We are not
in a position to disable these devices permanently, whatever our intentions.

On **defence and government** units it is **contractual, not structural** — and
this is an honest limitation, not a technicality. Secure boot is burned at the
buyer's own requirement, which necessarily means the device will run only
firmware we have signed. A buyer in that tier is trusting us in a way a
residential buyer is not. We do not claim otherwise, and we would regard any
vendor claiming a structural guarantee on a locked-bootloader device as
misrepresenting it.

Three things bound that trust:

1. **The update is auditable before acceptance.** Firmware source is published,
   and this tier's deployments are expected to review and approve updates rather
   than accept them automatically.
2. **Reproducible builds.** We commit to a build process that allows a buyer to
   compile the published source and verify that the resulting binary matches
   the signed image we supply — pinned toolchain version, deterministic flags,
   and normalised build paths and timestamps, with the procedure documented.
   Stated honestly: bit-for-bit reproducibility on embedded toolchains takes
   real effort to achieve and to keep working, and it is a commitment we are
   making rather than a property we have already demonstrated. §11 records it as
   outstanding.
3. **Escrow and continuity terms** are available in the contract for this tier,
   including signing-key escrow arrangements where the buyer's own security
   review requires them.

A defence buyer who finds contractual assurance insufficient has a real option
available: purchase the residential-tier electronics, which do not enforce
secure boot, and take on firmware control entirely. That is a legitimate
configuration and we will supply it.

### 4.2 Payload security

Transport encryption protects against a broker that can read traffic. It does
not, by itself, protect against a broker that has been compromised and used to
inject or replay commands. Sovereign Edge closes that gap at the application
layer, below and independent of TLS.

The design provides for a per-pairing key established between app and lock by
an **X25519 key agreement** performed during pairing, from which an
**AES-256-GCM** envelope is derived. Each control payload carries a monotonic
counter embedded in the nonce and authenticated additional data binding it to
its intended lock, giving both authenticity and replay resistance. A
compromised broker can refuse to deliver a command, or delay it, but it cannot
forge or replay one, because it holds no key material at any point.

**Status.** The envelope implementation is complete and verified against frozen
byte vectors, including tamper and replay tests, on the application side. The
key-agreement ceremony that provisions it at pairing time **is not yet wired
into the pairing flow**, and the lock-side AES-GCM primitive is not yet
present in firmware. Until that work lands, deployed systems operate on a
plaintext transport. This is stated plainly because §11 exists to prevent
exactly the kind of overclaiming that version 2 contained.

### 4.3 Local failover: staying operational without the cloud

Mode 2's bridge is designed to keep working through outages that take
cloud-dependent competitors offline entirely. If the connection to the broker
drops — an ISP outage, a broker incident, a firewall change — the bridge falls
back to serving commands directly over the local network: a phone on the same
Wi-Fi reaches the bridge at its local address and issues the same authenticated
commands, with no round trip to the internet. If mains power and the local
router both fail, the lock's BLE radio remains available as a last-resort
proximity path, letting a nearby phone pass an authenticated unlock over a
short-range link requiring no network infrastructure at all.

Each fallback layer requires strictly less infrastructure than the one above
it — cloud, then local network, then direct proximity — so an owner's
worst-case failure mode is never a locked-out door, only a shrinking radius of
how close they must be to open it themselves.

### 4.4 Bridge continuity and mesh restoration

A Thread mesh is defined by credentials held by its border router. A bridge
that is factory-reset or replaced would, naively, form a new network and orphan
every lock joined to the old one — requiring a physical re-pairing visit to
every door in the building.

The bridge therefore accepts an existing Thread operational dataset at
provisioning time and restores that mesh rather than forming a fresh one. The
app already holds this dataset, having read it from the bridge when the mesh
was first created. A replacement bridge is provisioned with the saved dataset
and every existing lock rejoins autonomously, with no door visits. The same
mechanism allows a second bridge to be added as a range extender.

This also matters for the sovereignty claim: the Thread network's key material
exists on the bridge and in the owner's app, and nowhere else. Restoration is
an owner-held capability, not a support request.

### 4.5 Standalone Wi-Fi: what it is, and what it is not

The C6's Wi-Fi radio allows a lock to operate with no bridge and no Thread mesh.
This configuration is offered for single-door customers who will not purchase
additional hardware, and it must be described precisely, because it is **not the
same product with reduced performance — it is a different capability set**.

**A standalone Wi-Fi lock cannot perform remote unlock.**

The reason is a power budget, not a software limitation. A battery-powered lock
cannot hold a persistent Wi-Fi association without destroying its battery life;
Wi-Fi association and DHCP renewal are expensive operations, and a lock that
maintains them continuously is the three-to-six-month failure mode this entire
platform exists to avoid. The standalone lock therefore wakes on a long
interval — of the order of two to ten minutes — which is adequate for
background synchronisation and useless for "let the courier in right now."

What the standalone configuration does provide:

| Capability | Standalone Wi-Fi | Bridge + Thread |
|---|---|---|
| Unlock at the door (BLE, phone in hand) | Yes | Yes |
| Keypad / PIN entry | Yes | Yes |
| **Remote unlock from anywhere** | **No** | Yes |
| PIN and credential sync | Yes, on wake interval | Yes, near-immediate |
| Audit upload | Yes, on wake interval | Yes, near-immediate |
| Credential revocation | Yes, delayed to next wake | Near-instant (§6) |
| Typical wake interval | 2–10 minutes | 5 s – 15 min, owner-set |

For a great many single-door households this is genuinely sufficient: the phone
in the owner's pocket unlocks the door over BLE as they approach, and the Wi-Fi
link exists to keep credentials and audit current rather than to carry live
commands.

It is nonetheless a real limitation with an operational consequence worth
stating for portfolio operators: a credential that must reach a lock before a
guest arrives depends on the next wake. **Delivery confirmation, not merely
"queued", must therefore be surfaced to the operator** — a missed wake that
leaves a guest without a working PIN at 11pm is the characteristic failure of
this mode, and the product's job is to make that visible before it happens
rather than to explain it afterwards.

Marketing and packaging must state the remote-unlock limitation plainly. A
customer who buys a standalone lock expecting to open their door from the office
has been mis-sold, and no amount of correct behaviour afterwards repairs that.

### 4.6 What portability means without Matter

Version 2 grounded its portability claim in Matter: the lock would keep working
inside an Apple, Google or Amazon ecosystem even if this company disappeared.
With Matter removed, that guarantee has to be met another way, and the
replacement is stronger.

Matter's guarantee was: *if we vanish, a consortium's standard keeps your lock
working* — contingent on that consortium's continued approval, purchased
annually, and revocable by a body we do not control.

The open-source guarantee is: *if we vanish, here is the source code, the
protocol specification, and the server. Run it yourself, or pay anyone you
choose to run it for you.*

The second is available without anyone's permission, costs nothing to exercise,
survives our insolvency or acquisition, and is **verifiable today** rather than
promised. A customer does not have to trust that we will still be here; they
can read the licence.

There is a cost, and it is narrower than it first appears.

A Sovereign Edge lock does not appear **natively** in Apple Home, Google Home
or Alexa — it cannot be commissioned directly into those ecosystems, and we do
not claim otherwise. A customer who wants a lock that pairs straight into Apple
Home from the box should buy a Matter product, and we will say so during a sale.

**But the ecosystems are reachable, without any certification, through Home
Assistant.** HA's **HomeKit Bridge** integration presents selected entities to
Apple's Home app over the **HomeKit Accessory Protocol** — the same mechanism
Homebridge has used for years, requiring no Apple certification and no
membership of any body. The full path is:

> doorlock → Thread → bridge → MQTT → Home Assistant → HomeKit Bridge → Apple
> Home (with an Apple TV or HomePod serving as the Home hub for remote access)

**There is no Matter anywhere in that chain.** HA's Google Assistant and Alexa
integrations reach the other two ecosystems by an equivalent route, with more
configuration.

Three honest qualifications:

- **It requires the customer to run Home Assistant.** This is not out-of-box
  ecosystem support; it is support for customers who have chosen to run their
  own hub — which is the same population Mode 1 already serves.
- **We cannot use the branding.** "Works with Apple Home" is Apple's trademark
  and requires their certification programme. We describe the product as
  compatible with Home Assistant and leave the onward path to be documented by
  the community, which is entitled to describe it in ways a vendor is not.
- **We do not control that chain.** HA could change its integration; Apple
  could tighten HAP. The distinction that matters is that the dependency is
  **additive rather than structural** — if it breaks, the lock continues to
  work exactly as before, because nothing in its operation passes through it.
  Under Matter the dependency sits in the *commissioning* path: no valid
  certificate, no working device. That is the difference between an integration
  and a permission.

The net position is therefore stronger than the one Matter would have bought.
We decline the certification regime, pay nothing, ask no one's approval — and a
customer who wants their lock in Apple Home can still have it.

**Source:** Home Assistant HomeKit Bridge integration documentation,
`home-assistant.io/integrations/homekit/`. Note the configuration constraint
that applies specifically to locks: they must be exposed in `accessory` mode
with a single-entity include filter rather than the default bridge mode. This
belongs in the setup guide — users who miss it will conclude the lock is at
fault.

### 4.7 Home Assistant as the supported integration path — and why state stays local

Home Assistant is the platform this product integrates with as a first-class
commitment rather than an incidental compatibility. The bridge publishes **MQTT
Discovery** configuration so that locks appear automatically as HA entities
without the owner hand-writing configuration, alongside lock state, availability,
and battery reporting.

**This capability is deliberately confined to Mode 1, and the reason is the
commitment in §4.1 rather than a commercial one.**

Home Assistant's lock entities require **retained** MQTT state topics — and a
retained MQTT message persists on the broker by design; that is what retention
means. Publishing lock state through a Sovereign Edge-operated broker would
therefore leave "which lock, in what state, at what time" sitting durably on our
infrastructure. That is exactly what the data inventory in §4.1 states is never
held, at any retention. A privacy commitment that is suspended whenever it
becomes inconvenient for a feature is not a commitment.

The division is therefore:

| | Mode 1 — own broker | Mode 2 — hosted relay |
|---|---|---|
| Commands relayed | Yes | Yes |
| MQTT Discovery / automatic HA entities | **Yes** | No |
| Retained lock state topics | **Yes** | **No — would breach §4.1** |
| Availability and battery reporting | **Yes** | Availability only, not retained |
| Who holds the state history | The owner | Nobody |

An owner who wants full Home Assistant integration runs their own broker, which
costs them a Raspberry Pi they very likely already own if they are running HA at
all. An owner who wants managed convenience uses the hosted relay and accepts
that it carries commands only.

This means Mode 1 is not merely the ideologically purer option — **it is the more
capable one**, and we would rather say that plainly than pretend the two modes
are equivalent. The customer who most values their privacy also gets the better
integration, which is the alignment this architecture was built to produce.

> **Status and priority.** Not yet implemented, and **deliberately last in the
> development queue** (§11.1, step 9). The core products — a lock and app that
> work out of the box, a bridge for speed, OZKEY for hospitality, OZPMS for
> portfolio operators — come first, because that is where the customers and the
> revenue are, and because the average buyer neither can nor should have to
> install Home Assistant.
>
> What this does *not* delay is self-hosting itself. **Mode 1 works today over
> plain MQTT** — an owner running their own broker can command these locks with a
> few lines of configuration. MQTT Discovery removes the configuration step; it
> does not create the capability.
>
> The remaining work: the bridge currently has no MQTT publish path at all, and
> the doorlock has no state uplink over Thread — the relay is one-directional.
> The **state uplink is scheduled early (step 4) as core product work**, not as
> part of this section, because the app needs lock state, unlock confirmation and
> battery level regardless of Home Assistant. Once it exists, the publish path,
> lock roster and Discovery payloads are a small increment. The lock roster is
> already the agreed architecture — the bridge is specified to hold the routing
> table — so it adds a use for state the bridge is meant to hold rather than new
> state.

---

## 5. Deployment Tiers

The same lock-and-bridge hardware serves three of the four tiers. What differs
is the server layout and the software configuration, not the silicon.

| Tier | Deployment | Server layer |
|---|---|---|
| **Residential** | One to five doors, one bridge | None required; optional self-hosted or Sovereign Edge relay |
| **Hospitality / PMS** | Portfolio scale — short-stay rentals, small hotel and motel groups, university and mining-site accommodation | Operator-hosted (local or cloud) providing roster sync, at-the-door guest pairing, and audit trail |
| **Defence / Government** | Strata, facilities panels, secured sites | Operator-hosted, on-premises, air-gapped where required |

Thread's mesh networking lets a modest number of bridges serve many locks
across a building or site rather than requiring one bridge per door, which is
what makes the hospitality tier's economics work at a 4:1 or better ratio.

The hospitality tier is where the sovereignty proposition converts most
readily, and for a reason distinct from consumer privacy sentiment: an operator
holding guest access logs on a foreign vendor's cloud has an Australian Privacy
Act exposure, and a GDPR exposure the moment a European guest checks in. "The
data never leaves your property" is a line item a legal review understands and
a budget holder can justify. Businesses purchase compliance where consumers
will not purchase privacy.

---

## 6. Credential Model and Revocation

A recurring question in institutional procurement is not which chip is inside
the lock, but who ultimately controls it once installed. Two scenarios
illustrate: a building manager's employment ends but their phone still holds
valid credentials; or a managing agent is replaced and the owners' corporation
must reclaim control of every door without the outgoing agent's cooperation.

Sovereign Edge's answer is an **owner-root delegation model**. Every downstream
credential — a building manager's, a cleaning contractor's, a guest's — is not
a permanent pairing but a token signed by the property's root key with a
bounded validity window: short for high-turnover access such as a guest code,
longer for a building manager's daily credential. The lock verifies the
signature and checks expiry **offline**, against a root public key it already
holds. It never needs to fetch a revocation list to know a credential has
lapsed, because an unrefreshed credential simply expires on schedule.

Revocation runs on two layers, mirroring how §4.3 layers network failover. The
**fast path** is a best-effort push: whenever the lock has any path to the
outside world, a revoke instruction reaches it at its next scheduled poll,
making revocation near-instant in the overwhelming majority of real cases. The
**guaranteed path** is the bounded-expiry token itself, for the case the fast
path cannot reach — a lock isolated with no network path of any kind. In that
case only, a departed party could operate for at most the remaining life of
their last valid token: a deliberate, disclosed window the property owner sets
per credential class, not an accidental gap.

**Three dependencies are named rather than assumed.**

*The clock.* Offline expiry checking requires the lock to know the time. The
clock is set from NTP when the lock has network reach and holds over gaps using
its RTC. The honest failure mode is a clock drifted or reset by extended power
loss, which can wrongly honour an expired token or reject a valid one until the
lock re-syncs. This is boundable — a clock error measures in hours, not the
unbounded exposure a missing revocation list would create — but it is real.

*The window is a genuine trade-off, in both directions.* A short window tightens
the worst-case revocation bound, but also means a legitimate credential holder
without connectivity past their token's expiry is locked out until they can
refresh. This is why the window is set per credential class rather than fixed
platform-wide.

*The root key itself.* The root keypair is generated locally at property setup
and never transmitted to or held by any Sovereign Edge server — consistent with
every other commitment here, but meaning that losing the device holding it is a
genuine risk. The setup flow generates a human-readable recovery phrase
alongside the keypair, in the pattern established by cryptocurrency wallets,
for storage independent of any Sovereign Edge system. For institutional buyers
the root supports delegating a second, co-equal root holder at setup, so
control of a property is never contingent on one person's phone surviving.

> **Status.** This section describes designed and specified behaviour. The
> token format, expiry semantics and delegation model are defined; the
> implementation depends on the key-agreement work described in §4.2 and is not
> yet live. Version 2 of this paper described this system as operational. That
> was incorrect and is corrected here.

---

## 7. Open Software and the Support Business

### 7.1 What is open

The server, the application, and the protocol specification are published under
an open licence. This includes the credential model, the envelope format and
its test vectors, the MQTT topic structure, and the bridge provisioning
protocol.

There is no feature-reduced community edition. The published software is the
software the hosted service runs.

Proposed licensing, subject to final legal review: **AGPL** for the server, so
that a party offering a modified version as a network service must publish
their modifications, preventing the code being taken closed by a better-funded
competitor; a **permissive licence** for the mobile application and client
libraries, since copyleft terms conflict with app-store distribution and the
client benefits from unrestricted integration; and an **open specification**
for the protocol itself, so that third-party implementations are legitimate
rather than tolerated.

Firmware is distributed as signed binaries with source available. Signing is
retained not to restrict the owner but to prevent unauthorised images being
loaded onto units in the supply chain (§8).

### 7.2 Why open makes the support business honest

The commercial model is hardware plus **support and SLA**. The software is not
the product.

This is not altruism, and the paper does not present it as such. It is the only
structure under which the §1.5 commitments are credible. A company that sells
support for software its customers cannot leave is not selling support; it is
collecting rent, and the customer's inability to leave is the actual product.
Once the software is open and self-hostable, every dollar of support revenue
has to be re-earned against the customer's live alternative of walking away.

That constraint is the point. It disciplines pricing, it disciplines service
quality, and it makes the relationship a mutual one. It also means the
company's incentives and the customer's remain aligned after the sale, which is
precisely where the incumbent model breaks.

Third parties may sell support for this platform, and this is expected and
welcomed rather than tolerated. An integrator with a regional customer base, or
an IT contractor already serving a hotel group, is often better placed to
support that customer than a five-person company in another country. A market
in support is the strongest possible guarantee of continuity for the buyer: it
means service does not depend on our survival.

### 7.3 Delivering tiered support from a five-person company

eBizco Australia is a five-person company. Supporting a 20,000-unit deployment
across consumer, hospitality and government tiers would ordinarily require an
organisation many times that size. The approach is to use AI assistance to
extend a small expert team's reach rather than to replace it.

The durable asset is not the AI. It is the **corpus**: the published source,
the protocol specification, the engineering decision record, the failure modes
already diagnosed and documented. An AI assistant is a retrieval and reasoning
layer over that corpus. Where the corpus is thorough, first-line support scales
almost without headcount; where it is thin, no amount of AI compensates. This
is why the engineering documentation discipline evident throughout this paper
is a commercial asset and not an indulgence.

What this genuinely delivers: first-line and second-line technical support at a
scale disproportionate to headcount, continuous availability across time zones,
and — importantly for a small company — institutional knowledge that does not
leave when a person does.

What it does not deliver, stated so that no buyer is misled: an AI assistant
cannot be accountable under an SLA, cannot authorise a warranty replacement,
cannot manage RMA logistics, and cannot make a judgement call in a security
incident. Those remain human responsibilities and are staffed accordingly.
Support tiers are structured so that escalation to a named human is always
available and is contractually bounded in the SLA. The AI extends the team's
reach; it does not stand in for its accountability.

### 7.4 Open source and signed firmware: resolving the tension

Publishing source under an open licence while enforcing signed firmware is, on
its face, a contradiction: a customer who builds the published source cannot run
it if the device refuses unsigned images. An open licence that grants the right
to modify software you are then prevented from installing is not open in any
sense that matters. The tension is real and this section resolves it explicitly
rather than leaving it to inference.

**What we explicitly reject.** One available resolution is that customers submit
builds for the company to sign. We decline it. That is precisely the permission
structure this paper objects to in §1 — a manufacturer's device running only
what a central authority has approved — and adopting it here while criticising
it there would be indefensible.

**The resolution is to stop conflating two separate problems.**

*Supply-chain integrity* is the concern in §8.1: preventing an assembly partner
building surplus units from the same files and selling them outside the
agreement. **This does not require boot restriction.** It is solved by
**per-device identity provisioned under company control** — credentials injected
by us, not by the assembly partner. A copied board boots perfectly well and
cannot enrol with any server, pair with any app, or join any mesh. The clone
fails functionally rather than being prevented from starting, which achieves the
commercial objective without taking anything from the legitimate owner.

*Firmware authenticity in transit* is a different concern: preventing a
compromised update server or a network attacker from pushing malicious firmware
to a fielded device. This is solved by **signature verification on the OTA
path** — the device verifies an image received over the network before installing
it. Note this is signed-image verification, not eFuse-burned secure boot: it
protects the remote update channel without restricting what a physically present
owner may install.

**The owner's path.** On residential and hospitality units, the hardware secure
boot eFuse is **not burned**. An owner who builds the published source can flash
it over the programming header, documented in the product materials.

The threat model justifies this rather than merely tolerating it. The programming
header sits on the **interior escutcheon** — the inside face of the door. An
attacker with the physical access required to reach it is already inside the
building and has no need to reflash anything. Enforcing secure boot on that
interface would protect against essentially no realistic attack while removing
the owner's control over a device they own. We judge that a bad trade and decline
to make it.

**The defence and government tier is deliberately different.** That tier ships
with full hardware secure boot enabled and the key digest burned to eFuse,
because its procurement requirements demand it and its threat model genuinely
includes supply-chain interdiction and physical tampering by capable adversaries.
A buyer in that segment is purchasing a device that will not run unauthorised
firmware, and that is the correct product for them.

This asymmetry is disclosed rather than hidden. A customer who wants a lock they
can rebuild and reflash should buy the residential or hospitality unit; a
customer who wants one that refuses anything unsigned should buy the defence
tier. Both are legitimate; what would not be legitimate is shipping the second
while describing it as the first.

**What this guarantee is actually worth to a non-technical owner.** It would be
dishonest to present "you may rebuild the source and flash it over the
programming header" as a practical protection for a typical residential
customer. Almost none will ever compile firmware, and presenting a developer
capability as a consumer safeguard is the kind of overclaim §11 exists to
prevent.

The rebuild path is a **structural** guarantee, and its beneficiaries are
mostly indirect. What it actually does is make a competitive market possible:
an integrator, a competitor, a security researcher, or a future maintainer can
take over this platform without our cooperation. The individual owner benefits
from that market existing, not from personally exercising the right.

For the non-technical owner, three concrete protections carry the weight
instead, and these are the ones worth putting in front of a consumer:

1. **The lock keeps working with no company and no internet.** BLE at the door
   and local-network control require nothing from us (§4.3). Our disappearance
   is an inconvenience, not a lockout.
2. **They can pay someone else.** Support is a competitive market by design
   (§7.2), not a channel we control.
3. **The credentials are already theirs.** The root key is generated on their
   device and never held by us (§6), so there is nothing for us to withhold.

Stated plainly: open source protects the *ecosystem*, and the ecosystem
protects the individual. We would rather say that than imply a homeowner's
sovereignty depends on their ability to use a toolchain.

### 7.5 Support tiers and service levels

| Tier | Intended for | First response | Escalation | Pricing |
|---|---|---|---|---|
| Community | Self-hosting owners, developers | Best effort, public issue tracker | None guaranteed | Free, permanently |
| Standard | Residential, small operators | Next business day | Named human within 3 business days | Published at launch; annual |
| Professional | Hospitality / PMS portfolios | 4 business hours | Named human same day | Published at launch; annual per site or per door |
| Critical | Defence, government, secured sites | 1 hour, 24/7 | Named engineer, contractually bounded | Negotiated per contract |

Response times above are the committed service levels. **Pricing is published
at launch rather than in this document**, and will be listed publicly rather
than quoted per customer for the Standard and Professional tiers — a published
price list is itself an anti-hold-up measure, since it prevents renewal pricing
being set against a customer's accumulated switching cost.

Two commitments apply across all paid tiers regardless of the numbers finally
set. First, **AI-assisted first-line response never blocks escalation**: a
customer may request a human at any point without justifying the request, and
the SLA clock does not reset on escalation. Second, **an incorrect AI-generated
answer is treated as a support failure and a documentation defect** — the
corpus is corrected, and the correction is published, since that corpus is
public. A wrong answer that survives to be given twice is an engineering bug,
not a service inconvenience.

Community-tier support is genuinely unpaid and genuinely unguaranteed, and
self-hosting customers are not disadvantaged in any technical respect for
choosing it. That is the point of §7.2: the paid tiers must be worth buying on
their merits, because the alternative is always available.

### 7.6 Continuity

No individual and no company is permanent. Every architectural choice in this
paper is made so that the platform outlives the company that built it: the
protocol is published, the server is open, the key material is owner-held, the
Thread mesh can be restored by its owner, and the support market is open to
competitors. A customer's investment in Sovereign Edge hardware is not
contingent on eBizco Australia continuing to exist.

That is the strongest form of the sovereignty claim, and it is the one the
company is willing to be held to.

---

## 8. Ethical Manufacturing: The Sanctuary Enterprise Model

Final assembly and firmware provisioning are carried out through a
training-and-employment partnership based inside a managed technology park in
Vietnam, operating as a straightforward international B2B services arrangement:
the Australian parent company contracts and pays the Vietnamese facility for
engineering and assembly labour, with all payments made through standard, fully
invoiced international bank transfers.

The facility doubles as a technical training academy. Recruitment deliberately
does not filter on university credentials, instead using aptitude testing for
logical reasoning and precision manual work, and draws candidates from
underemployed young workers and gig-economy drivers. Trainees are taught
surface-mount assembly, precision rework, and hardware debugging to an
internationally benchmarked standard, and are paid competitively above the
typical local factory-floor wage, with full health coverage.

Commercial revenue from the Australian launch is intended, in part, to sustain
the ongoing training academy — making ethical manufacturing not a marketing
claim layered on afterward, but the funding mechanism the business model is
built around.

### 8.1 Employment status of trainees

> **Requires the operator's confirmation before publication.** The statements
> below describe the intended arrangement. Each is a factual claim about labour
> practice and must be verified against the actual employment contracts and
> Vietnamese labour law before this paper is distributed externally. Do not
> publish this section on the strength of intent alone.

A facility that is simultaneously a training academy and an assembly line
invites a specific and legitimate question, because the combination has
historically been used to obtain unpaid labour under an educational
description. The question is asked here rather than waited for.

- **Trainees are paid employees from day one**, not students, interns, or
  unpaid apprentices. Training time is paid working time at the same rate as
  production time.
- **Output produced during training is production output**, and is compensated
  as such. There is no category of work that is unpaid because it was
  performed while learning.
- **There is no training bond, no cost-recovery clause, and no repayment
  obligation on departure.** A worker trained at the facility may leave at any
  time and take those skills to a competitor, including immediately. Training
  that a worker must buy their way out of is a debt instrument, not an
  opportunity, and the model does not use one.
- **Employment terms are documented in Vietnamese**, in writing, with standard
  statutory entitlements including social insurance, and are not conditional on
  the Australian parent's commercial performance.

The distinction between education and job-specific instruction is not the
material one — both are legitimate. The material questions are whether the
worker is paid, whether they are free to leave, and whether the qualification
travels with them. On this model the answers are yes, yes, and yes, and a buyer
or auditor is entitled to verify them.

### 8.2 Supply-chain integrity

Two provisioning measures protect both the customer and the manufacturing
partnership, and are worth stating because they are the counterpart to open
software:

**Per-device identity provisioned under company control.** Device credentials
are injected by the company or a trusted party, never by the assembly partner. A
copied board boots normally and cannot enrol with any server, pair with any app,
or join any mesh. This — not boot restriction — is the primary anti-clone
mechanism, for the reasons set out in §7.4.

**Signed OTA images.** Firmware delivered over the network is signature-verified
before installation, protecting the update channel against a compromised server
or a network attacker. On residential and hospitality units this is
signed-image verification only; the hardware secure-boot eFuse is not burned,
and an owner may build and flash the published source over the interior
programming header. The defence and government tier ships with full hardware
secure boot enabled. §7.4 explains why the two tiers differ and why that
asymmetry is disclosed rather than quietly applied.

**Device key material is not extractable.** Provisioned per-device secrets are
stored so that they cannot be read back over the programming interface. Note the
deliberate distinction from a blanket debug-port lockout: the owner retains the
ability to flash firmware, while the device's provisioned identity remains
protected. Version 2 specified permanently disabling debug ports after flashing;
that would foreclose the owner's rebuild path in §7.4 and has been narrowed to
key-material protection accordingly.

---

## 9. Regulatory Compliance Posture

### 9.1 What we certify, and what we decline to

Sovereign Edge products carry the **RCM (Regulatory Compliance Mark)** for
Australia and New Zealand, covering electromagnetic compatibility and radio
spectrum compliance, and the electrical safety requirements applicable to the
products as supplied.

Sovereign Edge does **not** carry Matter certification, for the reasons in §1.
We regard this as a considered position rather than an omission, and state it
in product documentation rather than leaving buyers to discover it.

The distinction matters and is worth drawing explicitly: **RCM is a legal
requirement to sell a radio product in Australia; Matter certification is
commercial permission to interoperate.** We comply fully with the first. We
decline the second because it is a permission structure rather than a safety
one, and because the whole architecture is built to avoid depending on
permissions that can be withdrawn.

### 9.2 Why the production bridge is a custom board

RCM is a **supplier declaration**, not a certificate that can be purchased. The
Responsible Supplier must be an Australian legal entity, must hold a compliance
folder of test evidence for five years, and carries the liability. Test evidence
may legitimately be produced by an overseas laboratory provided it is
accredited and tests against the **AS/NZS** standards specifically — an FCC
report is not sufficient and does not convert.

Two consequences shaped the hardware:

A **pre-certified shielded module** (ESP32-C6-WROOM-1) allows the radio
transmitter testing to be inherited, leaving end-product emissions testing as
the remaining obligation. A development board built around a bare unshielded
die carries nothing to inherit and requires full radio testing — and, more
seriously for an unshielded design with a high-speed external flash bus, is at
material risk of failing emissions rather than merely costing more to test.

**Rebadging a third-party development board would make us the Responsible
Supplier for a design we do not control.** Such boards are revised without
notice; a silent layout or component change invalidates a declaration made
against the tested configuration. For a product that opens doors, holding
compliance liability for a design that can change underneath us is not
acceptable. The production bridge is therefore our own PCB.

### 9.3 No mains-connected component

The bridge draws power over USB and no power adapter ships in the box. A
bundled 5 V adapter would be mains-connected equipment, pulling the product
into electrical-safety registration with its own Responsible Supplier liability,
in a category with a poor compliance record among low-cost imports. Supplying a
USB extension instead removes the category entirely rather than managing it.

Product documentation specifies a USB power adapter or a router USB port.
Television USB ports are not recommended: many cut power in standby, which
would silently disable a household's Thread gateway.

---

## 10. Go-to-Market

### 10.1 Launch volume and pricing

The initial production run is **20,000 locks across five mechanical form
factors and 5,000 bridges** — a 4:1 attach ratio reflecting expected
multi-door deployment.

| Product | Retail (AUD) |
|---|---|
| Doorlock (any form factor) | $120 |
| Bridge | $50 |
| Lock + bridge starter bundle | bundled below the sum of parts |
| Defence / government lock (EFR32MG24) | mid-$300 range |

This positions the consumer lock well below legacy competitors' $199–450 AUD
price points. Pricing is deliberately aggressive: the strategy is market share
and installed base, not first-run margin.

**The bridge is required hardware** for a Thread deployment, and this paper does
not claim otherwise. Version 2 claimed "no mandatory hub tax" on the strength of
a Matter mode that no longer exists — and, as §1.4 sets out, that claim was
inverted: the Matter path requires a $150–300 AUD third-party controller, not no
hub at all. The accurate claim is narrower and more defensible. Every path to
remote unlock in this market requires a box in the home. Ours costs **$50, is
owned outright, carries no subscription, and can be pointed at the customer's
own infrastructure at any time**. What is absent is not the hub — it is the
lock-in, and the ecosystem purchase that would otherwise come with it.

Single-door customers who will not purchase a bridge are served by the
standalone Wi-Fi configuration, **which cannot perform remote unlock** — see
§4.5 for exactly what that configuration does and does not provide. This is
stated in packaging and product listings, not left for the customer to discover.
A bridge is required for the remote-unlock experience this product is primarily
sold on.

### 10.2 Channel

| Tier | Channel | Sales motion |
|---|---|---|
| **Maker / self-hosting** | Community, crowdfunding, Home Assistant ecosystem, direct | Self-serve; open firmware, documentation, and full HA integration |
| Consumer | DIY hardware retail, direct e-commerce | Self-serve; price and out-of-box remote unlock |
| Hospitality / PMS | Direct B2B; PMS platform integration partnerships | Consultative; fleet economics and integration |
| Defence / Government | Direct B2B; facilities panels; security integrators | Consultative; compliance documentation, pilots |

**The maker and self-hosting channel is the pre-launch channel, and it is
deliberate rather than incidental.** Interest in this platform has arrived first
from people wanting a low-cost, open, DIY-serviceable lock — which is not a
coincidental overlap. It is the same property list this paper argues for,
recognised by the segment that already knows how to value it.

Three things make it strategically valuable beyond its unit volume:

- **It validates demand and funds inventory before retail.** A production run of
  this size requires substantial working capital committed before any retail
  revenue arrives, against 60–90 day payment terms. Pre-committed community
  demand inverts that cash flow.
- **It is the ecosystem that §7.2 and §7.4 depend on.** Those sections argue
  that open source protects the ecosystem and the ecosystem protects the
  individual owner. A real community of people building, running, documenting
  and supporting this platform is that argument becoming true rather than
  asserted. It is also the practical answer to "who supports this if you
  disappear."
- **It substitutes for the certification mark we declined.** A product whose
  source is public and whose users can verify its claims has a different kind of
  credibility from a badge — and one better matched to this positioning.

Three disciplines apply.

**The product is not redesigned for this channel**: the maker configuration is
the same PCB with headers populated and no enclosure, at community-tier support,
not a different lock.

**Nothing is pre-sold before the RCM declaration exists** (§9, §11.1) — a
crowdfunding campaign that under-delivers would damage this company
disproportionately, because not betraying a customer after they have committed
is the entire proposition.

**And this channel does not set engineering priority.** It is served by published
source and hardware documentation, both already in progress, not by integration
features. Mode 1 self-hosting works over plain MQTT today. The convenience layer
for Home Assistant is scheduled last (§11.1) precisely so that a vocal minority
channel does not pull development ahead of the core products — the lock, the app,
the bridge, OZKEY and OZPMS — where the customers and the revenue actually are.
A five-person company can afford to serve this community generously with
documentation; it cannot afford to let it set the roadmap.

The consumer tier is the volume and cash-flow engine. The defence and
government tier is the margin and credibility engine — a single portfolio
contract generates more revenue per sales-hour than the consumer funnel, and
public-sector reference deployments materially de-risk subsequent tenders. The
hospitality tier compounds: a single PMS integration reaches every property that
operator manages or later adds.

**Reproducible builds are a sales-readiness gate for the defence and government
tier, not a nice-to-have.** That tier accepts a locked bootloader, which makes
its no-remote-disable guarantee contractual rather than structural (§4.1). The
compensating control we offer is that a buyer can compile the published source
and verify it matches the signed binary we ship — and a security review will ask
to exercise that, not merely to read about it. Until the build is demonstrably
reproducible, the tier's central security argument rests on assurance alone, and
tenders that turn on supply-chain verification should not be pursued. It is
tracked as outstanding in §11 for that reason, and should be sequenced ahead of
the first government pilot rather than after it.

Recurring revenue across all tiers comes from **support and SLA contracts**
(§7), not from software licensing or subscription gating of hardware function.

---

## 11. Implementation Status

This section exists because version 2 described several systems as operational
that were designed but not built. It is published so that any reader — investor,
procurement reviewer, integrator — can distinguish the three categories without
having to ask.

| Component | Status |
|---|---|
| BLE commissioning (lock and bridge) | **Working**, bench-verified |
| Thread mesh formation and lock join | **Working**, bench-verified |
| Bridge → Thread command relay | **Working**, verified end-to-end on hardware |
| App → server → bridge → lock remote unlock | **Working**, verified with hardware team, 2–5 s response |
| Thread dataset restore / bridge replacement | **Built**, firmware side; app-side provisioning not yet wired |
| MQTT broker routing to bridged locks | **Working**, lab-verified |
| AES-256-GCM payload envelope | **Built and byte-verified** against frozen test vectors, app side |
| X25519 pairing ceremony | **Not implemented** — the blocking dependency for all payload security claims |
| Lock-side AES-GCM primitive | **Not implemented** in firmware |
| Server relay-opaque operation | **Not implemented** — server currently persists cleartext credentials |
| Owner-root delegation and bounded-expiry tokens | **Designed and specified**, not implemented |
| Sleepy End Device power mode | **Not implemented** — firmware currently runs as a Full Thread Device |
| Production lock PCB | **Specified** (R1–R5 incorporated), not yet fabricated |
| Production bridge PCB | **Specified**, not yet designed |
| EFR32MG24 defence tier | **Committed direction**, no hardware built |
| Five mechanical form factors | **Committed**, specification in progress |
| Standalone Wi-Fi mode (no bridge, no remote unlock) | **Working** in principle; wake-interval tuning and delivery-confirmation UX outstanding |
| Lock → bridge state uplink over Thread | **Not implemented** — the relay is one-directional today |
| Bridge MQTT publish path | **Not implemented** — `bridge32` currently only subscribes |
| Lock roster held on the bridge | **Not implemented** — already the agreed architecture (bridge holds the routing table), unbuilt |
| MQTT Discovery for Home Assistant | **Not implemented** — depends on the three items above |
| Per-device provisioned identity (anti-clone) | **Designed**, not implemented — depends on item 1 |
| Owner rebuild-and-flash path (§7.4) | **Policy decided**; documentation not written |
| Reproducible builds (signed binary verifiable against published source) | **Committed, not achieved** — required for the defence tier's audit path (§4.1); a **sales-readiness gate** for that tier, sequence ahead of the first government pilot (§10.2) |
| Published privacy policy and transparency report | **Committed, not published** |
| RCM certification | **Not yet obtained** — path defined per §9 |
| Video peephole variant | **Roadmap only** — see §11.2, nothing built |

The single most consequential item is the **X25519 pairing ceremony**. The
cryptographic envelope is complete and verified on both the specification and
application sides; what is missing is the ceremony that gives it a key. Until
that lands, the end-to-end encryption commitments in §4 describe the
architecture's design rather than its running state, and this paper says so
wherever those commitments appear.

### 11.1 Sequencing

Work is ordered so that nothing built now has to be rewritten later. The test
applied at each step is whether the component is payload-agnostic — anything
that parses message contents must wait until the payload format is final.

| Order | Item | Rationale |
|---|---|---|
| 1 | X25519 pairing ceremony | Unblocks everything below; the envelope has no key without it |
| 2 | Lock-side AES-GCM primitive | Completes the sealed path end to end |
| 3 | Server goes relay-opaque; remove cleartext credential storage | Closes the §4.1 disclosure — the highest-priority correctness item in this paper |
| 4 | **Lock→bridge state uplink** (state, unlock confirmation, battery) | **Core product, not an integration feature.** Required for "is my door locked", "did it actually unlock", battery warning, and the OZKEY/OZPMS audit trail. Payload-agnostic, so it can run in parallel with steps 1–3 |
| 5 | Sleepy End Device power mode | Makes §3's battery model valid; independent of the above |
| 6 | Production lock and bridge PCBs | Depends on R1–R5 being frozen, not on firmware |
| 7 | RCM testing and declaration | Requires final production hardware; gates retail sale |
| 8 | Owner-root delegation, bounded-expiry tokens | Builds on the sealed base from steps 1–3 |
| 9 | Bridge MQTT publish path, lock roster, MQTT Discovery payloads | Home Assistant integration (§4.7). **Last deliberately** — see the note below. Small once step 4 exists |

**Why Home Assistant is last.** The core revenue products are a lock, an app,
and a bridge that works out of the box — plus OZKEY for hospitality and OZPMS for
portfolio operators. The average buyer cannot install Home Assistant, does not
want to, and should never need to. Building integration for a platform that a
small minority of customers run, ahead of completing the protocol those
customers and every other customer depend on, would be optimising for the
articulate segment rather than the paying one.

Note that this does not weaken §10.2's maker channel. That channel is served by
**published source and hardware documentation**, which are separate deliverables
already in progress — a self-hosting owner can drive these locks over plain MQTT
today with a few lines of configuration. MQTT Discovery removes that
configuration step; it does not enable the capability. Distinguishing the two is
what allows HA to sit last without any cost to the community strategy.

Deliberately *not* being built until step 3 completes: any further feature that
touches the credential payload format. Adding them to the plaintext base would
mean writing them twice, which is the specific mistake that produced the gap
between version 2's claims and its implementation.

**RCM timeline.** Procurement buyers require a date rather than a path, so the
dependency chain and the resulting estimate are stated here. RCM testing cannot
begin before final production hardware exists, because the declaration attaches
to a specific tested configuration (§9.2).

| Stage | Duration |
|---|---|
| Bridge PCB design and lock PCB finalisation | 4–8 weeks |
| Fabrication, assembly, first-article bring-up | 3–6 weeks |
| End-product EMC testing (AS/NZS CISPR 32) at an accredited laboratory | 2–6 weeks |
| Compliance folder, declaration, ACMA supplier registration | 1–2 weeks |

From the date of this paper that yields **Q4 2026 at the earliest, with Q1 2027
the prudent planning assumption** — the difference being whether the first EMC
submission passes. An unshielded or marginal design that fails emissions adds a
respin cycle, which is precisely the risk the certified-module decision in §9.2
was taken to avoid.

**No product will be offered for sale in Australia before the declaration is in
place.** Buyers requiring a contractual date should treat Q1 2027 as the
commitment and anything earlier as upside.

> **Editorial note:** the above is a derived estimate from the dependency chain,
> not a committed schedule. It requires the operator's sign-off before external
> distribution.

### 11.2 Roadmap, not committed

Items in this section are direction, not plan. They are listed so that the
architecture's intent is legible, and separated so that no reader mistakes them
for product.

**Video peephole variant.** The competitive opening is specific. Tuya's own
reference design for a video door unit streams MJPEG and retains video **in
Tuya's cloud for up to a month**. A peephole built on WebRTC with DTLS-SRTP and
**no cloud retention** is architecturally incapable of watching the customer —
the strongest possible expression of this paper's thesis, in the product feature
where customers feel the privacy question most acutely. WebRTC signalling can
ride the existing MQTT path.

The engineering shape is settled even though nothing is built: a dedicated
camera MCU performs capture and encoding, the C6 remains the radio and protocol
brain, and the two are linked over **SPI** — the 9600-baud lock MCU bus is
roughly four orders of magnitude too slow to carry video. The C6 itself has no
camera interface, no MIPI-CSI and no hardware encoder; it can only be the
transport. The R5 connector revision in §2.2 exists to keep this option open.

**Video access control — design commitments made now, before anything is
built.** "No cloud retention" answers where video is stored. It does not answer
who can watch it, which is the question a privacy-sensitive buyer actually
cares about. Four commitments are recorded here so that the implementation is
constrained by them rather than retrofitted to them:

- **The company cannot view a stream.** Media is encrypted end-to-end under
  DTLS-SRTP between the doorbell and the viewing device. We hold no key at any
  point and there is no debug or support path that grants one. A support
  request about video quality will be answered without video.
- **Signalling cannot be used to insert a viewer.** WebRTC's usual weakness is
  that whoever operates signalling can attempt to substitute keys during
  negotiation. We close this by binding the DTLS fingerprint to the pairing keys
  established by the §4.2 ceremony, so a substituted key fails verification at
  the endpoint. Signalling can deny a session; it cannot join one.
- **Temporary access is a bounded-expiry grant**, using the same credential
  mechanism as §6 — a homeowner can give a repairer or a neighbour view access
  for a defined window, and it lapses on schedule without requiring a
  revocation.
- **Recording, if offered, is local and owner-controlled** — to the owner's own
  storage, never to infrastructure we operate.

These are stated at roadmap stage deliberately. A video product whose privacy
model is designed after the streaming works will have the streaming architecture
dictate what privacy is possible, which is how competitors arrived at a month of
video in their own cloud.

Two documented failures set the standard this variant has to clear:

- **Ring / Amazon, FTC complaint and settlement, May 2023.** [7][8] The FTC
  charged that Ring employees and contractors — including staff at a
  third-party contractor in Ukraine — **could access and download every
  customer's videos, with no technical or procedural restriction before July
  2017.** The complaint describes one employee viewing thousands of videos from
  at least 81 female users, from cameras the customers had labelled for
  bedrooms and bathrooms, over hundreds of occasions across several months. The
  FTC also charged that customer video was used to train algorithms without
  consent, and that security failures allowed outsiders to take over accounts
  and cameras. Ring was ordered to pay **$5.8 million** in consumer refunds.
  The lesson is not that Ring was uniquely careless. It is that **any
  architecture in which the vendor can technically view the stream will
  eventually have someone view it** — through an employee, a contractor, a
  breach, or a subpoena. Policy did not prevent this; only architecture can.
- **Eufy / Anker, 2022–23.** ‡ A product line marketed on local-only storage
  was reported to be transmitting content off-device, and separately to expose
  streams accessible without the protections customers had been led to expect.
  The lesson is that a privacy claim resting on configuration rather than
  architecture is one firmware change away from being false, and the customer
  has no way to audit it. *(‡ requires verification and formal citation before
  publication.)*

[7] FTC, "FTC Says Ring Employees Illegally Surveilled Customers, Failed to Stop
Hackers from Taking Control of Users' Cameras", 31 May 2023 —
`ftc.gov/news-events/news/press-releases/2023/05/`
[8] FTC case file, *Ring, LLC*, matter 2023113 —
`ftc.gov/legal-library/browse/cases-proceedings/2023113-ring-llc`

This is why the commitments above are architectural — end-to-end keys we never
hold, fingerprints bound to the pairing ceremony, storage under the owner's
control — rather than policy statements about what we intend to do with access
we would otherwise retain. A promise not to look is worth less than an inability
to look, and the difference is exactly what a buyer in this category should be
asking about.

**Buck converter evaluation** for lock revision 2, per R3, fitted inline on the
6 V lead so no board respin is required to test it.

**Second bridge as range extender**, using the dataset-restore mechanism already
described in §4.4.

---

## 12. Conclusion

Sovereign Edge is a straightforward proposition: a smart lock that keeps control
with its owner by default — over their data, their network dependency, their
trade-off between battery life and responsiveness, and their choice of who
supports the system — manufactured through a labour model that pays and trains
workers well above the regional norm.

Version 3's substantive addition is an argument rather than a feature. The
smart-home industry offers a choice between platforms that are open to
manufacturers and closed at runtime, and platforms that are open at runtime and
closed to manufacturers. Both arrangements place a gatekeeper somewhere in the
relationship between a person and the lock on their own front door. The
position taken here is that neither is necessary: publish the protocol, publish
the server, sign the firmware, hold the keys at the edge, and sell the one thing
that genuinely cannot be commoditised — the ongoing relationship of supporting
a system properly.

No lock. No bounty. No hold-up.

---

*eBizco Australia Pty Ltd · Version 3 · July 2026*
