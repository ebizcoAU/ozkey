ozkey-15 — Async Orchestrated Removal Flow (Member Self-Service + Admin Auto-Grant)
Author: PM
Date: 2026-08-08
Status: DESIGN, NOT YET BUILT
Related: ozkey-13.md (relay-opaque), XF-70 (UX change), ozkey-14.md (test plan)

1. Purpose
When a member wants to remove a door from their app list but cannot physically reach the lock to self‑revoke over BLE, the app orchestrates a remote removal flow:

text
banoi2 (member) → ozlockserv (MQTT) → banoi1 (admin) → doorlock (BLE DPID 101 admin revoke) → banoi1 → ozlockserv → banoi2
The user sees one action: "Xoá khỏi danh sách" (Remove from list). The app handles the complexity:

Try self‑revoke over BLE (instant, if user is at the door)

If that fails → send an async request to the admin

Admin auto‑grants (queues, executes when near the lock)

Member receives ACK → door removed from list

The admin never needs to manually approve anything. The server just relays messages; the admin's phone auto‑grants when it can reach the lock.

2. Overview
High‑Level Flow
text
┌─────────────────────────────────────────────────────────────────────────────┐
│                    ASYNC ORCHESTRATED REMOVAL FLOW                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  banoi2 (member)                                                           │
│      │                                                                     │
│      ▼                                                                     │
│  Tap "Xoá khỏi danh sách"                                                  │
│      │                                                                     │
│      ▼                                                                     │
│  Try BLE self‑revoke (DPID 101)                                            │
│      │                                                                     │
│      ├── SUCCESS ──► Door removed ✅ Done                                  │
│      │                                                                     │
│      └── FAIL (not at door / timeout / bond gone)                         │
│              │                                                             │
│              ▼                                                             │
│      Send MQTT request to admin via ozlockserv:                            │
│      ozkey/<site>/members/<admin_device_id>/request_remove                 │
│              │                                                             │
│              ▼                                                             │
│      banoi1 (admin) receives request                                       │
│              │                                                             │
│              ├── Near the lock? ──► Execute DPID 101 admin revoke over BLE │
│              │                     │                                       │
│              │                     ▼                                       │
│              │              Send ACK back to banoi2                        │
│              │                     │                                       │
│              │                     ▼                                       │
│              │              banoi2 receives ACK → remove door from list   │
│              │                                                             │
│              └── Not near the lock ──► Queue locally                       │
│                                         Retry when BLE range detected      │
│                                         (app foreground / periodic scan)   │
│                                                                             │
│  banoi2 retry strategy (if no ACK):                                        │
│      • Every 15 minutes                                                    │
│      • Every time the app opens                                            │
│      • Until ACK received                                                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
Trust Model
Participant	Role	Authority
banoi2 (member)	Initiates removal request	Cannot revoke admin‑owned bonds without admin
ozlockserv	Pure relay	No state, no authentication beyond MQTT ACLs
banoi1 (admin)	Executes the revocation	Has bond #0 → can admin‑revoke any member bond
Doorlock	Enforces revocation	Removes the member bond (DPID 101)
3. MQTT Topics & Payloads
S8-S9 (Server Team)
Topic	Direction	Payload
ozkey/<site>/members/<admin_device_id>/request_remove	banoi2 → ozlockserv → banoi1	See §3.1
ozkey/<site>/members/<banoi2_device_id>/ack_remove	banoi1 → ozlockserv → banoi2	See §3.2
3.1 Request Payload (banoi2 → banoi1)
json
{
  "request_id": "req_abc123",
  "target_lock_id": "ozk-acebe639f8c4",
  "target_member_app_id": "9d8a16dd1654fdc0cd8d990446b7706913285b2f2b2d30dec175c5ecf159a822",
  "timestamp": 1767225600000
}
Field	Type	Description
request_id	String	Unique ID generated by banoi2 (for dedupe)
target_lock_id	String	The lock the member wants to leave
target_member_app_id	String	The member's app_id (pubkey) – the bond to revoke
timestamp	Number	Unix timestamp (ms) of the request
3.2 ACK Payload (banoi1 → banoi2)
json
{
  "request_id": "req_abc123",
  "target_lock_id": "ozk-acebe639f8c4",
  "target_member_app_id": "9d8a16dd1654fdc0cd8d990446b7706913285b2f2b2d30dec175c5ecf159a822",
  "status": "ok",
  "timestamp": 1767225600000
}
Field	Type	Description
request_id	String	Echoed from the original request
target_lock_id	String	Echoed from the original request
target_member_app_id	String	Echoed from the original request
status	Enum	ok (success) or fail (revocation failed)
timestamp	Number	Unix timestamp (ms) of the ACK
4. banoi2 (Member) Logic – A9
4.1 Initiation
When user taps "Xoá khỏi danh sách" (Remove from list):

Try BLE self‑revoke (DPID 101) – this is the primary, immediate path.

If self‑revoke succeeds (REVOKE_OK) → remove door from list, done.

If self‑revoke fails (timeout/out of range/bond missing/REVOKE_DENIED):

Send MQTT request to admin.

Display: "Đang chờ xoá – chủ nhà sẽ xác nhận khi đến gần cửa"

4.2 Retry Strategy
Condition	Action
No ACK within 10 seconds	Start retry timer
Retry interval	Every 15 minutes
App opens	Retry immediately
ACK received (status: ok)	Remove door from list, stop retrying
ACK received (status: fail)	Display: "Không thể xoá – thử lại sau"; continue retrying
User cancels manually	Stop retrying (optional)
4.3 Deduplication
Each request has a unique request_id.

If banoi2 receives an ACK for a request it already processed, it ignores duplicates.

If banoi2 receives a stale ACK (from an old session), it checks the timestamp and ignores if too old.

4.4 Copy
State	Copy
Initial (before action)	"Xoá cửa này khỏi danh sách của bạn"
Waiting for admin	"Đang chờ xoá – chủ nhà sẽ xác nhận khi đến gần cửa"
Done	"Đã xoá khỏi danh sách"
Fail (ACK with fail)	"Không thể xoá – thử lại sau"
5. banoi1 (Admin) Logic – A10
5.1 Subscription
banoi1 subscribes to:

text
ozkey/<site>/members/<banoi1_device_id>/request_remove
5.2 On Receiving Request
Validate that the request is for a lock this admin owns (bond #0).

Check BLE range – is the lock reachable?

If reachable: Execute DPID 101 admin revoke over BLE.

Target bond = target_member_app_id (the member's pubkey).

Role‑gate: admin can revoke any member bond.

Send ACK (status: ok or fail).

If not reachable: Queue the request locally.

Retry when BLE range is detected (on app foreground, or periodic scan).

Also retry every 15 minutes.

5.3 Queuing
banoi1 maintains a local queue of pending removal requests (persisted in SQLite).

On app foreground or BLE scan:

Iterate queue, attempt each pending revocation.

On success → send ACK, remove from queue.

On failure → keep in queue, retry later.

5.4 Admin Auto‑Grant – No User Intervention Required
The admin does not need to manually approve the request.

The admin app auto‑grants when it can reach the lock.

Optionally, the admin can see a notification: "Đã xoá quyền của [member] khỏi cửa X" after success.

5.5 Copy (Admin Side)
State	Copy
Request received	No notification (auto‑grant)
Revocation succeeded	"Đã xoá quyền của [member] khỏi cửa X" (optional)
Revocation failed	"Không thể xoá quyền của [member] – thử lại sau" (optional)
6. Fallback (Member Self‑Revoke)
The member can always self‑revoke if they are physically at the door:

text
User is at the door → opens BLE window → taps "Rời khỏi cửa này" (self‑revoke) → lock removes bond
This is the primary path. The orchestrated removal is only a fallback for when the member cannot reach the lock.

7. Date + Time Picker for Expiry – A11/A12
7.1 User Flow
text
User taps "Cấp mã" (Issue PIN)
        ↓
Dialog appears:
    • Who for: [____________]
    • Valid from: [today] [now] (fixed)
    • Valid to:   [today + 1 day] [same time]  ← default is 24 hours
        ↓
User can change the date/time for "Valid to"
        ↓
App sends `date_to` with the selected timestamp
        ↓
Server stores metadata
        ↓
Lock enforces expiry (offline) or server revokes (online)
7.2 Default Expiry
Aspect	Value
Default	24 hours from current time
Why	Most temporary access (visitors, cleaners, tradespeople) is for 1 day
User override	✅ Yes – user can select any date/time
7.3 Display
Context	Format
Grant list	"đến [date] lúc [time]"
Toast on issue	"Đã cấp mã ${created.pin} – hết hạn vào [date] lúc [time]"
7.4 Implementation Notes
Component	Change
App UI	Date picker + time picker (or combined DateTime picker)
App (issueGrant())	Send full timestamp with time
Server	No change – already stores date_to as provided
Lock (MCU)	Compares date_to against its clock (if synced)
Server fallback	Revokes at exact time via MQTT (if lock is online)
8. Status Table
Ask	Description	Owner	Status
S8	Add MQTT app‑to‑app topics: request_remove + ack_remove	Server	✅ Done — live 2026-08-08
S9	Relay these topics (pure relay, no state)	Server	✅ Done — live 2026-08-08, see Server reply below for the relay-shape note
A8	One‑action "Xoá khỏi danh sách" – self‑revoke + orchestrated fallback	App	🔴 Open
A9	banoi2 orchestrated removal + retry (15min + on app open)	App	🔴 Open
A10	banoi1 listens for requests; auto‑grants DPID 101 admin revoke	App	🔴 Open
A11	Date + time picker for expiry (default 24 hours)	App	🔴 Open
A12	Display expiry with time: "đến [date] lúc [time]"	App	🔴 Open
9. Dependencies
Item	Depends On	Blocking
A8 (one‑action UI)	Self‑revoke already works; orchestration is new	None – can build UI first
A9 (banoi2 orchestration)	S8-S9 (MQTT topics)	Server must implement first
A10 (banoi1 auto‑grant)	S8-S9 (MQTT topics)	Server must implement first
A11 (date/time picker)	None	Can build anytime
A12 (display expiry)	A11	Can build anytime
Server S8-S9 is the only external dependency for orchestration. The UI can be built independently.

8.1 Server reply (S8-S9), 2026-08-08 — implemented, one relay-shape note and
one lab-environment caveat worth reading before app-side testing

**Implemented as observe-only, not active republish — reading §3's topic
table closely, they have to be.** Both `request_remove` and `ack_remove` name
the exact same topic string for the app→server and server→app hop
(`ozkey/<site>/members/<admin_device_id>/request_remove` appears once, not as
an inbound/outbound pair). Since the topic is already addressed to the
specific recipient's `device_id`, the MQTT broker delivers banoi2's publish
straight to banoi1's subscription with zero help needed from ozlockserv —
that's what a broker does natively for any two clients sharing a topic. If
ozlockserv had instead subscribed *and* republished onto that same topic (the
literal reading of "forward to the specific admin's topic"), it would be
subscribed to its own republish and loop forever, since source and
destination are the same topic. So `ozlockserv` subscribes to both wildcards
purely to log for visibility/audit (per §2 "no state, no persistence" — logged
to the in-memory event ring only, nothing written to `grants`/`pending_queue`
or any table), and does the "basic payload structure" check (valid JSON +
the three required fields) — it does not touch delivery. Live-verified with
`mosquitto_pub`/`mosquitto_sub` standing in for banoi2/banoi1: the subscriber
received the publish directly, and the server's log shows it observed the
same message (`Removal request req_test001: member 9d8a16dd1654… -> admin
admin-test-device for lock ozk-acebe639f8c4`), plus the ACK direction, plus
correct `WARN` logging for a non-JSON payload and for a payload missing
required fields.

**Lab caveat, worth knowing before trusting the "Unknown device → Dropped
(ACL)" test case:** the lab Mosquitto broker does not currently enforce
credentials at all — publishing with a fabricated username and a wrong
password still succeeded (checked live, exit 0, message delivered). This
isn't new to S8/S9 (the file header already notes broker credentials are
"minted + stored + acked for contract shape" but not enforced lab-side for
locks) but it means that specific test case can't actually be demonstrated as
passing in this environment right now — it needs real ACL rules on the
broker, which is infra, not `ozlockserv` code. Flagging so nobody discovers
this by having the "negative" test silently not test anything.

Recorded as an explicit production-readiness item in `ozlockserv/server.js`'s
file header (commit `9dbf422`) — configuring real ACLs on the production
broker (EMQX, not this lab's Mosquitto) is a **future task, deliberately
deferred** per operator instruction 2026-08-08, not something being acted on
in this pass. This note exists so it's a tracked requirement, not a
rediscovery.

10. Next Steps
Priority	Action	Owner
1	Implement S8-S9 (MQTT app‑to‑app topics)	Server Team
2	Implement A11 (date/time picker)	App Team
3	Implement A9 (banoi2 orchestration + retry)	App Team
4	Implement A10 (banoi1 auto‑grant)	App Team
5	Implement A8 (one‑action UX)	App Team
6	Integration test (orchestrated removal)	All
11. Related Documents
Document	Description
ozkey-13.md	Relay‑opaque migration spec (server sovereignty)
XF-70	UX change doc (one action, copy, orchestration)
ozkey-14.md	End‑to‑end test plan