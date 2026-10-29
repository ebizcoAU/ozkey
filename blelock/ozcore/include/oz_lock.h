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
 * epoch, "nc" nonce cache, "cr" credentials, "r0".."r15" relay contexts.
 */
#ifndef OZ_LOCK_H
#define OZ_LOCK_H

#include "oz_edhoc.h"
#include "oz_oscore.h"
#include "oz_invite.h"
#include "oz_limit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZ_LOCK_MAX_BONDS   16
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
    OZ_R_TIME_REJECTED = 33,
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
    uint8_t        battery;
    int64_t        utc_at_set;     /* clock: utc = utc_at_set + (mono − mono_at_set)/1000 */
    uint64_t       mono_at_set;
    uint8_t        clock_set;
    uint8_t        pair_failures;
    uint64_t       pair_lockout_until_ms;
    oz_limit       pin_limit;      /* keypad wrong-PIN limit (P5-B), kv "pl" */
    /* persisted relay contexts, one per bond (admin / service) */
    oz_oscore_ctx  relay[OZ_LOCK_MAX_BONDS];
    uint8_t        relay_used[OZ_LOCK_MAX_BONDS];
    oz_lock_hooks  hooks;
    const char    *fw;
} oz_lock;

/* Load state from kv, or mint a new identity (factory-fresh, unclaimed). */
int  oz_lock_init(oz_lock *L, const char *device_id, const char *fw, const oz_lock_hooks *hooks);
int  oz_lock_claimed(const oz_lock *L);
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
/* Call periodically: expires a pairing window that timed out. */
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

int64_t oz_lock_utc(const oz_lock *L);   /* 0 when the clock was never set */

/* The canonical CCS for an X25519 key. */
void oz_ccs_for(const uint8_t pub[32], uint8_t ccs[OZ_CCS_LEN]);

#ifdef __cplusplus
}
#endif
#endif /* OZ_LOCK_H */
