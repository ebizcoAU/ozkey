/* oz_lock.h — the OZKIE v2 lock engine (ozkey-48), transport-agnostic.
 *
 * The transport hands in reassembled OZKIE messages with their frame type
 * (ozkey-48 §2.1: 0 = EDHOC, 1 = OSCORE/CoAP, 2 = error) on a link, and sends
 * back whatever the engine writes to `out`. A link is one BLE connection
 * (session context, destroyed on disconnect) or the bridge relay (contexts
 * persisted per bond, ozkey-48 §4.1).
 *
 * Credentials (both sides, ozkey-48 §3): CRED_x is the canonical 44-byte CCS
 *   a1 08 a1 01 a3 01 01 20 04 21 58 20 ‖ X25519 public key
 * The lock's ID_CRED_R is {4: h'00'}; a bonded app's ID_CRED_I is {4: slot};
 * an unbonded app sends {14: CCS} (kccs) in the canonical form above.
 *
 * Persistence through oz_port_kv_*: "lk" identity, "b0".."b15" bonds, "ep"
 * epoch, "nc" nonce cache, "cr" credentials, "r0".."r15" relay contexts,
 * "pl" keypad limit, "rl" RFID card limit, "es" the event sequence numbers
 * reserved so far.
 */
#ifndef OZ_LOCK_H
#define OZ_LOCK_H

#include "oz_edhoc.h"
#include "oz_oscore.h"
#include "oz_invite.h"
#include "oz_limit.h"
#include "oz_events.h"
#include "oz_power.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long a responder handshake waits for message_3 before it is dropped
 * (ozkey-55 §5b). A relay EDHOC lease on the server must outlast it. */
#define OZ_LOCK_EDHOC_TIMEOUT_MS 30000u
#define OZ_LOCK_MAX_BONDS   16
#define OZ_LOCK_MAX_BRIDGE_BONDS 2   /* `bridge`-role bonds; a 3rd is NO_SLOT (F1-BRIDGE-BOND-1) */
#define OZ_LOCK_MAX_CREDS   32
#define OZ_LOCK_NONCES      32
#define OZ_LOCK_LABEL       24
#define OZ_LOCK_CRED_SECRET 16
#define OZ_CCS_LEN          44

enum { OZ_FRAME_EDHOC = 0, OZ_FRAME_OSCORE = 1, OZ_FRAME_ERROR = 2 };

enum {
    OZ_ROLE_NONE = 0, OZ_ROLE_ADMIN = 1, OZ_ROLE_MEMBER = 2, OZ_ROLE_SERVICE = 3,
    OZ_ROLE_BRIDGE = 4, OZ_ROLE_ISSUER = 5,
};

/* Result codes: the v1 names (ozkey-28 §4) as small integers (ozkey-48 App. A). */
enum {
    OZ_R_OK = 0, OZ_R_UNLOCK_OK = 1, OZ_R_LOCK_OK = 2, OZ_R_BOND_OK = 3, OZ_R_MEMBER_OK = 4,
    OZ_R_REVOKE_OK = 5, OZ_R_CRED_OK = 6, OZ_R_TIME_OK = 7, OZ_R_PAIR_PENDING = 8,
    OZ_R_DENIED = 16, OZ_R_BAD_REQUEST = 17, OZ_R_UNSUPPORTED = 18, OZ_R_NO_SLOT = 19,
    OZ_R_CRED_NOT_FOUND = 20, OZ_R_EXPIRED = 21, OZ_R_STALE_EPOCH = 22, OZ_R_FORBIDDEN = 23,
    OZ_R_BOND_DENIED = 24, OZ_R_MEMBER_REPLAY = 25, OZ_R_MEMBER_FULL = 26,
    OZ_R_MEMBER_EXPIRED = 27, OZ_R_INVITE_BOUND_ELSEWHERE = 28, OZ_R_PAIR_FAILED = 29,
    OZ_R_PAIR_LOCKED_OUT = 30, OZ_R_NOT_FOUND = 31, OZ_R_REVOKE_DENIED = 32,
    OZ_R_TIME_REJECTED = 33, OZ_R_BATTERY_LOW = 34, OZ_R_PIN_TOO_SHORT = 35,
};

/* Matter DoorLock vocabulary (🟡 values to be checked against Matter 1.x, ozkey-48 §5). */
enum { OZ_LOCKSTATE_NOT_FULLY_LOCKED = 0, OZ_LOCKSTATE_LOCKED = 1, OZ_LOCKSTATE_UNLOCKED = 2 };
enum { OZ_CRED_PIN = 1, OZ_CRED_RFID = 2, OZ_CRED_FINGERPRINT = 3 };

typedef struct {
    uint8_t  used;
    uint8_t  role;
    uint8_t  pub[32];
    char     label[OZ_LOCK_LABEL + 1];
    int64_t  expires;      /* unix s, 0 = never */
    uint16_t max_days;     /* issuer: longest guest invite it may mint */
} oz_bond;

typedef struct {
    uint8_t  used;
    uint8_t  type;
    uint16_t id;           /* allocated by the lock */
    uint8_t  secret_len;
    uint8_t  secret[OZ_LOCK_CRED_SECRET];
    int64_t  from, to;     /* unix s, 0 = open */
    int32_t  uses;         /* −1 = unlimited */
} oz_cred;

typedef struct {
    uint8_t nonce[16];
    uint8_t tag[8];        /* first bytes of the redeemer's key; zero = cancelled */
} oz_nonce_entry;

struct oz_lock;

/* One `events` Observe registration (ozkey-48 §5.1, RFC 7641): the token and
 * OSCORE exchange of the GET that registered, so each notification binds to
 * it, and the reader's cursor into the event ring. */
typedef struct {
    uint8_t        active;
    uint8_t        tkl;
    uint8_t        token[8];
    oz_oscore_xchg x;
    uint32_t       cursor;         /* next event sequence number to send */
    uint32_t       obs;            /* Observe value of the last message sent */
} oz_observer;

typedef struct {
    void (*actuate)(void *app, int unlock);
    /* Ask the owner for the 6-digit pairing code on the keypad (on = 1), or stop. */
    void (*pair_prompt)(void *app, int on);
    void *app;
} oz_lock_hooks;

typedef struct {
    struct oz_lock *lock;      /* set by oz_lock_rx */
    int       relay;           /* 1 = bridge relay link */
    uint8_t   peer_ccs[OZ_CCS_LEN];
    oz_edhoc  edhoc;
    uint8_t   edhoc_active;
    uint64_t  edhoc_deadline_ms; /* a handshake waiting for message_3 is dropped after this */
    uint8_t   ctx_ready;       /* session context established (BLE) */
    oz_oscore_ctx osc;
    int       slot;            /* bond of this session, −1 = none */
    /* first pairing (ozkey-48 §3.4) */
    uint8_t   pairing;
    uint8_t   pair_pub[32];
    uint32_t  pair_code;
    uint64_t  pair_deadline_ms;
    uint8_t   pair_req_pending;
    oz_oscore_xchg pair_x;
    oz_coap_msg    pair_req;   /* header of the deferred request (token, mid) */
    /* Echo (ozkey-48 §4.2, RFC 9175) */
    uint8_t   echo[8];
    uint64_t  echo_ms;
    uint8_t   echo_valid;
    /* relay EDHOC: the bond it will persist for */
    int       relay_ctx_slot;
    /* `events` observed on this BLE session (ends with it) */
    oz_observer obs;
} oz_link;

typedef struct oz_lock {
    uint8_t        priv[32], pub[32];
    char           device_id[OZ_INVITE_MAX_DEVICE_ID + 1];
    uint8_t        ccs[OZ_CCS_LEN];
    oz_bond        bonds[OZ_LOCK_MAX_BONDS];
    uint32_t       epoch;
    oz_nonce_entry nonces[OZ_LOCK_NONCES];
    uint8_t        nonce_next;
    oz_cred        creds[OZ_LOCK_MAX_CREDS];
    uint16_t       next_cred_id;
    uint8_t        lock_state;
    uint8_t        last_move;      /* OZ_MOVE_* of the last finished move (oz_motor.h) */
    oz_power       power;          /* battery level and percent (P5-F), from the port's readings */
    int64_t        utc_at_set;     /* clock: utc = utc_at_set + (mono − mono_at_set)/1000 */
    uint64_t       mono_at_set;
    uint8_t        clock_set;
    uint8_t        pair_failures;
    uint64_t       pair_lockout_until_ms;
    oz_limit       pin_limit;      /* keypad wrong-PIN limit (P5-B), kv "pl" */
    oz_limit       card_limit;     /* RFID wrong-card limit (P5-E), kv "rl" */
    oz_events      events;         /* RAM ring; seq reserved in kv "es" */
    uint32_t       ev_reserved;    /* seqs below this are reserved in kv */
    uint32_t       rx_bad_type;  /* messages of an unknown OZKIE type, dropped (ozkey-48 §2.4) */
    oz_observer    relay_obs[OZ_LOCK_MAX_BONDS];   /* `events` over the relay, per bond (RAM) */
    uint16_t       mid;            /* CoAP message id of lock-sent notifications */
    /* persisted relay contexts, one per bond (admin / service) */
    oz_oscore_ctx  relay[OZ_LOCK_MAX_BONDS];
    uint8_t        relay_used[OZ_LOCK_MAX_BONDS];
    oz_lock_hooks  hooks;
    const char    *fw;
} oz_lock;

/* Load state from kv, or mint a new identity (factory-fresh, unclaimed). */
int  oz_lock_init(oz_lock *L, const char *device_id, const char *fw, const oz_lock_hooks *hooks);
int  oz_lock_claimed(const oz_lock *L);

/* F1-BRIDGE-BOND-1 (ozkey-48 rev 7 §2.3): which connection may own `relay`
 * and which one takes the app slot. The port asks; the lock decides.
 *  - authenticated bridge = the connection's `ozkie` session completed EDHOC
 *    as a bond that is still enrolled with role `bridge`;
 *  - with NO bridge bond enrolled the lock keeps the rev 6 rule (the first
 *    `relay` writer owns it), so nothing changes before enrolment;
 *  - once one is enrolled, only an authenticated bridge may own `relay`;
 *  - an authenticated bridge never takes the app slot (never makes the lock
 *    busy), whether or not it has written to `relay` yet. */
int  oz_lock_has_bridge_bond(const oz_lock *L);
int  oz_lock_link_is_bridge(const oz_lock *L, const oz_link *session);
int  oz_lock_relay_may_own(const oz_lock *L, const oz_link *session);
int  oz_lock_takes_app_slot(const oz_lock *L, const oz_link *session, int owns_relay);
/* App-slot squat timeout (F1-BRIDGE-BOND-1 (B), PM 2026-11-06): 1 when this
 * connection should be dropped. It must take the app slot, have been up for
 * OZ_LOCK_AUTH_GRACE_MS, and still have no completed `ozkie` EDHOC (any role).
 * A first pairing waiting for its code is never dropped here (it has its own
 * deadline). The keypad and card paths don't involve connections at all. */
#define OZ_LOCK_AUTH_GRACE_MS 30000u
int  oz_lock_link_squatting(const oz_lock *L, const oz_link *session, int owns_relay,
                            uint64_t connected_ms);
/* GATT `info` value (ozkey-48 §2): CBOR {1: device_id, 2: fw, 3: pub, 4: state, 5: profile}. */
int  oz_lock_info(const oz_lock *L, uint8_t *out, size_t cap, size_t *out_len);

void oz_link_open(oz_link *k, int relay);
void oz_link_close(oz_link *k);

/* One reassembled message in, at most one message out (out_len 0 = none). */
int  oz_lock_rx(oz_lock *L, oz_link *k, uint8_t type, const uint8_t *in, size_t len,
                uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len);
/* The owner typed a pairing code on the keypad: may produce the deferred
 * BOND_OK / PAIR_FAILED response for that link. */
int  oz_lock_pair_code(oz_lock *L, oz_link *k, uint32_t code,
                       uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len);
/* Call periodically: expires a pairing window or a half-done handshake that timed out. */
int  oz_lock_tick(oz_lock *L, oz_link *k,
                  uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len);
/* A PIN typed at the door (not during pairing): 1..OZ_LOCK_PIN_ENTRY_MAX
 * digits. Anti-peep: it opens if a live PIN appears anywhere in the entry, so
 * an owner can hide the PIN among other digits. Wrong entries count toward the
 * oz_limit lockout; while locked out nothing is checked.
 * Returns OZ_PIN_OPEN, OZ_PIN_WRONG or OZ_PIN_LOCKED_OUT. */
#define OZ_LOCK_PIN_ENTRY_MAX 16
#define OZ_LOCK_PIN_MIN       6      /* shortest PIN `cred` accepts (PM ruling 2026-10-29) */
enum { OZ_PIN_WRONG = 0, OZ_PIN_OPEN = 1, OZ_PIN_LOCKED_OUT = 2 };
int  oz_lock_keypad_pin(oz_lock *L, const char *digits);
/* Time left in a keypad lockout, 0 when there is none (for the panel). */
uint64_t oz_lock_keypad_lockout_ms(const oz_lock *L);

/* A card tapped at the reader (one tap: oz_card.h filters a card held on
 * it). id is what the port verified the card as: today the ISO 14443 UID or
 * PUPI, which a cloned card copies (OZ-5, open with the operator). It opens
 * on an exact match with a live RFID credential. Wrong cards count toward
 * their own oz_limit lockout, the keypad's policy; while locked out nothing is
 * checked. Returns OZ_CARD_OPEN, OZ_CARD_WRONG or OZ_CARD_LOCKED_OUT. */
enum { OZ_CARD_WRONG = 0, OZ_CARD_OPEN = 1, OZ_CARD_LOCKED_OUT = 2 };
int  oz_lock_card(oz_lock *L, const uint8_t *id, size_t len);
uint64_t oz_lock_card_lockout_ms(const oz_lock *L);

/* The port's motor finished a move (oz_motor.h OZ_MOVE_*): the lock state
 * follows what the move did. A jam leaves the bolt somewhere in between:
 * OZ_LOCKSTATE_NOT_FULLY_LOCKED. An unconfirmed move (no sensor) reports the
 * direction it drove. The actuate hook alone sets the commanded state. */
void oz_lock_actuated(oz_lock *L, int unlock, int result);

/* Battery readings from the port (oz_power.h): at rest, and the lowest one
 * while the motor drove. A level change raises a `battery` event. At
 * CRITICAL `lock/lock` is refused with BATTERY_LOW; unlocking never is. */
void oz_lock_power_rest(oz_lock *L, uint16_t mv);
void oz_lock_power_load(oz_lock *L, uint16_t min_mv);

/* Pending `events` notifications for whoever observes on link k: the BLE
 * session's own registration, or on the relay link each bond's. At most one
 * message per call; call again until out_len is 0 (oz_ble_tick does). */
int  oz_lock_events_poll(oz_lock *L, oz_link *k,
                         uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len);

int64_t oz_lock_utc(const oz_lock *L);   /* 0 when the clock was never set */

/* The canonical CCS for an X25519 key. */
void oz_ccs_for(const uint8_t pub[32], uint8_t ccs[OZ_CCS_LEN]);

#ifdef __cplusplus
}
#endif
#endif /* OZ_LOCK_H */
