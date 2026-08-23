#pragma once
/**
 * Tuya low-power UART frame codec — SIMLOCK N8 test-master.
 *
 * Mirrors `blelock/common/ozdoorlock_core.h`'s `ozTuyaFrameOk()` /
 * `ozBuildDpFrame()` byte-for-byte (same algorithm, same field layout) rather
 * than #including that file directly — it requires a per-board macro set
 * (LCD pins, touch controller, etc.) this bare test-master board doesn't
 * have. Kept here as its own small, standalone header so a mismatch between
 * this and the doorlock/bridge32 codec would show up as a diff, not a
 * silent divergence. See docs/DPSuppliers/protocol_vr4iiuqtyh0q4nix_20260811.pdf
 * and ozkit/docs/simlock-dp-reconciliation.md for the protocol this
 * implements.
 *
 * Frame: 55 AA <ver:1> <cmd:1> <len:2 BE> <data:N> <checksum:1 = sum%256>
 * DP unit (inside `data` for a DP-report/DP-issue frame):
 *   <dp:1> <type:1> <vlen:2 BE> <value:vlen>
 *
 * 🔴 Command-byte variant is UNRESOLVED (ozkey-27 §2.2 Q1 — general vs
 * low-power). This header hardcodes the GENERAL variant (0x06 DP-issue
 * down, 0x07 DP-report up), matching what `ozdoorlock_core.h` already
 * assumes for the same reason: two of three signals point at general, and
 * it is a one-question fix if wrong. Do not hand-roll a second guess here —
 * change both places together if the supplier answers "low-power".
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define OZ_TUYA_CMD_DP_ISSUE_DOWN 0x06 // module -> MCU (us -> DL-MCU under test)
#define OZ_TUYA_CMD_DP_REPORT_UP  0x07 // MCU -> module (DL-MCU under test -> us)

// DP value types, per the supplier doc's "类型说明" / ozprofile catalogue.
enum OzTuyaDpType : uint8_t {
  OZ_TUYA_RAW    = 0x00,
  OZ_TUYA_BOOL   = 0x01,
  OZ_TUYA_VALUE  = 0x02, // 4-byte big-endian int
  OZ_TUYA_STRING = 0x03,
  OZ_TUYA_ENUM   = 0x04, // 1 byte, 0-255
  OZ_TUYA_BITMAP = 0x05,
};

// Structural validation of any Tuya frame (either direction), checksum
// included — same predicate as ozdoorlock_core.h's ozTuyaFrameOk(), just not
// pinned to cmd==0x06 so it can check an up-report too.
static bool ozkitTuyaFrameOk(const uint8_t *f, size_t n) {
  if (n < 7 || f[0] != 0x55 || f[1] != 0xAA) return false;
  const size_t dlen = ((size_t)f[4] << 8) | f[5];
  if (6 + dlen + 1 != n) return false;
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += f[i];
  return sum == f[n - 1];
}

// Build a single-DP command frame in place: 55 AA 00 <cmd> <len:2> <dp>
// <type> <vlen:2> <value> <ck>. Returns the frame length, or 0 if `out`
// isn't big enough (caller-owned buffer, no allocation here).
static size_t ozkitBuildDpFrame(uint8_t cmd, uint8_t dp, uint8_t type,
                                 const uint8_t *val, size_t vlen,
                                 uint8_t *out, size_t outCap) {
  const size_t dlen = 4 + vlen;
  const size_t n = 6 + dlen + 1;
  if (n > outCap) return 0;
  out[0] = 0x55; out[1] = 0xAA; out[2] = 0x00; out[3] = cmd;
  out[4] = (uint8_t)(dlen >> 8); out[5] = (uint8_t)(dlen & 0xFF);
  out[6] = dp; out[7] = type;
  out[8] = (uint8_t)(vlen >> 8); out[9] = (uint8_t)(vlen & 0xFF);
  if (vlen) memcpy(out + 10, val, vlen);
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += out[i];
  out[n - 1] = sum;
  return n;
}

// Parse a single-DP report frame (cmd == OZ_TUYA_CMD_DP_REPORT_UP). `val`
// points into `f` — valid only as long as `f` is, no copy made.
static bool ozkitParseDpReport(const uint8_t *f, size_t n, uint8_t *dp,
                                uint8_t *type, uint16_t *vlen,
                                const uint8_t **val) {
  if (!ozkitTuyaFrameOk(f, n)) return false;
  if (f[3] != OZ_TUYA_CMD_DP_REPORT_UP) return false;
  if (n < 11) return false;
  const size_t vl = ((size_t)f[8] << 8) | f[9];
  if (10 + vl + 1 != n) return false;
  *dp = f[6]; *type = f[7]; *vlen = (uint16_t)vl; *val = f + 10;
  return true;
}

// Scan a receive buffer for the next valid frame, resyncing past junk bytes
// the way locksim/lib/tuya.ts's extractFrames() does. Returns the byte
// offset of a found frame (with *frameLen set to its length), or `n` with
// *frameLen == 0 if nothing complete is in the buffer yet (caller should
// keep reading — a header may be sitting at the tail waiting on more bytes).
static size_t ozkitScanForFrame(const uint8_t *buf, size_t n, size_t *frameLen) {
  for (size_t i = 0; i + 6 <= n; i++) {
    if (buf[i] != 0x55 || buf[i + 1] != 0xAA) continue;
    const size_t dlen = ((size_t)buf[i + 4] << 8) | buf[i + 5];
    const size_t total = 6 + dlen + 1;
    if (i + total > n) { *frameLen = 0; return i; } // header found, wait for more
    uint8_t sum = 0;
    for (size_t k = i; k + 1 < i + total; k++) sum += buf[k];
    if (sum == buf[i + total - 1]) { *frameLen = total; return i; }
    // checksum mismatch at this offset — false positive on 0x55 0xAA, keep scanning
  }
  *frameLen = 0;
  return n;
}

// Self-test, run once at boot — same shape as ozdoorlock_core.h's
// ozM4SelfTest(), scoped to just the frame codec since that's all this file
// owns. Cheap: no I/O, runs in microseconds.
static bool ozkitTuyaFrameSelfTest() {
  uint8_t f[32];
  const uint8_t val = 0x01; // e.g. autolock=on, DP 23, bool
  size_t n = ozkitBuildDpFrame(OZ_TUYA_CMD_DP_ISSUE_DOWN, 23, OZ_TUYA_BOOL, &val, 1, f, sizeof(f));
  // 55 AA 00 <cmd> <dlen:2> <dp> <type> <vlen:2> <value:1> <ck> = 12 bytes.
  bool pass = (n == 12) && ozkitTuyaFrameOk(f, n) &&
              f[6] == 23 && f[7] == OZ_TUYA_BOOL && f[10] == 0x01;

  // A flipped checksum must fail.
  f[n - 1] ^= 0xFF;
  pass &= !ozkitTuyaFrameOk(f, n);
  f[n - 1] ^= 0xFF;

  // Round-trip as if it were an up-report instead (cmd 0x07), and parse it.
  uint8_t rep[32];
  size_t rn = ozkitBuildDpFrame(OZ_TUYA_CMD_DP_REPORT_UP, 47, OZ_TUYA_BOOL, &val, 1, rep, sizeof(rep));
  uint8_t dp, type; uint16_t vlen; const uint8_t *v;
  pass &= ozkitParseDpReport(rep, rn, &dp, &type, &vlen, &v) &&
          dp == 47 && type == OZ_TUYA_BOOL && vlen == 1 && v[0] == 0x01;

  // Scanner must find a frame preceded by junk bytes.
  uint8_t buf[40] = {0xFF, 0x00, 0x55}; // 0x55 alone is not a header
  memcpy(buf + 3, rep, rn);
  size_t flen = 0;
  size_t off = ozkitScanForFrame(buf, 3 + rn, &flen);
  pass &= (off == 3) && (flen == rn);

  return pass;
}
