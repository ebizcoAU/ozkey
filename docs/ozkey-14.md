# ozkey-14 — sealed grant/delete end-to-end test (ozkey-13 rollout)

**Channel note:** this doc is the coordination point with the parallel session
working `ozlockserv` server-side (S1-S6, `[[ozkey-13-server-status]]` memory,
commit `fffcf53`). App-team coordination stays in
`ftpos/ftposDecisions/XFtposDecisions-69.md` as before — this doc does not
duplicate that thread, only references it.

## Status as of 2026-08-08, session handoff (operator asleep ~6h)

**Firmware — DONE, hardware-flashed, confirmed alive:**
- `doorlock-1.24` → DoorA (`ozk-acebe639f8c4`). F7 (sealed envelope over
  Thread UDP relay) included.
- `bridge32-1.8` → bridge (`ozb-98a316a7e638`). BR1 (relay generalized to
  carry `envelope_hex` or legacy `payload`) included.
- DoorB (`ozk-b0a6048b5fd8`) and DoorC (`ozk-acebe63acab8`) still on
  pre-migration firmware — not flashed, not needed for the first test (DoorA
  is sufficient), flash before broader/multi-lock testing.

**Server — DONE (parallel session), live-verified against real broker/DB:**
- S1/S2/S5/S6 committed `fffcf53`. S7 (bridged-lock relay, this session)
  committed `14898c4`, live-reloaded under `node --watch`, confirmed healthy.
- S3/S4 deliberately NOT run (cutover-only, needs an explicit operator
  decision once every app+firmware build in the field is sealed-capable).

**App — NOT YET READY. This is the current blocker, not firmware/server.**
Per `XFtposDecisions-69.md` §6 (2026-08-08 morning): ftpos confirmed A1/A2
already existed in their codebase, agreed A3/A5 + the revoke slot-lookup +
A7 (PIN-redisplay fix) were buildable, gave a **3-4 day estimate**, said
they'd "proceed on that basis" and flag if server timing was an issue. No
newer reply as of this doc. **Do not expect a sealed-capable app build
imminently** — check XF-69 for an update before assuming Step 3 is ready.

## Test sequence (operator's plan, reproduced here for the server-side
    session to see)

| Step | Action | Team | Deliverable |
|---|---|---|---|
| 1 | Flash `doorlock-1.24` on DoorA | Firmware | ✅ DONE — confirmed running |
| 2 | Flash `bridge32-1.8` on the bridge | Firmware | ✅ DONE — confirmed running |
| 3 | Build and push app with sealed grant support | App | ⏳ NOT READY — ftpos estimate 3-4 days, no ship confirmation yet |
| 4 | Issue a PIN from the app (sealed grant) | App | Blocked on 3 |
| 5 | Monitor server: `envelope_hex` stored, `raw_value` absent | Server | Blocked on 4 |
| 6 | Monitor bridge: `envelope_hex` forwarded over Thread UDP | Firmware | Blocked on 4 |
| 7 | Monitor lock: envelope opens, DPID 21 forwarded to MCU | Firmware | Blocked on 4 |
| 8 | Verify PIN stored on the lock (MCU) | Firmware | Blocked on 4 |
| 9 | Revoke the PIN from the app (sealed delete) | App | Blocked on 4 |
| 10 | Monitor lock: DPID 22 forwarded, PIN removed | Firmware | Blocked on 9 |

## What can be verified right now without the app, if useful before Step 3
    lands

`ozctl.py mqtt-grant`/`mqtt-delete` (built earlier this session) can exercise
steps 5-8 and 10's firmware/server mechanics directly — it builds and seals
the identical DP 21-24 frames the app eventually will, publishes straight to
the MQTT broker, bypassing `ozlockserv`'s `POST /grants` (so it won't
exercise the server's own grant/metadata bookkeeping, only the relay+
firmware chain). Useful as an early sanity check on the firmware/relay side
while waiting on the app, not a substitute for the real end-to-end test.

## Resume point

1. Check `XFtposDecisions-69.md` for an app-side update before assuming
   Step 3 is ready.
2. If still waiting, optionally run the `ozctl.py mqtt-grant` firmware-only
   sanity check on DoorA (bench identity needs to hold bond #0 on DoorA —
   verify with `list_bonds` first, don't assume from a prior session, per
   `[[feedback-reverify-bench-identity-ownership]]`).
3. Serial (`duallog.py`) + MQTT (`mqttlog.py`) monitoring is OFF as of
   session end — re-arm before either the sanity check or the real test.
