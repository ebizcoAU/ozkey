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
static bool ozProfileSelect(const char *id) {
  for (uint8_t i = 0; i < OZ_PROFILE_COUNT; i++) {
    if (strcmp(OZ_PROFILES[i].id, id) == 0) {
      g_ozProfileIdx = i;
      return true;
    }
  }
  return false;
}

/** Resolve the boot default. Call once from setup(). */
static void ozProfileBegin() {
  if (!ozProfileSelect(OZ_PROFILE_DEFAULT_ID)) g_ozProfileIdx = 0;
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
