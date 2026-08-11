/*
 * oztime.h — ozkey-21 T1/T2: the module's clock, and the time service it owes
 *            the lock MCU.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Tuya's architecture makes the MODULE the time source and the MCU its client:
 *
 *   "the module comes with a software real-time clock (RTC) that provides time
 *    for the MCU, and therefore, even when the module is offline, the MCU can
 *    also get the time."
 *
 * We replaced the Tuya module with our own ESP32 and never implemented that
 * service. Confirmed on the bench 2026-08-10 (ozkey-21 §2.3): DoorA receives
 * 0x1C and never answers, tx=0. The consequence is not cosmetic — DP 21/23
 * temporary PIN and RFID windows are checked by the MCU against a clock we
 * never set, so every time-limited credential we have ever issued has been
 * effectively permanent. This is a shipped security defect and this file is
 * its fix.
 *
 * DELIBERATELY FREE OF <Arduino.h>
 * --------------------------------
 * Everything here is plain C++ over <stdint.h> and <time.h>, and the caller
 * passes its own millisecond tick in rather than this file calling millis().
 * That is not fastidiousness: it lets bench/t2_host.cpp compile THIS EXACT
 * SOURCE natively and answer a real mcu_time_probe.py over a PTY, so T2 can be
 * tested byte-for-byte with no board attached and no flash cycle. A codec that
 * can only be exercised on hardware is a codec that gets tested once.
 *
 * WIRE FORMAT (module -> MCU reply body)
 *   byte 0   success flag — 0 means "I was asked and I do not know"
 *   byte 1   year - 2000        byte 4   hour 0-23
 *   byte 2   month 1-12         byte 5   minute 0-59
 *   byte 3   day 1-31           byte 6   second 0-59
 *   byte 7   weekday 1-7, 1 = Monday .. 7 = Sunday   (0x1C only)
 *
 * Byte-identical to locksim/lib/tuya.ts buildTimeReply(), which is the same
 * codec on the other side of the wire and is covered by locksim/test.
 *
 * ⚠ TIMEZONE — OPEN QUESTION, NOT AN OVERSIGHT.
 * 0x1C is nominally "local time" and we serve UTC in it, because the lock has
 * no timezone and nothing ever tells it one. That is safe ONLY IF the DP 21/23
 * windows the app writes are also UTC. If the app is sending local wall-clock
 * windows, every temporary credential is wrong by the UTC offset — 7 hours in
 * Vietnam. Raised with ftpos and the manufacturer; until it is answered, a
 * non-zero tzOffsetMin is available below but defaults to 0.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h> // snprintf — ozFormatStamp()
#include <time.h>

// Tuya command bytes for the time service.
static const uint8_t OZ_TUYA_GET_GMT_TIME   = 0x0C;
static const uint8_t OZ_TUYA_GET_LOCAL_TIME = 0x1C;
static const uint8_t OZ_TUYA_TIME_NOTIFY    = 0x34;

// 0x34 sub-commands. 0x34 is MULTIPLEXED — see handleMcuFrame()'s note; the
// factory reset (0x0A) shares this command byte with the time push.
static const uint8_t OZ_TUYA_SUB_SUBSCRIBE = 0x01;
static const uint8_t OZ_TUYA_SUB_PUSH      = 0x02;

// Sanity floor: any UTC below this is not a real clock reading. Same constant
// isoNow() already uses, so the two agree on what "we have time" means.
static const uint32_t OZ_TIME_FLOOR = 1600000000UL;

/*
 * Forward-jump ceiling — 400 days.
 *
 * WHAT THIS STOPS. Monotonic-forward alone leaves an IRREVERSIBLE failure: one
 * datagram carrying "year 2099" expires every temporary credential at once, and
 * because every legitimate time afterwards is now "backwards", the lock refuses
 * all of them forever. There is no beacon that walks it back — the recovery is
 * a factory reset. That is a brick from a single packet, so the cap matters
 * more than it looks.
 *
 * WHY 400 DAYS rather than something tight. The cap only has to be absurd-
 * proof, not tight: 2099 is ~27,000 days away, so 400 blocks it comfortably
 * while letting a lock that sat in a drawer for a year resync in ONE step. A
 * tight cap (say 32 days) would force a long-dormant lock to crawl forward one
 * beacon at a time — days of catch-up at a daily cadence — for no security gain.
 *
 * Does NOT apply to the first sync: a clock that has never been set has no
 * reference to be "far" from, and refusing the first time would be refusing the
 * whole feature.
 */
static const uint32_t OZ_TIME_MAX_STEP = 400UL * 24 * 60 * 60;

// ─────────────────────────────────────────────────────────────────────────────
// T1 — the module's own clock
// ─────────────────────────────────────────────────────────────────────────────
struct OzClock {
  uint32_t baseUtc;     // UTC at the moment of the last accepted sync; 0 = never
  uint32_t baseMillis;  // caller's tick at that moment
  uint32_t syncCount;   // accepted syncs — proof the service is being fed
  uint32_t refusedCount;// rejected syncs — proof the monotonic rule is biting
};

inline void ozClockInit(OzClock &c) {
  c.baseUtc = 0; c.baseMillis = 0; c.syncCount = 0; c.refusedCount = 0;
}

inline bool ozClockKnown(const OzClock &c) { return c.baseUtc >= OZ_TIME_FLOOR; }

/*
 * Current UTC, free-running on the local oscillator between syncs. Returns 0
 * when the clock has never been set — and 0 must be read as "unknown", never
 * as "1970". Rule 2 of ozkey-21 §3.4: unknown is a third state, not a zero.
 *
 * The free-run is what the operator's external 32.768 kHz crystal pays for:
 * ~±20 ppm (~1.7 s/day) instead of the internal RC's percent-level drift, which
 * is what makes a daily beacon sufficient instead of constant resync.
 */
inline uint32_t ozClockNow(const OzClock &c, uint32_t nowMillis) {
  if (!ozClockKnown(c)) return 0;
  return c.baseUtc + (uint32_t)((nowMillis - c.baseMillis) / 1000UL);
}

/*
 * Accept a time. MONOTONIC-FORWARD ONLY (CONTRACT.md:496, ozkey-21 §3.4 rule 1).
 *
 * Refusing a backwards time is a security rule, not tidiness: accepting one
 * un-expires every credential that has already lapsed, which turns the time
 * service into an access-extension primitive for anyone who can spoof it.
 *
 * Returns true if accepted. Millis rollover (~49.7 days) is handled by the
 * unsigned subtraction in ozClockNow(); a sync at any point re-bases both.
 */
inline bool ozClockSet(OzClock &c, uint32_t utc, uint32_t nowMillis) {
  if (utc < OZ_TIME_FLOOR) { c.refusedCount++; return false; }
  if (ozClockKnown(c)) {
    const uint32_t cur = ozClockNow(c, nowMillis);
    if (utc < cur) { c.refusedCount++; return false; }          // backwards
    if (utc - cur > OZ_TIME_MAX_STEP) { c.refusedCount++; return false; } // absurd
  }
  c.baseUtc = utc;
  c.baseMillis = nowMillis;
  c.syncCount++;
  return true;
}

/*
 * Human stamp for the LCDs — "Tu 11/08/2026 07:28:11AM" (operator's format,
 * 2026-08-11). DD/MM/YYYY, 12-hour with AM/PM, two-letter weekday.
 *
 * Shared by bridge32 and both doorlock panels so the two screens can never
 * drift into different date formats — which matters more than it sounds when
 * you are comparing a bridge log against a lock log at 3am.
 *
 * `utc == 0` means the clock has never been set, and it renders as
 * "-- --/--/---- --:--:--" rather than a plausible-looking 1970 date. A screen
 * that shows a confident wrong time is worse than one that shows none: the
 * whole ozkey-21 defect survived this long because nothing ever said "I do not
 * know what time it is".
 *
 * `out` needs 26 bytes. Returns the length written.
 */
inline size_t ozFormatStamp(char *out, size_t cap, uint32_t utc, int16_t tzOffsetMin = 0) {
  static const char *kWd[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  if (utc < OZ_TIME_FLOOR) {
    const char *unk = "-- --/--/---- --:--:--";
    size_t n = 0;
    while (unk[n] && n + 1 < cap) { out[n] = unk[n]; n++; }
    out[n] = 0;
    return n;
  }
  time_t t = (time_t)utc + (time_t)tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  int h12 = tmv.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  return (size_t)snprintf(out, cap, "%s %02d/%02d/%04d %02d:%02d:%02d%s",
                          kWd[tmv.tm_wday % 7], tmv.tm_mday, tmv.tm_mon + 1,
                          tmv.tm_year + 1900, h12, tmv.tm_min, tmv.tm_sec,
                          tmv.tm_hour < 12 ? "AM" : "PM");
}

/*
 * Narrow stamp — "11/08/26 09:07PM" (operator, 2026-08-11).
 *
 * Two-digit year, no seconds, 12-hour with AM/PM. 16 characters.
 *
 * The width is the whole point. The bridge panel is 240 px and text size 2 is
 * 12 px per character, so 20 characters is the hard ceiling — the earlier
 * 19-char form fitted only by a hair and still crowded the edge. 16 chars is
 * 192 px, which leaves real margin. Seconds were the first thing to go: on a
 * screen showing a once-a-day-synced clock they were false precision, and they
 * forced a redraw every second to display a digit nobody reads.
 *
 * `out` needs 17 bytes.
 */
inline size_t ozFormatStampNarrow(char *out, size_t cap, uint32_t utc,
                                  int16_t tzOffsetMin = 0) {
  if (utc < OZ_TIME_FLOOR) {
    const char *unk = "--/--/-- --:--";
    size_t n = 0;
    while (unk[n] && n + 1 < cap) { out[n] = unk[n]; n++; }
    out[n] = 0;
    return n;
  }
  time_t t = (time_t)utc + (time_t)tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  int h12 = tmv.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  return (size_t)snprintf(out, cap, "%02d/%02d/%02d %02d:%02d%s",
                          tmv.tm_mday, tmv.tm_mon + 1,
                          (tmv.tm_year + 1900) % 100, h12, tmv.tm_min,
                          tmv.tm_hour < 12 ? "AM" : "PM");
}

// ─────────────────────────────────────────────────────────────────────────────
// T2 — the codec
// ─────────────────────────────────────────────────────────────────────────────

/* Frame a Tuya message in place. Returns bytes written. */
inline size_t ozTuyaFrame(uint8_t *out, uint8_t cmd, const uint8_t *payload, size_t len) {
  out[0] = 0x55; out[1] = 0xAA; out[2] = 0x00; out[3] = cmd;
  out[4] = (uint8_t)((len >> 8) & 0xFF);
  out[5] = (uint8_t)(len & 0xFF);
  for (size_t i = 0; i < len; i++) out[6 + i] = payload[i];
  uint8_t sum = 0;
  for (size_t i = 0; i < 6 + len; i++) sum += out[i];
  out[6 + len] = sum;
  return 6 + len + 1;
}

/*
 * Build the reply body. `haveTime == false` produces the full-length body with
 * a zero flag byte — Tuya sends the same length either way, and "I do not know
 * the time" is a REAL answer the MCU must be able to tell apart from silence.
 * That distinction is the entire diagnostic value of this command: silence
 * means the service is absent, flag 0 means it is present and unfed.
 */
inline size_t ozTuyaTimeBody(uint8_t *body, bool local, bool haveTime, uint32_t utc,
                             int16_t tzOffsetMin = 0) {
  const size_t len = local ? 8 : 7;
  for (size_t i = 0; i < len; i++) body[i] = 0;
  if (!haveTime) return len;

  time_t t = (time_t)utc + (time_t)tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&t, &tmv);          // gmtime because the offset is already applied
  body[0] = 1;
  body[1] = (uint8_t)((tmv.tm_year + 1900 - 2000) & 0xFF);
  body[2] = (uint8_t)(tmv.tm_mon + 1);
  body[3] = (uint8_t)tmv.tm_mday;
  body[4] = (uint8_t)tmv.tm_hour;
  body[5] = (uint8_t)tmv.tm_min;
  body[6] = (uint8_t)tmv.tm_sec;
  // struct tm: 0 = Sunday. Tuya: 1 = Monday .. 7 = Sunday.
  if (local) body[7] = (uint8_t)(tmv.tm_wday == 0 ? 7 : tmv.tm_wday);
  return len;
}

/* Full 0x0C / 0x1C reply frame. `out` needs 16 bytes. */
inline size_t ozTuyaBuildTimeReply(uint8_t *out, bool local, bool haveTime, uint32_t utc,
                                   int16_t tzOffsetMin = 0) {
  uint8_t body[8];
  size_t n = ozTuyaTimeBody(body, local, haveTime, utc, tzOffsetMin);
  return ozTuyaFrame(out, local ? OZ_TUYA_GET_LOCAL_TIME : OZ_TUYA_GET_GMT_TIME, body, n);
}

/*
 * Unsolicited push: 0x34 [0x02 PUSH][timeType][7 GMT bytes].
 *
 * timeType 0 = GMT, 1 = local. The body is the 7-byte GMT form in both cases —
 * matching locksim's buildTimePush(), which passes local=false to the body
 * builder regardless of the flag.
 */
inline size_t ozTuyaBuildTimePush(uint8_t *out, bool local, bool haveTime, uint32_t utc,
                                  int16_t tzOffsetMin = 0) {
  uint8_t payload[9];
  payload[0] = OZ_TUYA_SUB_PUSH;
  payload[1] = local ? 0x01 : 0x00;
  size_t n = ozTuyaTimeBody(payload + 2, /*local=*/false, haveTime, utc, tzOffsetMin);
  return ozTuyaFrame(out, OZ_TUYA_TIME_NOTIFY, payload, 2 + n);
}
