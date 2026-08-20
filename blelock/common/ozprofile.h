#pragma once
/**
 * Device-profile dispatch for the firmware.
 *
 * WHAT THIS REPLACES
 * ------------------
 * `ozDpForwardable()` was:
 *
 *     return dp == 1 || (dp >= 21 && dp <= 24);
 *
 * Every number in that expression was INVENTED. Against the real Tuya lock
 * catalogue (`ozkey-27 §2.1`) DP 21 is `navigation_volume`, 23 is `auto_lock`,
 * 24 is `auto_lock_delay` and 22 is not allocated at all — so on real hardware
 * that allow-list forwards exactly the SETTINGS DPs and blocks every credential
 * operation. The precise inverse of its intent.
 *
 * The fix is not a corrected constant. Constants are how we got here. The map
 * lives in `profiles/`, is shared byte-for-byte with LockSim, and reaches the
 * firmware through `ozprofile_gen.h` (see `tools/gen_profile.py`).
 *
 * BEHAVIOUR IS UNCHANGED AT THE DEFAULT PROFILE — deliberately.
 * `ozkie-legacy-v0` marks DP 1 as `both` and 21-24 as `down`, with 2/3/5/8/60
 * as `up`. So `ozProfileDisposition(dp, OZ_DIR_DOWN)` permits exactly
 * {1, 21, 22, 23, 24} — bit-for-bit the old allow-list. This refactor changes
 * no behaviour on the bench; switching profile is what changes behaviour, and
 * that is a deliberate act.
 */

#include "ozprofile_gen.h"
#include <string.h>

/** What to do with a DP that just arrived (or that we are about to send). */
enum OzDisposition : uint8_t {
  OZ_DISP_HANDLE = 0,      // known, right direction, encodable
  OZ_DISP_UNSUPPORTED = 1, // known DP, payload layout not supplied — say so, send nothing
  OZ_DISP_REJECT = 2,      // not in this profile at all — never forward
};

/** The active profile. Index into OZ_PROFILES; defaults to OZ_PROFILE_DEFAULT_ID. */
static uint8_t g_ozProfileIdx = 0;

static const OzProfile *ozProfile() { return &OZ_PROFILES[g_ozProfileIdx]; }
static const char *ozProfileId() { return ozProfile()->id; }

/** Select by id. Returns false and changes nothing if the id is unknown. */
// Find the profile whose Tuya PID matches what the MCU reported to 0x01.
// Returns nullptr when we do not know this product — which is the SAFE
// outcome: the caller keeps its current profile rather than guessing.
static const OzProfile *ozProfileByTuyaPid(const char *pid) {
  if (!pid || !*pid) return nullptr;
  for (uint8_t i = 0; i < OZ_PROFILE_COUNT; i++)
    if (OZ_PROFILES[i].tuya_pid && strcmp(OZ_PROFILES[i].tuya_pid, pid) == 0)
      return &OZ_PROFILES[i];
  return nullptr;
}

static bool ozProfileSelect(const char *id) {
  for (uint8_t i = 0; i < OZ_PROFILE_COUNT; i++) {
    if (strcmp(OZ_PROFILES[i].id, id) == 0) {
      g_ozProfileIdx = i;
      return true;
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 🔴 THE DP MAP IS A BUILD-TIME DECISION (operator, 2026-08-20)
// ─────────────────────────────────────────────────────────────────────────────
//
// A lock is manufactured against ONE DL-MCU. Which DP list it speaks is a fact
// about the hardware in the door, decided before the firmware is flashed — it
// is not something to negotiate at runtime with whatever is on the far end of
// the UART.
//
// WHY THIS EXISTS. Until now firmware discovered its profile at runtime from
// the MCU's 0x01 product-info reply and ADOPTED it. LockSim can change its DP
// list from a dropdown; firmware cannot, and must not try to keep up. On the
// bench, 2026-08-20, firmware was observed switching ozsim-fullfeature ->
// tuya-ds013-t3 -> back again while the simulator's UI never moved, because a
// UART reconnect re-announced a stale identity. For part of that window the
// lock was interpreting every DP under a map for a product that was not
// attached, and nothing anywhere reported the disagreement (XF-118 §4).
//
// SECURITY, not just tidiness: runtime adoption means anything that can put
// bytes on the Tuya UART can tell the lock what its DP numbers MEAN. Choosing
// the map at build time removes that as an input entirely.
//
// Override per build; the default preserves the historical behaviour:
//   make flash BOARD=19 PROFILE=tuya-ds013-t3
#ifndef OZ_PROFILE_BUILD
#define OZ_PROFILE_BUILD OZ_PROFILE_DEFAULT_ID
#endif

// Set by the Makefile when PROFILE= was passed. An EXPLICIT flag rather than
// comparing OZ_PROFILE_BUILD to the default, because `PROFILE=ozkie-legacy-v0`
// is a deliberate choice and must not be mistaken for "nobody chose".
//
// It decides one thing: whether an unknown MCU PID may move us to the generic
// profile. A pinned build never moves — the whole point of pinning is that the
// far end of the UART does not get a vote (XF-118 §4).
#ifndef OZ_PROFILE_PINNED
#define OZ_PROFILE_PINNED 0
#endif

/** True once ozProfileBegin() has pinned the build profile. */
static bool g_ozProfilePinned = false;

/** Resolve the boot default. Call once from setup(). */
static void ozProfileBegin() {
  if (!ozProfileSelect(OZ_PROFILE_BUILD)) {
    // A build pinned to a profile that is not compiled in is a build error we
    // can only discover here. Say so loudly rather than silently running the
    // wrong map — that is the whole failure this pinning exists to end.
    Serial.printf("[PROFILE] 🔴 BUILD PINNED TO '%s' WHICH IS NOT COMPILED IN "
                  "— falling back to '%s'. Fix OZ_PROFILE_BUILD.\n",
                  OZ_PROFILE_BUILD, OZ_PROFILES[0].id);
    g_ozProfileIdx = 0;
  }
  g_ozProfilePinned = true;
  Serial.printf("[PROFILE] %s: '%s' (rev %u, %u DPs)\n",
                OZ_PROFILE_PINNED ? "pinned at build time" : "DEFAULT — NOT PINNED",
                ozProfileId(), (unsigned)ozProfile()->rev,
                (unsigned)ozProfile()->count);
  if (!OZ_PROFILE_PINNED) {
    // 🔴 Since ozkie-legacy-v0 was deleted (2026-08-20) every profile describes
    // a REAL product, and DP numbers differ between them — DP 76 is
    // `unlock_ble` on a Luona DS013-T3 and `fill_light` on Tuya's standard map.
    // So an unpinned build is not "the safe default", it is a guess about which
    // lock is in the door. Say so; do not let it pass silently.
    Serial.printf("[PROFILE] 🔴 this build was NOT pinned. '%s' is a GUESS — "
                  "DP numbers differ per product. Rebuild with PROFILE=<id>.\n",
                  ozProfileId());
  }
}

/** Table lookup. Profiles are small (10-34 entries) and sorted, but linear is
 *  plenty here and avoids a binary-search off-by-one for no measurable gain. */
static const OzDpEntry *ozDpFind(uint8_t dp) {
  const OzProfile *p = ozProfile();
  for (uint16_t i = 0; i < p->count; i++) {
    if (p->entries[i].dp == dp) return &p->entries[i];
  }
  return nullptr;
}

/** Human name for logs. Never null — unknown DPs still need to print. */
static const char *ozDpName(uint8_t dp) {
  const OzDpEntry *e = ozDpFind(dp);
  return e ? e->name : "unknown";
}

/**
 * The dispatch gate.
 *
 * `want` is the direction of travel for THIS frame: OZ_DIR_DOWN when we are
 * about to write to the MCU, OZ_DIR_UP when the MCU reported to us.
 */
static OzDisposition ozProfileDisposition(uint8_t dp, OzDpDir want) {
  const OzDpEntry *e = ozDpFind(dp);
  if (!e) return OZ_DISP_REJECT;

  // Direction is part of the contract, not decoration. A report-only DP
  // arriving as a downstream write is either a bug or an attacker, and on this
  // path frames are attacker-reachable (the broker is anon-open), so it is
  // refused rather than passed through.
  if (e->dir != OZ_DIR_BOTH && e->dir != want) return OZ_DISP_REJECT;

  // RESERVED is the honest state: DS013-T3 documents the TYPE of every
  // credential-write DP and the payload layout of NONE of them (ozkey-27 §2.5).
  // Refuse loudly instead of sending plausible-looking bytes at a door lock.
  if (e->status == OZ_DP_RESERVED || e->status == OZ_DP_UNKNOWN) return OZ_DISP_UNSUPPORTED;

  return OZ_DISP_HANDLE;
}

/** Convenience for the forward path — same predicate `ozDpForwardable()` had. */
static bool ozDpForwardable(uint8_t dp) {
  return ozProfileDisposition(dp, OZ_DIR_DOWN) == OZ_DISP_HANDLE;
}

// ─────────────────────────────────────────────────────────────────────────────
// OZKIE VERB RESOLVER (XF-120 / PM directive 2026-08-20)
// ─────────────────────────────────────────────────────────────────────────────
//
// Firmware is the ONLY layer that may know a Tuya DP number. The app sends a
// semantic verb, the server relays it opaquely, and this is where the verb
// becomes a DP — from data, not from a C branch.
//
// It replaces `if (isUnlock) { if (ozDpFind(76)) dp = 76; else dp = 1; }`, which
// compiled two DP numbers into logic and made "profile-driven" aspirational: a
// second supplier whose unlock DP was not 76 needed a firmware change. It is
// also how DP 1 — a number we invented — survived long enough to reach real
// hardware and silently fail there.
//
// Ordering IS policy. The generator sorts (verb, STATUS, field), so a bare verb
// with no field lands on the CONFIRMED candidate before any RESERVED one:
// `lock.unlock` resolves to DP 76, never to DP 10 whose payload layout the
// supplier has never supplied (ozkey-42 §2.2).
//
// Returns nullptr when this product has no such command — which is a real and
// useful answer, not a failure: it is how a lock says "I cannot do that", and
// the caller must surface it rather than substituting something that looks
// close. Substituting is exactly what produced the DP 1 fiction.

/** Resolve an OZKIE verb to a DP on the ACTIVE profile.
 *
 * @param verb  e.g. "lock.unlock", "cred.put"
 * @param field optional sub-type ("pin", "ble", …). nullptr matches the first
 *              candidate for `verb` in generator order (confirmed first).
 * @param dir   OZ_DIR_DOWN for a command we send, OZ_DIR_UP for a report.
 * @return the mapping, or nullptr if this product cannot do it.
 *
 * The caller MUST check `status`: a non-null result with OZ_DP_RESERVED means
 * "this lock has the DP but we were never told its payload layout" — known,
 * and still unusable.
 */
static const OzVerbMap *ozResolveVerb(const char *verb, const char *field,
                                      OzDpDir dir) {
  if (!verb || !*verb) return nullptr;
  const OzProfile *p = ozProfile();
  if (!p->verbs) return nullptr;
  for (uint16_t i = 0; i < p->verb_count; i++) {
    const OzVerbMap *m = &p->verbs[i];
    if (m->dir != dir) continue;
    if (strcmp(m->verb, verb) != 0) continue;
    // A caller that names no field takes the first (best-status) candidate.
    // A caller that names one must match it exactly — asking for `cred.put`
    // on "pin" must never be answered with the RFID DP.
    if (field && *field) {
      if (!m->field || strcmp(m->field, field) != 0) continue;
    }
    return m;
  }
  return nullptr;
}

/** True if the verb resolves AND is actually usable on this product. */
static bool ozVerbUsable(const OzVerbMap *m) {
  return m && m->status == OZ_DP_CONFIRMED;
}
