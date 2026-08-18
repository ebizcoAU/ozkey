# ozkey-40 — "Remove lock from app" left LockB owned by nobody: three ways it can fail, and one of them is an unauthenticated wipe

**Status: 🔴 OPEN — bench report, root cause NOT yet isolated.** Written
2026-08-18 by **firmware**, from an operator bench observation the same day.
Consumers: **app (ftpos/BANOI)**, **server**, PM/operator.

Companion docs: `XFtposDecisions-114` (the app-facing ask this doc backs),
`XF-113` (same failure class — the app reporting success for a command the door
never received), `ozkey-23` (the sealed remote factory reset), `ozkey-13` S8/S9
(the broker enforces no credentials), `CONTRACT.md` §bond-0.

---

## 0. The observation

Operator, 2026-08-18:

> *"banoi2 hosts lockB, can open lockB, tested. I removed lockB from banoi2 and
> the app did remove the doorlock — but lockB failed to factory reset. Note I
> did this while the BLE window was still on."*

So: **the app's state changed and the lock's did not.** The app now believes it
has no relationship with LockB. LockB still believes it is owned by BANOI2.

**Why that specific end state is expensive.** Bond #0 is exclusive and a factory
reset is the ONLY thing that clears it (`CONTRACT.md` §350, `ozcrypto.h:274`).
A lock whose owner has forgotten it cannot be re-paired by anyone — not the
original owner, not a new one. The only recovery is physical: hold BOOT for 5 s
at the door. On a deployed site that is a truck roll, per lock.

**This is not a cosmetic sync bug. It is a lock that can no longer be
administered by software.**

---

## 1. 🔴 Firmware defect found while investigating: the wipe is unauthenticated on Wi-Fi locks

Independent of the bug above, and worse.

`ozdoorlock_core.h:3789` — inside `onMqttMessage()`, before any authentication:

```c
const char *op = doc["op"] | (const char *)nullptr;
if (op && (strcmp(op, "factory_reset") == 0 || strcmp(op, "unpair") == 0)) {
  Serial.println("[MQTT<-] factory_reset (unpaired by app/server)");
  factoryReset();
  return;
}
```

There is **no seal check, no bond check, no sender identity** between the JSON
parse and the wipe. The lock subscribes to its command topic on a broker that
`ozkey-13` S8/S9 recorded as enforcing **no credentials at all** — a fabricated
username still publishes.

**Therefore: anyone who can reach the broker can factory-reset every Wi-Fi lock
on the site, one publish each, and the only recovery is physical access to every
door.**

The sealed OZKIE path at `ozdoorlock_core.h:5285` was built for exactly this
reason and its own comment says so — *"'wipe this lock' is the most destructive
command we have. Unauthenticated, on a broker that today enforces NO credentials
at all, it is a one-line site-wide DoS."* It is owner-only (`slot != 0` →
`REVOKE_DENIED`). **The old unauthenticated path was never removed.** Both are
live; the sealed one is bypassable by simply not using it.

🔴 **NOT FIXED IN THIS DOC, deliberately.** If the app's current removal flow
depends on the bare `op`, deleting it removes the app's only working removal
path on Wi-Fi locks and turns one bug into two. Sequence: app confirms what it
sends (§3) → firmware deletes the bare path. **Operator's call on ordering.**

---

## 1b. 🔴 Operator's answers narrow this to one candidate — and it is a new one

Operator, 2026-08-18, answering §4:

1. **LockB is a Wi-Fi lock.** Identified on the bench without opening a port
   (`ioreg`, USB serial = MAC): **`B0:A6:04:8B:5F:D8`** → `ozk-b0a6048b5fd8`.
   *(This is the MAC to look up in NEXUS.)*
2. **Bond ownership unknown** — *"may be this pair before bond#0 introduce"*.
3. **The removal went over BLE.**

Wi-Fi + BLE **eliminates candidates A and C below**: the command did not need
MQTT and the lock was not asleep. The command had a live path and the lock was
in a position to answer.

**The pre-bond-#0 worry is probably unfounded**, which matters because it was
the most plausible reading. `ozcrypto.h:296` — M2→M3 migration is automatic:
*"a lock already carrying b0pub from M2 loads it into slot 0 on first M3 boot,
writes the table, then deletes the old keys."* And `ozBondsLoad()` reads **both**
record strides (88 and the pre-T4 80) precisely so a field upgrade cannot silently
come up with no bonds. So a bond predating the table is a **slot 0 owner** bond,
not a lost one. Still worth confirming with `list_bonds` — but it is not the
likely cause.

### 🔴 D — There is NO legacy route to a factory reset. At all.

The BLE control path branches on the first plaintext byte
(`ozdoorlock_core.h:5917`):

```c
const bool semantic = (body[0] == '{');   // '{' = OZKIE JSON, 0x55 = legacy DP frame
```

`factory_reset` lives **only** on the semantic branch (`ozSemanticDispatch`).
The legacy branch (`ozControlDispatch`) forwards Tuya DPs, and the only in-lock
verbs it recognises are **101 `bond_revoke`, 102 `invite_cancel`, 103
`list_bonds`** (`ozkie-legacy-v0.json`). **None of them is a factory reset,** and
none of them wipes the keypair, the mesh or the owner.

**And we already know this app is on the legacy branch.** Yesterday's bench log,
for the unlock that works: `[CTL] OPENED — bond 0, … DP 1 (legacy frame)`. It is
the same finding that keeps DP 76 from ever firing (`ozkey-39`, `XF-113 §10.2`).

So the leading explanation is not that the reset failed. It is that **no reset
was ever requested in a form the lock could act on**:

- **D1** — the app sent a legacy sealed DP frame. `ozSemanticDispatch` is never
  reached, `factory_reset` is never consulted, the lock forwards or rejects a DP
  and carries on owned.
- **D2** — the app sent **DP 101 `bond_revoke`** as its "removal". If BANOI2 is
  bond #0 this is **refused by design** — bond #0 is never revocable, because a
  lock that can revoke its own owner is a lock nobody can administer. And even
  if it were allowed, revoking a bond **is not a factory reset**: the keypair,
  the mesh membership and the ownership state all survive.

**Log fingerprint: `[CTL] OPENED — bond N, counter …, DP n (legacy frame)`** —
i.e. the lock happily *authenticated and executed* something, just not a reset.
That is why this looks like silence from the app side.

**This unifies three open bugs into one app-side migration.** Unlock DP
selection, DP 76 never firing, and remote removal are all the same root cause:
the app is still sealing legacy DP frames instead of OZKIE semantic verbs.

---

## 2. The three ways this can produce "app forgot, lock didn't"

Kept for the record. **A and C are eliminated by §1b**; B is weakened but not
closed. Each leaves a different fingerprint in the lock's serial log.

### A — Wrong transport: the message could never arrive

`{"op":"factory_reset"}` published to `locks/<id>/command`. **A Thread lock has
no Wi-Fi and never subscribes to MQTT**, so the message cannot reach it. This
was verified on the broker 2026-08-11 and confirmed on the bench, and is written
into the sealed path's own rationale (`ozdoorlock_core.h:5269`). It works on a
Wi-Fi lock, which is why it reads as a regression rather than a routing gap.

**Log fingerprint: nothing at all.** The lock never heard anything.

### B — Not the owner: the lock refused, and said so

The sealed path is **owner-only**. A member who was invited to a lock must never
be able to escalate from *"I have access"* to *"nobody has access"*.

```c
if (slot != 0) {
  Serial.printf("[OZKIE] %s REFUSED — bond %d is not the owner\n", kind, slot);
  notifyStatus("REVOKE_DENIED");
  return;
}
```

If BANOI2 holds a member bond on LockB rather than bond #0, the lock **answered
`REVOKE_DENIED` and the app removed the lock anyway.**

🔴 **The open BLE window makes this MORE likely, not less.** The operator flagged
the window as suspicious; it points the other way. An open window means the
command had a live path to the lock and the lock was in a position to answer.
A refusal that the app discards looks exactly like silence.

**Log fingerprint: `[OZKIE] factory_reset REFUSED — bond N is not the owner`.**

### C — Wi-Fi lock asleep: the command was published into a gap

A keep-alive lock sleeps between 60–600 s wakes. A non-retained command
published while it sleeps is simply gone. It cannot be fixed by retaining it —
`ozdoorlock_core.h:3785` records that a retained message on the command topic is
redelivered as a **replayed command on every reconnect**, which for
`factory_reset` means a lock that re-wipes itself forever.

**Log fingerprint: nothing, then a normal wake with no reset.**

---

## 3. The answer to the operator's question

> *"Should the app need BLE confirmation before it deletes the doorlock?"*

**Yes — and not only over BLE. The app must not discard its local state until
the lock has acknowledged, on whatever transport it used.**

Three reasons, all of them already true in this codebase:

1. **The failure is unrecoverable by software.** §0 — bond #0 is exclusive and
   only a factory reset clears it. Every other app/lock desync is self-healing
   on the next reconcile; this one is not.
2. **The firmware already sends the confirmation the app is discarding.** The
   sealed path calls `notifyStatus("FACTORY_RESET")` immediately *before* wiping,
   and the comment says exactly why: *"Tell the app before we go: factoryReset()
   ends in a platform reset and never returns."* On refusal it sends
   `REVOKE_DENIED`. **Both signals exist and ship today.** This is not a request
   for new firmware — it is a request that the app stop ignoring an answer it
   is already being given.
3. **Precedent, one week old.** `XF-113`: the app reported a successful unlock
   for a door that never received the command. Same root shape — *"I published
   it"* treated as *"it happened."* Removal is the higher-stakes instance of the
   identical mistake.

### 🔴 One caveat the app team must not miss

**`FACTORY_RESET` is a promise, not a report.** It is sent *before* the wipe
because `factoryReset()` ends in a platform reset and never returns — there is
no post-reset state left in which to tell anyone anything. So the ack means
*"understood, wiping now"*, not *"wiped"*.

The proof that the wipe actually happened is different and also already exists:
**a reset lock announces itself on `hotel/locks/unpaired/heartbeat`**
(`CONTRACT.md` §77). So the robust flow is two-stage:

- **Ack gates the delete** — no `FACTORY_RESET`, no local removal. `REVOKE_DENIED`
  must surface to the user as *"you are not the owner of this lock"*, not as a
  silent success.
- **The unpaired announcement confirms it** — reconcile against it, and if it
  never arrives, the lock is still out there owned and must stay visible in the
  app as needing attention.

---

## 4. What firmware needs to isolate the root cause

The single decisive artifact is **LockB's serial log across the removal**,
matched against §2's three fingerprints. Failing that, a broker capture of what
BANOI2 actually published.

Operator — to reproduce, firmware needs to know:

1. **Is LockB a Thread lock or a Wi-Fi lock?** (Splits A from C.)
2. **Is BANOI2 LockB's bond #0, or a member?** Bond ownership decays silently
   across sessions and the bench has been wrong about it before — as of
   2026-08-17 LockA's bond #0 was BANOI's, not the bench's. `list_bonds` over
   BLE answers this.
3. **Did the app use BLE or the network path** for the removal?

⚠ Firmware will not open LockB's serial port or re-run this without the
operator's say-so — the port open can reset the board and would destroy the
state that holds the answer.

---

## 5. Asks

| # | Owner | Ask | State |
|---|---|---|---|
| 1 | **app** | Gate local removal on the lock's ack; surface `REVOKE_DENIED` as a real failure. `XF-114`. | open |
| 2 | **app** | 🔴 **Does "remove lock" send OZKIE semantic JSON (`{"kind":"factory_reset"}`) or a legacy sealed DP frame?** Per §1b there is no legacy route to a reset, so this is now the decisive question. | open |
| 3 | **app/server** | Reconcile against `hotel/locks/unpaired/heartbeat` so a lock that was never wiped stays visible. | open |
| 4 | **firmware** | Delete the unauthenticated bare-`op` path (§1) — **after** ask 2 is answered. | 🟢 **PM-DIRECTED 2026-08-18**, item 3: *"Firmware — After app confirmation, remove the unauthenticated bare-op reset path."* Sequencing ratified; **held until the app answers ask 2.** |
| 5 | **operator** | §4 — answered 2026-08-18, see §1b. Remaining: `list_bonds` on LockB to confirm BANOI2's slot. | part-answered |

---

*No claim in this doc about what the app sent has been verified — firmware has
read its own code and the operator's report, and nothing else. §2 is three
candidates, not a diagnosis. §1 and §3 are read directly from shipping source
and are not in doubt.*
