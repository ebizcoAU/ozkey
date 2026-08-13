# ozkey-30 — Commercial-tier audit trail: the `ozkey-29` follow-up, for LATER development

**Status: 🟡 DIRECTIVE, operator-directed 2026-08-13, FOR LATER DEVELOPMENT —
not scheduled, not to be started now.** This is `ozkey-29 §6`'s promised
follow-up ("OZPMS/OZLODGE... gets its own follow-up doc") placed on record
so the direction is captured before it's forgotten, not a current work item.
Addressed to: server team (`ozlodgeserv`), whenever commercial-tier work
resumes.

---

## 1. The directive, verbatim (operator, 2026-08-13)

> *"Server Team (ozlodgeserv – Commercial)*
> *Implement the sealed event delivery (same query_events mechanism).*
> *Store events on the on-prem server (the hotel's own database). This is
> the audit trail.*
> *Configurable retention (default 30 days) – this is the operator's
> retention knob. It applies to the hotel's own audit trail, not to the
> cloud relay."*

## 2. Why this is a *different* answer from `ozkey-29`, not a contradiction

`ozkey-29 §10.2` flagged a real tension: the operator had told ftpos
"user-configurable retention, default 30 days" for `ozlockserv`
(residential), which read plainly against the "no log" claim `ozkey-29`
exists to make true. This directive resolves that tension directly: **the
30-day figure was never about the residential cloud relay.** It belongs to
the commercial on-prem server, which has a different, legitimate job.
`ozkey-29 §10.2`'s proposed reconciliation (route the 30-day knob onto the
sealed buffer's delivery TTL) is now superseded by this cleaner answer — the
number simply doesn't apply to `ozlockserv` at all.

## 3. Grounding, checked against `docs/ozlodge_v2.2.pdf` before writing this

Read the spec directly rather than assume the directive's framing —
confirms it, doesn't just repeat it:

- **§2.2.3** (on-prem local server, named "OZKEYSERV" in this spec — a
  known dead name per `ozkey-27 §6.4`, not corrected here, flagging again):
  *"runs a local MySQL instance storing room rosters, backup PMS database
  tables, **audit trails**, and coordinates with third-party payment and SMS
  gateways locally."* Storing the audit trail is the on-prem server's job
  *by design*, not a gap to close.
- **§5.1.1 comparison table**: *"Transaction Logs — [competitor] Stored
  indefinitely on third-party servers vs. [OZLODGE] **Stored only locally on
  the hotel's on-premises server**."* The commercial architecture's own
  sovereignty claim is that the trail exists, on hotel-owned hardware, under
  hotel custody — the opposite emphasis from residential's "no log at all."
- **Appendix, status register, L8**: *"Full audit trail on hotel's own
  server — ACHIEVABLE — framing corrected; doesn't conflict with E2E blind
  relay."* Already reconciled once, in the source spec itself: the on-prem
  server holding a trail and the cloud relay staying blind are not in
  tension — they're two different components with two different jobs, same
  as `ozkey-27 §6.1`'s trust-model fork (on-prem server as credential
  authority for hospitality, cloud relay is not).

## 4. What this actually asks for, decomposed

1. **Sealed event delivery — same `query_events` mechanism as `ozkey-29
   §11.4`.** Cursor-based, per-record sealed, `dropped_before_seq` loss
   admission — no new design needed, reuse what firmware specified for
   residential verbatim. The wire contract between lock and consumer doesn't
   change; only *who* the consumer is, and what it does with what it
   receives, differs by tier.
2. **The on-prem server is the consumer, and it stores what it receives.**
   Unlike `ozlockserv` (which per `ozkey-29 §5.2`/`§12` now forgets grant
   metadata immediately and never becomes the events consumer at all — the
   app is), the on-prem server *is* the audit trail's home. This is the
   trust-model fork `ozkey-27 §6.1` already named: the on-prem server has an
   authority role residential's cloud relay structurally does not.
3. **Configurable retention, default 30 days, on the on-prem server's own
   copy.** This is genuinely a bounded-plaintext-retention design (the
   pattern `ozkey-29 §1` argues against *for a zero-knowledge broker
   claim*) — and that's fine here, because the on-prem server was never
   claimed to be zero-knowledge. It's the hotel's own database, under the
   hotel's own custody, which is precisely what makes a retention window a
   legitimate operational knob rather than "policy sitting on top of
   retained access" — there's no third-party custodian for a subpoena or
   breach to reach past.

## 5. Explicitly not resolved here

- **Naming.** "OZKEYSERV" (this PDF), "the on-prem server," and whether it's
  the same codebase as `ozlodgeserv` deployed in a different mode, or a
  distinct component, is unresolved — `ozkey-27 §6.4` flagged this once
  already for v2.3. Not re-litigated in this directive; whoever picks up the
  implementation should settle it first rather than build against an
  ambiguous name.
- **Server-vs-server split.** Per the operator's own phrasing ("the cloud
  relay" vs. "the hotel's own database"), there appear to be two distinct
  server roles in the commercial topology — worth confirming whether both
  live in the `ozlodgeserv` directory (config-switched) or are genuinely
  separate deployables before writing code.
- **Nothing here is scheduled.** `ozkey-29` (residential) is fully
  implemented and live as of this session; this directive is a placeholder
  for when commercial work resumes, not a task in flight.

## 6. Status

🟡 RECORDED. No code written, none expected until commercial-tier work is
explicitly picked back up. `ozkey-29 §10.2`'s open reconciliation question is
now closed by this doc — the 30-day figure was always this tier's, not
residential's.

---

*Server team, 2026-08-13. Supersedes `ozkey-29 §10.2`'s proposed
reconciliation. Companion docs: `ozkey-29` (residential, the completed
sibling of this directive), `ozkey-27 §6` (the trust-model fork this
directive is downstream of), `docs/ozlodge_v2.2.pdf` (source spec).*
