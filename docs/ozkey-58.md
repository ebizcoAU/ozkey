# ozkey-58: OZLODGE ↔ TalkPOS ↔ Nexus/TalkStay: direct booking, guest keys, offline sync (design note)

> **Status:** DRAFT rev 0, 2026-11-26, ozkey-srv (server lane). **Paper only:** nothing here is built.
> For PM nexus-1b. The Nexus and ftpos parts are **interface specs** (§7). The PM takes them to those
> teams after 11-29.
> **Inputs:** the operator's rulings of 2026-11-26 (§2); ozkey-48 rev 7 (OZKIE v2: roles §6, invites
> §3.3, events §5.1); ozkey-50 (server core, the lodge profile, open booking API, `issuer` role);
> the lock code in `blelock/ozcore` (checked 2026-11-26, §4.3); and the OTA and SiteMinder research
> in `nexus-lab-backups/ozkey_notes/2026-11-26_OTA_connectivity_research.md` (summary in Appendix A).
> **Supersedes:** in ozkey-50 §4, "MAVEN (the hotel's PMS) stays the system of record". MAVEN is the
> AU name of TalkPOS (PM, ruled 11-07), and §6 below replaces the one-way sync with two-way sync.

## 0. The idea

**TalkStay direct booking is the same strategy as TalkRide against Uber and Grab.** We own the
channel and the guest relationship inside TalkKim. The OTAs (Booking.com, Agoda, …) are optional
add-ons, not the base (operator, 2026-11-26).

The guest books a room in **TalkKim**. **Nexus/TalkStay** takes the booking and the payment, and
passes the booking to the property's **OZLODGE** over a connection that OZLODGE opens itself. OZLODGE
is the property's always-on server. It holds the rooms, the bookings and the room↔lock map, and
it issues the guest's door key: an invite bound to the guest's own TalkKim key. Nexus delivers the
invite as a blind mailbox. At the door the guest's phone redeems it over Bluetooth. **TalkPOS**, the
property's PMS front end on staff phones and tablets plus the self check-in kiosk, syncs with OZLODGE
whenever it is online.

No OTA is in this path. Nexus never holds anything that opens a door. The property keeps working
when the internet is down.

## 1. Who does what

| Piece | Runs on | Role here |
|---|---|---|
| **TalkKim** | the guest's phone | books (TalkStay), pays, holds the guest's own X25519 key pair, receives the invite, opens the door over BLE (OZKIE v2 session link) |
| **Nexus / TalkStay** | our cloud | the booking channel: listing, availability shown to guests, payment, commission. The far end of OZLODGE's outbound link. The **blind mailbox** for guest keys |
| **OZLODGE** | the property's own machine, on its LAN | **master for anything that touches a lock** (§2): rooms, bookings, room↔lock map, guest keys. Holds the property's `issuer` bond and `service` bond on each lock (ozkey-48 §6). Backend for TalkPOS and the kiosk. The bridges point to it |
| **TalkPOS** | staff phones/tablets; the kiosk as a secondary device | the PMS front end: bookings, staff, accounting, inventory, tax, full POS, AI IVR self check-in. **Master for prices, accounting and guest details** (§2). Not always online |
| **Lock (AK1037)** | the door | enforces everything: invite MAC and binding, stay window, roles. Bond #0 is a hotel admin's staff device (ozkey-50 §3), never a server |

**Business templates.** TalkPOS has one business type for these, `lodging`, with
`premises_class` set to `commercial` (Hotel, and Motel: PM 11-26, the label only) or `residential`
(HomeStay) (ftpos `lib/config/business_types.dart`, XF-342). This design is the same for all three.
The differences are size (§6.4: a 2-room homestay may run OZLODGE on a small box or a spare tablet;
OPEN Q5) and tax (TalkPOS's concern, not OZLODGE's).

## 2. Rulings this note follows

Operator, 2026-11-26 (relayed and confirmed by the PM):
1. **Item 1 ruled by the operator: A first.** Our own channel is TalkStay direct booking inside TalkKim,
   with no OTA in the path. B (SiteMinder booking email → TalkPOS, staff confirm) and C (SiteMinder
   pmsXchange) are "later", design to be confirmed (§9). The original item 1 ("Nexus is the default
   channel manager for Booking.com/Agoda") is **superseded by the facts**: Booking.com and Airbnb are
   closed to new connectivity partners (Appendix A).
2. **OZLODGE dials out** to Nexus over a persistent outbound connection. No inbound ports.
3. **OZLODGE is master** for room↔lock mapping and key issuance.
4. **Guest key, zero trust:** TalkKim makes its own key pair. The public key reaches OZLODGE through
   Nexus. OZLODGE's issuer bond signs a time-limited invite bound to it. Nexus delivers it as a blind
   mailbox.
5. **Cloudflare is optional.** Properties with their own IT may use Cloudflare or a VPN instead of the outbound link.
6. **Offline mode:** OZLODGE works fully (kiosk check-in, door unlock) without internet, and syncs
   with Nexus on reconnect.
7. **Conflicts:** OZLODGE wins for anything touching a physical lock. TalkPOS wins for prices,
   accounting and guest details.
8. **Open booking API:** third-party channel managers (SiteMinder, …) can bypass Nexus if the property
   chooses (§8).
9. PM, N2: offline check-in = a kiosk-issued local credential (PIN/RFID) plus a local invite hand-off.
   **That key never goes through Nexus** (§5).

## 3. Flow A: a direct booking in TalkKim

```
TalkKim ──book + pay + guest_pub──► Nexus/TalkStay ◄══ outbound link ══ OZLODGE ◄── LAN sync ──► TalkPOS
   ▲                                   │                                   │
   └──────── invite (blind mailbox) ◄──┘ ◄──── invite bound to guest_pub ──┘
   │
   └── at the door: EDHOC (session link) + invite in EAD_3 ──► lock: checks MAC, binding, window
```

### 3.1 Availability
- OZLODGE publishes **availability per room type** to Nexus over the link: dates, counts and a
  price reference. TalkPOS owns prices (§2.7), so the prices come from TalkPOS through OZLODGE's copy.
- **Allotment while OZLODGE is offline** (proposal, OPEN Q1). Each availability message also grants
  Nexus a small **allotment**: N rooms per type per night that Nexus may sell without asking OZLODGE.
  Then a property whose internet is down still takes bookings, and the double-booking risk is bounded
  by N, which the property sets (default 0 = no offline sales).

### 3.2 Booking
1. The guest picks a stay in TalkKim. TalkKim sends Nexus the booking request, the payment, and the
   **guest public key `guest_pub`** (X25519, 32 B, made by TalkKim and kept on the phone).
2. Nexus asks OZLODGE over the link: `booking.hold` → OZLODGE answers `held` (room type, hold
   expiry) or `unavailable`. If OZLODGE is offline and there is allotment left, Nexus sells from the
   allotment and marks the booking `pending_property`.
3. After payment: `booking.confirm` (booking id, stay window, guest details, `guest_pub`,
   payment reference). OZLODGE stores it, picks a room (or leaves that to check-in, OPEN Q2) and
   answers `confirmed` with the room type and the property's booking reference.
4. Every message has an id, is acknowledged, and is **idempotent** (§6.2). A resend after a dropped
   connection changes nothing.

### 3.3 The key
5. When the room is assigned, OZLODGE issues an **`OZINV2` member invite** (ozkey-48 §3.3, canonical
   v3) through its `issuer` bond on that room's lock:
   - `bind` = SHA-256(`guest_pub`)[0..16]: only that TalkKim can redeem it;
   - membership expiry = check-out (plus a grace period, OPEN Q3), within the lock's `max_days` for
     this issuer;
   - start ≥ now − 1 h (the lock's `ISSUER_START_SLACK`), so a key can't be issued backdated.
6. OZLODGE sends `key.deliver {booking_id, room, lock_id, invite}` over the link. Nexus stores it in the
   guest's **mailbox** and TalkKim fetches it. To Nexus it is opaque: without the guest's private key
   it opens nothing (§4.2).
7. At the door, TalkKim runs EDHOC with the lock on the **session link** (BLE) and puts the invite in
   EAD_3. The lock checks the MAC, the binding and the window, and makes a `member` bond that expires
   at check-out. From then on the guest unlocks over BLE. Members never reach the lock through the
   bridge relay (ozkey-48 §4.1), so a guest **never has remote unlock** (ozkey-48 §6).

### 3.4 Changes, cancellations, check-out
- **Normal check-out:** nothing to send. The lock ends the bond at its expiry (`bond_live`).
- **Extension:** a new invite with the later expiry. The guest redeems it at the door, or a future
  "extend" verb does it (OPEN Q4).
- **Cancellation before redemption, early check-out, room move:** these need the lock to cancel an
  unredeemed invite or revoke a guest bond **before** its expiry. **The lock does not let the
  issuer or service bond do that today (§4.3, gap G1).** Until it does, either a hotel admin device
  does it (bond #0, over the relay or at the door), or the stay simply runs to its expiry.

## 4. Guest keys: what the lock already does, and two gaps

### 4.1 OZLODGE can issue keys with no internet and no lock round trip
Checked in `blelock/ozcore` 2026-11-26. The invite MAC key comes from X25519(lock private key, issuer
public key) (`oz_lock.c` `redeem_invite`, `oz_invite.c`). So OZLODGE, holding the issuer private key
and the lock's public key, computes the invite **entirely on its own**. The lock checks it at
redemption. Issuing a key needs no lock round trip, no bridge and no internet. This is what makes
offline check-in (§5) work.

The lock also enforces the following for an issuer's invite:
- version 3 (`OZINV2`) with the right `iss`;
- a membership end no further out than `max_days`;
- a start no more than 1 h back;
- a **set clock**. Without one the lock refuses issuer invites. On the LAN, the bridges get their
  time from OZLODGE, so OZLODGE's own clock must be right while it is offline (OPEN Q6: an RTC or a
  GPS/LTE time source on the box).

### 4.2 Why Nexus can't use what it carries
An `OZINV2` invite is only redeemable by a key that completes EDHOC method 3 with the lock as the
bound key. Nexus has `guest_pub` but never the private key. An invite copied from the mailbox, or a
`guest_pub` swapped by a compromised Nexus, gives at most a key bound to **Nexus's own** key, for a
room and dates OZLODGE chose for a paid booking. **Residual risk:** a compromised Nexus can book a
room and receive a working key bound to a key it made up, the same as any paying guest. That is a
fraud problem (payment), not a lock break. A stolen mailbox message alone opens nothing.

### 4.3 Gaps in the lock (for ozkey-fw, via the PM; not server work)
- **G1. Only the admin can cancel an invite or revoke another bond.** `bond/invite/<nonce>` DELETE
  and `bond/<n>` DELETE allow `admin` (and a member revoking itself) only (`oz_lock.c` ~1436-1441). A
  hotel needs OZLODGE to cancel a booking, end a stay early, or move a guest **without** a staff admin
  device. **Proposal:** let the `issuer` bond cancel invites it issued and revoke member bonds made
  from its own invites (the lock already records `iss`). This changes ozkey-48 §6, so it needs the
  operator's OK.
- **G2. Expired guest bonds keep their slot.** `free_slot()` only checks `used`, and `bond_live()`
  treats an expired bond as dead but leaves it in the table. A lock has **16 bonds**: admin + issuer +
  service + up to 2 bridges leaves about **11** for guests. After about 11 stays with no clean-up, the
  next guest gets `MEMBER_FULL`. **Proposal:** the lock reuses the slot of a member bond that expired
  more than a grace period ago (and drops its relay contexts), so no server visit is needed. This is a
  firmware change.

## 5. Offline: what keeps working

| What is down | Bookings | Key to the guest | Door |
|---|---|---|---|
| The property's internet | Nexus sells from the allotment (§3.1); confirms queue in Nexus | **Kiosk** (§5.1) | works (BLE, lock-local) |
| Nexus | no new TalkStay bookings; TalkPOS walk-ins work | **kiosk** | works |
| The guest's phone has no data | — | invite fetched earlier, or **kiosk** | works (BLE needs no data) |
| OZLODGE itself | TalkPOS keeps working on its own copy; nothing issues keys | none until OZLODGE is back; staff admin device as the fallback | **works for existing bonds and PINs** |

### 5.1 Offline check-in at the kiosk (PM N2: the key never goes through Nexus)
The kiosk is a TalkPOS secondary device on the LAN, talking to OZLODGE.
1. **Who is this guest?**
   - **Online booking that reached OZLODGE:** the guest shows the booking in TalkKim (QR). The kiosk
     looks it up.
   - **Booking made while the property was offline** (allotment, so OZLODGE hasn't heard of it): the
     QR carries a **booking confirmation signed by Nexus** (Ed25519; OZLODGE pinned Nexus's public
     key at enrolment). OZLODGE checks the signature offline, records the booking, and takes it off
     the allotment. Proposal, OPEN Q1.
   - **Walk-in:** booked and paid in TalkPOS at the kiosk or desk.
2. **Which credential:**
   - **Local invite hand-off** (default for TalkKim users). TalkKim shows its `guest_pub` as a QR.
     OZLODGE issues the `OZINV2` (§4.1, no lock round trip), and the kiosk shows it as a QR that
     TalkKim scans. It never leaves the LAN, and it is the same invite §3.3 would deliver.
   - **PIN** (guests without TalkKim). OZLODGE writes a time-limited PIN through its `service` bond
     (`cred` POST, epoch rule ozkey-48 §4.3). That needs the lock reachable through a bridge on the
     LAN, which works without internet. The PIN window = the stay.
   - **RFID card**: a `service`-bond `cred` write, if the kiosk has a card encoder (OPEN Q7).
3. TalkPOS records the check-in. It syncs to Nexus (stay status) when the link is back.

## 6. Sync

### 6.1 Who owns which field (operator rule §2.7)
| Data | Master | Why |
|---|---|---|
| room ↔ lock, lock ids, bridges | OZLODGE | touches a lock |
| booking state: held/confirmed/checked-in/checked-out/cancelled | OZLODGE | drives keys |
| room assignment, stay window (check-in/out times) | OZLODGE | drives keys |
| issued invites, PINs, cards, revocations | OZLODGE | touches a lock |
| prices, rate plans, taxes, payments, invoices, accounting | TalkPOS | business |
| guest name, contact, ID documents (VN CCCD at check-in) | TalkPOS | guest details |
| availability as Nexus sees it | OZLODGE (counts) + TalkPOS (prices) | merged by OZLODGE before publishing |

A change from the non-master side is a **request**: for example, TalkPOS moves a guest to another
room. OZLODGE applies it if the move is still valid, and that may issue a new invite and revoke the
old one. If it is no longer valid, OZLODGE refuses with a reason, and TalkPOS shows the refusal.

### 6.2 OZLODGE ↔ TalkPOS (LAN)
- **Change log:** every record has a `version` (per-record counter) and OZLODGE keeps a change log
  with a monotonic `seq`. A TalkPOS device pulls `changes?after=<seq>` and pushes its own changes
  with the `version` it last saw (If-Match style).
- **Conflict:** if a pushed change hits a newer version, the master's field wins (§6.1). The other
  side gets the current record back.
- **Idempotent:** each change carries a client-made id, so a resend after a dropped connection
  applies once.
- **Several TalkPOS devices** (staff phones, tablets, the kiosk) sync through OZLODGE, never with
  each other.
- **Discovery:** OZLODGE's mDNS on the LAN (it exists today). Auth: a per-device credential that hotel
  staff issue, like the per-client API keys of OZLOCK-AUTH-1.

### 6.3 OZLODGE ↔ Nexus (outbound)
- **Transport:** OZLODGE opens a TLS connection out to Nexus and keeps it open: WebSocket or MQTT over
  TLS, Nexus's choice (OPEN Q8). It reconnects with backoff. Nothing listens on the property's side.
- **Messages:** the same id / ack / idempotency rules as §6.2, queued on both sides while the link
  is down and delivered in order on reconnect.
- **Auth:** a per-property credential that OZLODGE gets at enrolment, plus OZLODGE's own key pair.
  Payloads that carry keys are signed by OZLODGE, so Nexus or a tablet can check them. Nexus pins
  OZLODGE's public key at enrolment.
- **Privacy:** OZLODGE keeps the hotel audit (ozkey-50 §3: the events module is loaded on lodge).
  Nexus gets booking and stay status, not door events.

### 6.4 Where OZLODGE runs
On site, on the property's LAN (ozkey-50 §3, XF-390 ARCH-3a). A hotel uses a small server or mini-PC.
A 2-room homestay may not want dedicated hardware. Options: a small box we supply, the property's
always-on tablet, or a hosted OZLODGE in our cloud. A hosted OZLODGE loses offline mode, because the
lock and bridges then depend on the internet (OPEN Q5).

## 7. Interfaces (specs for the other teams; they confirm after 11-29)

### 7.1 Nexus/TalkStay ↔ OZLODGE link
| Message | Direction | Fields (draft) |
|---|---|---|
| `availability` | OZLODGE → Nexus | property, room types, per-night counts, price ref, **allotment** per type/night, valid-until |
| `booking.hold` / `held` / `unavailable` | Nexus → OZLODGE / reply | booking id, room type, stay window, guest count; hold expiry |
| `booking.confirm` / `confirmed` | Nexus → OZLODGE / reply | booking id, stay, guest details (minimum needed), `guest_pub`, payment ref; property ref |
| `booking.modify`, `booking.cancel` | Nexus → OZLODGE | booking id, new stay / reason |
| `booking.from_allotment` | Nexus → OZLODGE on reconnect | as confirm + the signed confirmation id (§5.1) |
| `key.deliver` | OZLODGE → Nexus | booking id, room, lock id, invite (opaque blob), valid from/to |
| `key.cancelled` | OZLODGE → Nexus | booking id, invite id (when G1 exists) |
| `stay.status` | OZLODGE → Nexus | booking id, checked-in / checked-out / no-show |
| `ack` | both | message id |

Nexus also holds its **Ed25519 signing key** for offline booking confirmations (§5.1). OZLODGE pins its
public key at enrolment.

### 7.2 TalkKim (ftpos)
- Generate and keep the guest X25519 key pair per stay or per install (OPEN Q9). Send `guest_pub` with the booking.
- Fetch `key.deliver` from the Nexus mailbox. Store the invite.
- At the door: EDHOC session + invite in EAD_3 (ozkey-48 §3.3 transport). Then unlock as a member.
- Show the booking QR (id + Nexus-signed confirmation) and the `guest_pub` QR. Scan the kiosk's invite QR.

### 7.3 TalkPOS (ftpos)
- LAN sync client for §6.2 (change log, versions, conflict replies).
- Kiosk: QR scan + QR show; PIN display; card encoder if any.
- Staff actions that touch a lock (move, early check-out, cancel) are requests to OZLODGE (§6.1).

## 8. Open booking API: third parties can bypass Nexus
The §7.1 booking messages are also an **HTTP API on OZLODGE** (ozkey-50 §4), with a **separate
credential per reseller** that hotel staff issue and revoke (ozkey-50 Q4: only hotel staff approve
resellers). A property that wants a third-party channel manager connects it here through its own
Cloudflare tunnel or VPN (§2.5). Nexus is one client of the same messages, not a special case in the
code. Booking payloads are encrypted end to end to OZLODGE's key when they pass through Cloudflare
(ozkey-50 Q2(a)).

## 9. Later (design to be confirmed)
- **B. OTA bookings by email → TalkPOS.** A SiteMinder (or OTA) booking email is parsed into TalkPOS
  as a pending booking. **Staff confirm it before any key is issued.** Then the normal OZLODGE path
  applies: room, invite (if the guest has TalkKim) or PIN/card.
- **C. SiteMinder pmsXchange.** OZLODGE (or Nexus for it) becomes a SiteMinder PMS partner, and
  reservations are pushed with guest details. Partnership agreement first. **Its terms are not public**
  (Appendix A): any non-compete or data-use clause must be read under NDA before we build. Guest data
  pushed this way belongs to the hotel (the controller), so it must stay out of Nexus's channel business.
- **iCal** for single-unit homestays as a double-booking guard on Airbnb/Booking.com/Agoda: dates
  only, so it can't issue a key by itself. That fits under B.

## 10. Server work (after the PM schedules it; not before OZLOCK-AUTH-1)
1. OZLODGE onto `ozserver-core` as the lodge profile (ozkey-50 §7), with the Tuya 55AA credential path
   retired. OZLODGE is a standalone `server.js` today (found 2026-11-19).
2. Its own data model: rooms, room types, bookings, stays, room↔lock, invites/credentials, change log.
3. An OZKIE v2 client: `issuer` (invite minting, §4.1) and `service` (cred writes, events, SAFE-mode
   handling, acceptance list B of OZKIE-V2-SERVER-1).
4. The outbound Nexus link (§6.3) and the TalkPOS LAN sync (§6.2).
5. Kiosk endpoints (§5.1). The open booking API with per-reseller credentials (§8).
6. Tests red-first, as for every server change. A host-lock end-to-end: book → invite → redeem at a
   host lock → expiry.

## 11. Open questions
| # | Question | Proposal |
|---|---|---|
| Q1 | Offline sales: allotment + Nexus-signed confirmations? | yes, default allotment 0 (off) per property |
| Q2 | Room assigned at booking or at check-in? | at check-in for hotels (flexibility), at booking for homestays |
| Q3 | Key grace after check-out time | 30 min, set per property |
| Q4 | Extensions: re-invite or a lock "extend" verb? | re-invite now; verb later with G1 |
| Q5 | Where a small homestay runs OZLODGE | a small supplied box; hosted only if the owner accepts no offline mode |
| Q6 | OZLODGE's clock while offline (the lock refuses issuer invites without time) | an RTC on the box; refuse to issue if its clock is unset |
| Q7 | Kiosk card encoder | per property; PIN + phone key need none |
| Q8 | Link transport (WebSocket vs MQTT/TLS) | Nexus's choice |
| Q9 | Guest key per stay or per TalkKim install | per stay (unlinkable across hotels) |
| G1 | Issuer may cancel/revoke what it issued | yes: ozkey-48 §6 change, operator + ozkey-fw |
| G2 | Lock reuses expired guest slots | yes: firmware, ozkey-fw |

## Appendix A. OTA and channel-manager facts (checked 2026-11-26)
Full table with URLs: `nexus-lab-backups/ozkey_notes/2026-11-26_OTA_connectivity_research.md`.
- **Booking.com:** connectivity is through approved channel managers only, and it is **paused for new
  providers** ("pausing integrations with new connectivity providers until further notice",
  connect.booking.com). iCal sync is for small properties only: ≤20 room types, 1 unit each, and none
  while using an XML channel manager (help-page quote; the page refused our fetch). It carries dates only.
- **Airbnb:** API **invite-only / closed** to new access requests (wording via third parties, Oct 2026,
  unverified on Airbnb's own page). iCal refreshes every 3 h and carries no guest name.
- **Agoda:** YCS API, open via a questionnaire, certification + a 2-3 property pilot. iCal only for room
  types with 1 room.
- **Expedia, Traveloka:** open but gated (licence, NDA, certification, pilot).
- **SiteMinder pmsXchange:** push or pull; a partnership agreement is signed before any API access;
  ~60 days to production. **The agreement is not public**, so any non-compete is unknown. The hotel
  is the data controller (SiteMinder DPA).
- **Cloudbeds:** its API licence forbids using the API to "substantially replicate or compete with
  core products or services offered by Cloudbeds" (§4.4, updated 2026-04-02).
