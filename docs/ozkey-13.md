ozkey-13 — ozlockserv relay-opaque migration + async orchestrated removal (PM priority #1, Option B)
Status 2026-08-08 — consolidated:

Layer	Tasks	Status
Firmware	F1-F7 (BLE/MQTT sealed dispatch, bridge relay)	✅ Code complete, compiled, committed, **flashed and reconfirmed 2026-08-08 22:16** (doorlock-1.24 on DoorA / ozk-acebe639f8c4, bridge32-1.8 on the bridge / ozb-98a316a7e638 — both hash-verified via esptool, DoorA's flash showed changed sectors and reflashed clean, bridge's was byte-identical confirming last night's flash held)
Server	S1-S7 (accept envelope, store metadata, drop raw_value, bridge relay)	✅ Done, committed, live-verified
Server	S8-S9 (app-to-app MQTT topics for orchestrated removal)	✅ Done, committed `e6e00b1`, live-verified — plus a broker-ACL production-readiness gap flagged (`9dbf422`, see §8 detail)
App	A1-A7 (sealing, PIN cache, revoke)	✅ Done, committed
App	A8-A12 (one-action UX, date/time picker, default 24h, orchestrated flow)	🔴 New spec – not yet built
S3/S4	Drop grants.raw_value, delete buildCredentialFrame()	✅ Done, committed, live-verified
Remaining work: A8-A12 (app) – the async orchestrated removal UX, plus the date/time picker for grants. S8-S9 (server) are done. These are tracked in ozkey-15.md and XF-70.

1. What's actually broken — two things, now fixed
1a. Server-side plaintext storage. (Fixed by S1-S7, S3/S4 cutover, fffcf53, b00671f – grants.raw_value dropped, buildCredentialFrame()/buildDeleteFrame() deleted.)

1b. The MQTT command channel has no per-command authentication. (Fixed by F1-F7, BR1, bridge relay – all MQTT commands now sealed, including over Thread bridges.)

2. Why the current split exists — unchanged
ozdoorlock_core.h:2480-2485, inside ozControlDispatch() (the sealed-envelope handler), states the reasoning explicitly:

v1 carries exactly one MCU verb over BLE: DP 1, the at-the-door unlock. Credential frames 21-24 are NOT accepted here — they reach the MCU on the server path, which is where issuance lives. Accepting them over BLE too would mean two authorities for one operation, and the BLE one has no server-side record of what it issued.

This is a real, deliberate constraint, not an oversight: something needs to remain the record of "what PINs/RFIDs currently exist on this lock" for the management UI (list active credentials, revoke by slot) and for slot-collision avoidance. Today that's grants in MySQL. Option B1 keeps metadata only (grants drops raw_value).

3. What "full parity with the bond ceremony" requires, concretely
The bond ceremony's pattern: app builds the DPID frame, wraps it in challenge(16B) ‖ frame, seals with AES-GCM using a key derived from the bond's X25519 pairing secret, writes app_id_hex ‖ envelope to the BLE control characteristic. Lock opens it (ozControlTry()), dispatches by DPID (ozControlDispatch()), forwards to MCU if it's an MCU verb. Fully authenticated, fully sealed, zero plaintext transits or persists anywhere outside the two endpoints.

Applying that to credential issuance is NOT a small change, because credential issuance today is a remote/queued operation (issue a temp PIN for a cleaner without being at the door), not an at-the-door BLE operation like unlock/revoke:

App: build + seal the grant/delete frame using the same per-bond key derivation already built for control — but sealed offline, without a live BLE connection to the target lock. This matters: BLE's freshness check is challenge == last-issued, read live over the challenge characteristic (…0005) at connection time. A queued/remote command has no live connection at seal time, so it cannot echo a fresh challenge — freshness must fall back to the bond's monotonic counter alone (counter > floor), the same guarantee list_bonds etc. ultimately rest on, but without the extra live-challenge layer BLE commands get. This is a genuine, structural security-property difference between BLE-direct and remote-queued sealed commands — documented as an honest difference, not hidden.

Firmware: a NEW entry point that opens a sealed envelope arriving over MQTT (not just BLE). The open/dispatch logic (ozEnvOpen, bond lookup, counter check) is already written for ozControlTry() — this is a factoring job, not new crypto: pull the "open + verify" core out so both the BLE control characteristic and the MQTT command topic can feed it, with the frame source as the only difference. Then extend ozControlDispatch()'s allowed-DPID set to include grant/delete (21-24), role-gated to bond #0 like 101/102/103, forwarding to MCU exactly like DP 1 does today.

Rollout: don't repurpose payload_hex for this — add a distinct envelope_hex field so pre-B firmware (which does a blind forward) and post-B firmware (which requires a sealed envelope) can coexist during rollout. The server picks which field to send based on the lock's reported fw/caps.

Server: POST /locks/:id/grants accepts envelope_hex from the app instead of raw_value + type + dates. Drop buildCredentialFrame()/buildDeleteFrame() server-side entirely — the server never builds a credential frame again, only relays what the app already sealed. grants.raw_value is deleted from the schema, not just left unused.

4. Credential-record question — RESOLVED: B1
grants stays, drops raw_value only: keeps slot_number, type, user_name, date_from/to, sync_status. Enough for the management UI (list credentials, revoke by slot, show who/when) without ever holding or deriving the actual PIN/RFID value — the server genuinely cannot answer "what is Bà Ngoại's PIN" even under subpoena or breach, which is the actual sovereignty property being promised.

5. Remote-command freshness — RESOLVED: counter-only is accepted
BLE's challenge == last-issued live round-trip has no equivalent for a queued/remote command (no live connection at seal time). Confirmed acceptable to fall back to the bond's monotonic counter alone (counter > floor) for this case — document the difference honestly rather than imply parity with BLE's freshness guarantee: BLE commands get counter + live-challenge; MQTT-sealed commands get counter only.

6. MQTT authentication gap — RESOLVED: bundled in, not deferred
§1b above (originally scoped out) is now explicitly in scope: the same sealed-envelope machinery being built for grant/delete extends to every MQTT command — remote unlock (DP 1), factory reset, assisted unlock included. One mechanism, not a credential-only special case. onMqttMessage() moves from "pure forward" to "open sealed envelope, then dispatch" for every verb it currently blind-forwards.

7. Sign-off — all confirmed 2026-08-08
#	Decision	Outcome
1	Credential record model	B1 — metadata only, raw_value dropped
2	Counter-only freshness for remote commands	Accepted — documented as honest difference
3	MQTT authentication gap	Bundled — same machinery covers all MQTT commands
4	User‑selectable expiry (date + time, default 24 hours)	Confirmed
5	One‑action "Xoá khỏi danh sách": self‑revoke + orchestrated fallback	Confirmed
6	Orchestrated removal: banoi2 → MQTT → banoi1 → doorlock	Confirmed
8. Work breakdown by team
Firmware (ozkey) – ✅ DONE
Task	Description	Status
F1	Factor ozControlTry() open/verify	✅ Done
F2	MQTT sealed-envelope entry point	✅ Done
F3	Extend ozControlDispatch() for DPID 21-24 + role gate	✅ Done
F4	Role-gate DPID 21-24 to bond #0	✅ Done
F5	Add envelope_hex to MQTT command topic	✅ Done
F6	Legacy payload_hex support	Later
F7	pollThreadUdp() sealed path (Thread relay)	✅ Done
BR1	Bridge relays envelope_hex over Thread UDP	✅ Done
FW_DISPLAY_VERSION	Fix stuck version string	✅ Done
Acceptance: All compiles clean; pending flash and bench test.

Server (ozlockserv) – S1-S7 ✅ DONE, S8-S9 🔴 NEW
Task	Description	Priority	Status
S1	Accept envelope_hex in POST /grants	High	✅ Done
S2	Store envelope_hex in pending_queue	High	✅ Done
S3	Drop grants.raw_value column	High	✅ Done
S4	Delete buildCredentialFrame() / buildDeleteFrame()	High	✅ Done
S5	Keep grants metadata	High	✅ Done
S6	Add msg_type marker to pending_queue	Medium	✅ Done
S7	Bridge relay for envelope_hex	High	✅ Done
S8	Add app-to-app MQTT topics: ozkey/<site>/members/<device_id>/request_remove and .../ack_remove	High	✅ Done
S9	Relay these topics (pure relay, no state)	High	✅ Done
S8/S9 detail: These support the async orchestrated removal flow (see §9). The server does not persist any state; it only observes for visibility/audit (event ring, no DB write) — implemented observe-only rather than active republish, since the spec's topic table names the same topic for both hops and the broker already delivers publish→subscribe directly between banoi1 and banoi2 (an active republish onto that same topic would self-loop, given the server's own subscription). Full reasoning in ozkey-15.md §8.1.

**Production-readiness finding, S8/S9 (2026-08-08):** verified live that the lab Mosquitto broker enforces no credentials at all — `mosquitto_pub` with a fabricated username and wrong password still published successfully. ozkey-15's own trust model assumes "app_id and MQTT ACLs already handle routing" for S8/S9's routing/authorization; that assumption doesn't hold yet. Recorded as an explicit pre-production blocker in `ozlockserv/server.js`'s file header (commit `9dbf422`) and in ozkey-15.md §8.1 — configuring real ACLs on the production broker (EMQX, not this lab's Mosquitto) is deliberately deferred (operator instruction), not actioned this pass.

App (ftpos) – A1-A7 ✅ DONE, A8-A12 🔴 NEW
Task	Description	Priority	Status
A1	Client-side sealing for grant/delete (already built)	High	✅ Done
A2	Adapt sealing for offline/no-live-challenge (already built)	High	✅ Done
A3	Send envelope_hex to server instead of raw_value	High	✅ Done
A4	Keep legacy raw_value path (fallback)	Medium	✅ Done
A5	Update POST /grants call site to sealed path	High	✅ Done
A6	BLE grant/delete – not needed	—	✅ Done
A7	PIN re‑display UX – cache PIN locally; metadata‑only fallback	High	✅ Done
A8	One‑action "Xoá khỏi danh sách" – try self‑revoke first, fallback to orchestrated removal	High	🔴 New
A9	Orchestrated removal: banoi2 sends MQTT request to banoi1 via ozlockserv; retry every 15min + on app open until ACK	High	🔴 New
A10	banoi1 listens for removal requests on MQTT; auto‑grants DPID 101 admin revoke over BLE when near the lock	High	🔴 New
A11	Date + time picker for expiry (default 24 hours)	High	🔴 New
A12	Display expiry with time: "đến [date] lúc [time]"	High	🔴 New
A8-A12 detail: Full spec in ozkey-15.md and XF-70. The one-action UX eliminates confusion: a user taps "Xoá khỏi danh sách", the app tries BLE self‑revoke; if that fails (not at door / bond gone / timeout), it sends a request to the admin via MQTT, with retry and auto‑grant on the admin side. The user never needs to choose between two actions. Expiry is user‑selectable (date + time) with a sensible default of 24 hours.

ftpos findings, XF-69 §6 (2026-08-08), recorded here:

A1/A2 were already built, not new work — Keyring.sealAddTempPin/sealAddTempRfid/sealDeletePin/sealDeleteRfid (keyring.dart:263-295) already seal DPID 21-24 with no challenge parameter at all (built under XF-42/46 for a server path that was never finished).

A3/A5 are real, scoped work — issuePin() (doorlock_service.dart:1472-1504) still calls plaintext raw_value; envelope_hex didn't exist yet on the request/response types (directory_client.dart:574-592). Now fixed.

Revoke is further behind than issue — revokeGrant() doesn't build a frame client-side at all today, sealed or plaintext (bare DELETE /grants/:grantId). Reaching parity needs an extra step: look up the grant's slot_number (already returned by listGrants()) and build/seal the DPID 22/24 delete frame before the call. Now fixed.

9. Async orchestrated removal flow – spec summary (ozkey-15)
MQTT Topics
Topic	Direction	Payload
ozkey/<site>/members/<admin_device_id>/request_remove	banoi2 → ozlockserv → banoi1	{ request_id, target_lock_id, target_member_app_id, timestamp }
ozkey/<site>/members/<banoi2_device_id>/ack_remove	banoi1 → ozlockserv → banoi2	{ request_id, target_lock_id, target_member_app_id, status: ok/fail }
banoi2 (member) logic
Tap "Xoá khỏi danh sách".

Attempt BLE self‑revoke (DPID 101) – immediate.

If succeeds → door removed from list, done.

If fails (timeout/out of range/bond missing) → send MQTT request to admin.

Display: "Đang chờ xoá – chủ nhà sẽ xác nhận khi đến gần cửa".

Retry every 15 minutes + on each app open until ACK received.

On ACK → remove door from list.

banoi1 (admin) logic
Subscribe to ozkey/<site>/members/<banoi1_device_id>/request_remove.

On receiving request:

If near the lock (BLE range): execute DPID 101 admin revoke → send ACK.

If not near the lock: queue locally; retry when BLE range is detected (on app foreground or periodic scan).

Send ACK back to banoi2 via MQTT.

Fallback
If banoi2 is at the door, self‑revoke still works (the primary path).

If admin is never near the lock, the request remains queued – but the member can still self‑revoke if they ever return.

10. Expiry – date + time, default 24 hours
Aspect	Implementation
Default	24 hours from issuance
User override	Date + time picker – user can select any date/time
Display	"đến [date] lúc [time]"
Storage	Full timestamp in date_to
Offline enforcement	Lock checks against its clock (if synced)
Server fallback	Server revokes at exact time via MQTT
11. Security properties — before / after
Aspect	Before	After
PIN stored on server	Plaintext in grants.raw_value	No PIN anywhere on the server
Server can read credentials	Yes (plaintext)	No (sealed envelopes)
Server can build frames	Yes (buildCredentialFrame())	No (app builds/seals)
MQTT command authenticated	No (broker ACL only)	Yes (sealed envelope + counter)
Remote command freshness	Depends on lock wake, no cryptographic freshness	Counter-based, honestly documented as weaker than BLE's counter+challenge (§5)
Breach exposure	All PINs, all logs	Ciphertext + non-secret metadata only
12. Success criteria – updated
Criteria	Verification	Status
No plaintext PINs in grants	MySQL grants has no raw_value column	✅ Done
Server relays envelope_hex only	Server code review + live test	✅ Done
App sends envelope_hex	POST /grants captures show no raw_value	✅ Done
Lock opens sealed envelope over MQTT and Thread bridge	Bench test: issue PIN via app, lock stores it	⏳ Pending flash
Legacy locks still work	payload_hex path continues to work during rollout	✅ Done
Member cannot issue PIN	Non-admin bond #N gets UNLOCK_DENIED on DPID 21-24	✅ Done
One‑action "Xoá khỏi danh sách" works	App UI test	🔴 A8-A12
Orchestrated removal works end‑to‑end	Full test: banoi2 → MQTT → banoi1 → doorlock	🔴 A8-A12 (server S8-S9 done, verified reachable via mosquitto_pub/sub)
Date/time picker works with default 24h	App UI test	🔴 A11-A12
Expiry display shows time	App UI test	🔴 A12
13. Related documents
ozkey-14.md – end‑to‑end test plan (firmware/server/app coordination, bench steps)

ozkey-15.md – full spec for async orchestrated removal (MQTT topics, retry, auto‑grant)

XF-70 – cross‑team UX change doc (one action, copy, orchestration)

14. Next steps
Priority	Action	Owner
1	Flash doorlock-1.24 and bridge32-1.8 on bench locks	Firmware (with operator)
2	Implement A8-A12 (app UX + orchestrated removal)	App Team
3	Run integration test (sealed grant + orchestrated removal)	All
4	(ozkey-16, PM priority #2) Decide the four open questions in §4, then U1-U6 — bridge32 MQTT uplink for Thread lock heartbeat/logs	Firmware + Server (U4)