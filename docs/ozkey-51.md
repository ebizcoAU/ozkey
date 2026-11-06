# ozkey-51 — OZPMS tenancy model: company → manager → tenant → family, and a guaranteed clean move-out

> Status: **DRAFT (R&D), 2026-10-18, firmware (team lead).** Second in the agreed order (52 → 51 → 50 → 53).
> Extends ozkey-48 (OZKIE v2 roles) with a **custodial mode**. Server-side parts are for the server
> developer to build (ozkey-50 will place them in the shared server core).
> Not legal advice. Tenancy-law points are flagged for Finance/legal (they cover AU and VN law per XF-386).

## 0. Requirements (operator, 2026-10-18)

- A property-management company runs **~10,000 rental houses** with **~50 managers**, each handling
  100–300 houses. Administration has **3 levels** with **regional control**, self-hosted on its own VPS.
- **The tenant is the resident and has exclusive use of the door**, including remote unlock, for
  themselves and their family.
- Authority flows down the chain of command to the tenant and family, securely.
- **When a tenant vacates, all their access is removed before the next tenant moves in.**

## 1. Where each part lives

| Layer | Lives in | Enforced by |
|---|---|---|
| Company → region → manager → house | **ozpms** (database + role checks + audit) | the server. The lock never sees managers |
| Custodian → occupant → family → credentials | **the lock** (bond table) | the lock, offline, cryptographically |

The lock can't check a 3-level org chart, and doesn't need to. It knows exactly one landlord
identity, the **custodian**, and enforces the tenant's exclusivity itself. ozpms decides *which
manager* may ask the custodian to do something.

## 2. Lock roles in custodial mode (extension to ozkey-48 §6)

A lock is claimed into **custodial mode** at installation instead of the residential owner mode.

| Role | Holder | Count | May | May not |
|---|---|---|---|---|
| **custodian** (bond #0) | the company's custodian key, **held by ozpms**, one key **per region** (§5) | 1 | assign / end the occupancy; issue contractor credentials under notice (§4); emergency entry, logged; read events | **routine unlock**; read or change the occupant's family or PINs |
| **occupant** | the tenant's phone (X25519 app key) | **exactly 0 or 1** | **remote unlock** (relay context, like the owner in ozkey-48); unlock at the door; invite family; create guest PINs; see every credential on the door, including the custodian's | touch the custodian; outlive the lease |
| **family** (member) | invited by the occupant | ≤ 14 | unlock **at the door** (on premises, as ozkey-48 members) | remote unlock, invite others |

The operator's owner-only remote-unlock rule (ozkey-48 §4.1) generalises cleanly: **remote unlock
belongs to the top resident role.** That is the owner in OZLOCK, the occupant in OZPMS, and the hotel
in OZLODGE, where guests are on premises only.

## 3. Move-out: three independent guarantees

Every bond and credential on the lock records **who created it** (`parent`). Everything the tenant
made hangs under the occupant bond.

1. **Explicit end.** The manager presses "End tenancy" in ozpms. ozpms sends `occupancy/end`
   (custodian, relay). The lock deletes the occupant and **their whole subtree** (family bonds,
   tenant-made PINs, cards, fingerprints) in one atomic write. It then increments the epoch and
   reports an empty occupancy.
2. **Automatic expiry.** The occupant bond carries the **lease end** (+ a configurable grace, e.g. 24 h).
   - At that time the lock ends the occupancy by itself, even if nobody pressed anything and even
     if the bridge is down.
   - The clock rule from CONTRACT.md applies: the clock is set by the bridge and moves forward only.
3. **Move-in gate.** `occupancy/assign` is **refused with `OCCUPIED`** while any occupant exists.
   - ozpms shows a house as **"Vacated, confirmed by lock at <time>"** only after the lock reports
     the empty occupancy.
   - The dashboard won't issue a new tenant invite before that.

So a new tenant can't be let in while the old one still has access, whatever the managers forget,
and the lock itself guarantees it.

## 4. The landlord's own access, kept lawful and visible

Landlord entry during a tenancy is regulated (notice periods; emergencies excepted). The lock
doesn't judge the law, but it makes every landlord access **explicit, bounded and visible to the tenant**:

- **Contractor or inspection credentials** from the custodian must carry a time window and a
  `notice_ref` (ozpms records the notice given). They appear in the occupant's credential list,
  marked as landlord-issued.
- **Emergency unlock** by the custodian needs a `reason` code (fire, police, welfare, …). It is
  logged as a distinct event and **pushed to the occupant**.
- **No routine custodian unlock**, and no "lockout" control. This is the lawful design ozpms.md §5
  rev 1.1 already set: access lapses at lease end; it is never seized mid-tenancy except by a
  recorded tribunal order, which is `occupancy/end` with an `order_ref`.

🟡 For legal review (Finance/legal): notice periods per AU state and in VN, and whether emergency
entry needs anything more than a logged reason.

## 5. Limiting the damage if the landlord side is breached

- **One custodian key per region**, kept in ozpms behind a key-encryption key from env (server team,
  ozkey-48 §7). A stolen key exposes one region, not 10,000 houses.
- **Every occupancy change notifies the outgoing occupant.** Even an attacker holding the custodian
  key can't silently end a tenancy and move themselves in.
- **The manager scope is server-side:** each manager acts only on their portfolio, and every action
  is audited with who, which house, and why.
- **Phase 2:** each admin gets their own signing key. ozpms forwards a custodian command only with
  a signed chain (company → region → manager). This protects against a compromised ozpms login,
  though not a compromised ozpms host. The lock still needs no signatures.

## 6. Installation and onboarding

1. **The installer claims the lock for the company.**
   - The installer's phone relays the EDHOC handshake between the unclaimed lock and ozpms. The
     region's custodian key never leaves ozpms.
   - The installer types the keypad code (ozkey-48 §3.4), which ozpms computes and shows on the phone.
   - The lock becomes custodial. The device certificate (ozkey-52) proves it's genuine.
2. **The lease is signed.**
   - The manager creates the tenancy in ozpms (lease dates).
   - ozpms issues a **custodian-signed occupant invite** (the same OZINV format, from the custodian bond).
3. **Move-in day:** the tenant redeems the invite at the door (EDHOC EAD_3) and becomes the
   occupant, valid to lease end. Remote unlock works from then on.
4. **Family:** the tenant invites them from the app.

## 7. Factory reset in custodial mode

In residential mode, a reset gesture at the door wipes everything, and a crowbar-reset has no
software rescue (threat model memory). **In custodial mode a tenant must not be able to reset the
lock and keep it.** Proposal:
- The local reset gesture only **ends the current occupancy**, which hands the door back to the landlord.
- A full wipe needs a **custodian command**.
- 🟡 Open: hardware service reset for a lost custodian key, e.g. a signed service token. Decide with ozkey-52.

## 8. Changes this needs

- **ozkey-48 (v2 spec):**
  - mode `custodial` at claim;
  - roles custodian / occupant;
  - verbs `occupancy/assign`, `occupancy/end`, `occupancy` GET;
  - a `parent` on every bond and credential, with cascade delete;
  - lease-end expiry;
  - result codes `OCCUPIED`, `NOT_OCCUPIED`;
  - emergency `reason` and `notice_ref` / `order_ref` fields.
- **ozpms (server):**
  - company / region / manager / house tables and scoped roles;
  - per-region custodian keys;
  - the "vacated, confirmed" gate;
  - occupancy notifications;
  - audit with reasons.
- **App (ftpos):** occupant role UI (remote unlock, family, guest PINs, "landlord access" list).
  It's a new XF once this doc is approved.

## 9. Questions for the operator

1. **Family remote unlock:** OZLOCK's rule is members on premises only. Should the **occupant** be
   allowed to grant remote unlock to a family member (e.g. a spouse), or is remote unlock occupant-only?
   Firmware recommends: occupant-only, the same as OZLOCK.
2. **Grace after lease end:** 24 h default?
3. **Regions:** is "one custodian key per region" the right blast-radius unit, or per manager portfolio?
