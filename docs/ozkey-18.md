# ozkey-18 — OZLOCK completion roadmap, and the server team's work queue

**Status: ACTIVE.** Written 2026-08-10, operator-approved. Supersedes any
informal task list. This is the server team's standing instruction — it
lives in `docs/` deliberately, and nothing in it requires reading a doc in
`ftpos/ftposDecisions/` to act on.

---

## 0. Standing instruction — read this before building anything

**Verify my specs against the code before you build, and push back when
they are wrong.** This is not politeness; it is the process.

In the last 24 hours I got three things wrong that both teams caught:

- I told everyone remote revoke was BLE-only. It has worked over MQTT since
  ozkey-13. **Three days of app work was spent routing around a constraint
  that did not exist.**
- I specified the bond-verb route as "the same way a remote grant already
  goes." No such route existed. ftpos read `server.js` before building and
  stopped to ask, which is exactly right.
- I said the uplink needed "zero server-side change" because of a wildcard
  subscription. The wildcard does not exist; MQTT matches full topic
  strings. You found that by testing before writing code, which is exactly
  right.

Each was caught because someone checked rather than complied. Keep doing
that. A spec from me is a **proposal to verify**, not an instruction to
follow blindly — especially when it asserts what some other component can
or cannot do.

Corollary: when you finish something, state what you **actually proved**
versus what you assumed. The distinction between "transport-verified" and
"executed" cost us a scramble today. You adopted that language unprompted;
please keep it.

---

## 1. The three gates — nothing expands past OZLOCK until these are answered

Every tier — OZLOCK, OZLODGE, OZPMS — inherits the same firmware, the same
transport and the same trust model. So an unresolved question here is
unresolved in all three, and a spec written on top of it may need rewriting.

| Gate | Status | Why it blocks everything |
|---|---|---|
| **G1 — Battery life** | **UNMEASURED** | The lock runs `rx_on=1 ftd=1` (receiver-on-when-idle, a Full Thread Device — NOT the "Sleepy End Device" the product docs claimed until 2026-08-10). No measurement exists in that mode under realistic mesh traffic. "Multi-year battery" appears in all three specs with nothing behind it. At 500 rooms or 30,000 camp doors this IS the maintenance contract. If it forces a design change, every tier's economics move. |
| **G2 — OTA** | **NOT BUILT** | There is no field update path. Every bug found this week would be permanent on a shipped lock. For a security product that is disqualifying. The N8 flash was chosen with dual-OTA in mind; nothing is built on it. |
| **G3 — Owner recovery** | **NOT DESIGNED** | If the phone holding bond #0 is lost, there is currently no recovery, no second admin, no backup. At 20,000 units that is a support catastrophe. This is a design question before it is a coding one, and it has security consequences (any recovery path is also an attack path). |

**None of these are server-team work** — G1 and G2 are firmware, G3 is a
joint design. They are listed here so it is clear why tier expansion is
sequenced behind them, and so nobody builds a spec that a battery number
invalidates.

---

## 2. Server team work queue — in order

### S12 — The rename (do this first)

`ozkey` → `ozlodge`, `ozkeyserv` → `ozlodgeserv`, and any
`OZKEY`/`OZKEYSERV` naming in code, config, topics, DB and docs that means
the hospitality tier.

Why first: it is pure housekeeping, zero design risk, and the current
naming is already causing real confusion — `OZKEY` currently means both the
webapp *and* the on-prem hotel server in different documents
(`product-topology-and-names` records the collision). The codebase will
never be smaller than it is now.

Take care with anything an already-deployed device or app depends on —
MQTT topic prefixes, DB names, config keys, ports. Where a rename would
break a live device, leave a documented alias rather than breaking it, and
list what you aliased.

### S13 — C7: seal the door-event log

**This is the change that makes OZLOCK's headline claim true.**
`ozlock.md` says the server "stores no information about who opened which
door." That is currently false: `publishLog()` sends
`{device_id, result, detail, ts}` in **plaintext** to
`ozkey/<site>/locks/<id>/log`, where `detail` distinguishes owner from
member, and `ozlockserv` subscribes and stores it. Recorded as gap **C7**
in `ozlock.md`'s Status Register.

The split to implement (ozkey-17 §6a, metadata vs content):

| Field | Treatment |
|---|---|
| `device_id`, `ts`, message size | **stays plaintext** — you need it to route, to update `last_seen_at`, and to flush the queue on wake |
| `result` (`granted`, `bond_revoked`, …) | **sealed** |
| `detail` (`BLE unlock (owner)` / `(member)`) | **sealed** |

Firmware half is mine — the sealed lock→app channel exists and is proven
(ozkey-17 U1). Coordinate with me on the payload shape before building; do
not guess it from this table.

**Product consequence to flag, not to decide alone:** once sealed, hotel
and PM audit trails can only be assembled by something holding a bond key —
the operator's own app or on-prem server, not the relay. That is arguably
more correct for the sovereignty story, but it changes how OZLODGE/OZPMS
deliver "full audit trail." Raise it; do not resolve it silently.

### S14 — Multi-tenant isolation design (design doc, not code yet)

The platform's aim is that each organisation hosts its own network. What
has never been written down is what actually keeps them apart at scale.
Cover at minimum:

- **`site_id` collisions.** It is an operator-chosen string today. Two
  independent deployments both choosing `hotel` or `lab` is near-certain at
  scale. Note that ozkey-17's K1 (`server_id` in the HKDF salt) makes this
  cryptographic rather than advisory — a cross-site message cannot decrypt,
  not merely gets rejected. That is a strong argument for K1's priority.
- **Broker topology.** A single broker will not hold a large fleet.
  `ozkey/<site>/…` needs to resolve to *which broker*, not only which
  topic. Sharding axis, discovery, failover.
- **Device identity.** `device_id` is `ozk-<mac>` — a borrowed namespace.
  Espressif MACs are administered so there is no collision risk, but a
  silicon-vendor change breaks the scheme, and counterfeit modules with
  duplicated eFuse MACs are a real supply-chain risk at volume. Recommend a
  platform-assigned device UUID at provisioning as the canonical identity,
  with MAC kept for manufacturing/RMA. Design it; do not implement yet.
- **Cross-org RF coexistence.** Thread networks are cryptographically
  isolated by network key, so this is not a data-leak question — but BLE
  discovery (every lock advertises as `OZLOCK`) and 2.4 GHz congestion are
  real at density. Document the failure modes.

### S15 — OZLODGE functional + design spec

Only after S12–S14. Hospitality is the nearest adjacent market and shares
the most with what is built.

**Mandatory discipline:** every claim gets a status in a register at the
top of the document, exactly as `ozlock.md` / `ozlodge.md` / `ozpms.md` now
do — `VERIFIED` (with evidence cited) / `UNMEASURED` / `DESIGN ONLY` /
`INCORRECT — corrected`. Errors are recorded visibly, not silently fixed.
The operator's rationale: this register is evidence of genuine development
maturity for later IP work, and a register showing "claimed X, tested it,
found X false, corrected to Y" is worth more than one showing only polished
end-states.

Do **not** design the web frontend yet. A frontend encodes workflows, and
several workflow-defining decisions are still open (owner recovery, the
pending-vs-confirmed model, whether audit trails are sealed). Building UI
now means rebuilding it.

### HELD — OZPMS

**Do not write further OZPMS specification.** Not a capacity decision; a
correctness one:

- Its headline commercial feature — remote lockout for rent arrears — was
  rewritten on 2026-08-10 because, as specified, it describes an **illegal
  eviction**. Denying a residential tenant access without a tribunal order
  is unlawful in every Australian state, and the named target buyers
  (public housing, defence housing) are the most exposed to that scrutiny.
- Its LoRa option is design-only with no hardware evaluated.
- Its 1M-property tier has never been load-tested at any scale.
- Its defence compliance table asserted "Espressif, Taiwan" — Espressif is
  Shanghai-headquartered and STAR Market listed, i.e. the table stated the
  opposite of the truth in the one place a defence evaluator would check.

What OZPMS needs next is **domain validation** — a tenancy lawyer, and one
real PM company — not more pages from us. Elaborating a specification whose
commercial premise just moved multiplies the correction work.

---

## 3. Key references — what is authoritative for what

| Question | Authoritative source |
|---|---|
| OZKIE protocol, semantic `kind` contract | `ozkey-17.md` §6c — **normative**, hardware-verified |
| Thread multicast behaviour | `ozkey-17.md` §6d — measured, supersedes all earlier assumptions incl. bridge32 source comments |
| Metadata vs content (what a relay may read) | `ozkey-17.md` §6a |
| v2→v3 envelope migration rules | `ozkey-17.md` §5e — safety-critical, read before touching key derivation |
| Sealed grant/delete cutover | `ozkey-13.md` (S1–S9) |
| Bond-verb routes (S11) | `ozkey-14.md` — done, live |
| Product claims + their verification status | Status Registers atop `ozlock.md`, `ozlodge.md`, `ozpms.md` |
| BLE GATT contract | `blelock/CONTRACT.md` |
| Firmware reference implementation | `blelock/common/ozdoorlock_core.h` — **when this and any doc disagree, the firmware is right and the doc is stale.** Say so and it gets fixed. |

---

## 4. What is NOT server-team work

Listed so nobody duplicates: firmware (mine), the BANOI/MAOI app (ftpos),
G1/G2 (mine), G3 (joint design, operator-gated).

Open app-side items are tracked in `XFtposDecisions-84.md` §13–§15 — a
fail-open on remote revoke, a sync that can only delete and never
reinstate, and the downlink framing. Mentioned only so the server team
knows why remote revoke is not yet end-to-end working; nothing there is
theirs to fix.

---

## 5. Sequencing summary

```
NOW      S12 rename ──► S13 C7 sealed log ──► S14 multi-tenant design
                                                      │
G1 battery ─┐                                         │
G2 OTA ─────┼─► gates ────────────────────────────────┴─► S15 OZLODGE spec
G3 recovery ┘                                                  │
                                                               ▼
                                            OZLODGE frontend (after workflows settle)

OZPMS ── HELD pending domain validation (tenancy law, real PM company)
```
