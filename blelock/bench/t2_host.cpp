/*
 * t2_host.cpp — ozkey-21 T2 tested with no board attached.
 *
 * Compiles blelock/common/oztime.h — the REAL firmware source, not a copy —
 * natively, and checks it two ways:
 *
 *   1. Against locksim/test's own on-the-wire fixture. LockSim is the other end
 *      of this UART, so if the two codecs disagree by a byte the whole service
 *      is theatre. Cross-checking firmware C++ against the TypeScript emulator
 *      is the point: they were written from the same spec on different days.
 *   2. Against the monotonic-forward security rule (ozkey-21 §3.4 rule 1),
 *      including the case that matters — a backwards time must not be able to
 *      resurrect an expired credential.
 *
 * Then, with --pty, it stands up a pseudo-terminal and serves the real reply
 * to a real bench/mcu_time_probe.py, which is the same probe that produced the
 * §2.3 "tx=0" finding on hardware. Same probe, same frames, no flash cycle.
 *
 *   c++ -std=c++17 -o /tmp/t2_host t2_host.cpp && /tmp/t2_host
 *   /tmp/t2_host --pty        # prints the slave path, then serves until Ctrl-C
 */
#include "../common/oztime.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static int failures = 0;

static std::string hex(const uint8_t *b, size_t n) {
  std::string s;
  char t[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(t, sizeof(t), "%02X", b[i]);
    if (i) s += ' ';
    s += t;
  }
  return s;
}

static void check(bool ok, const std::string &name, const std::string &detail = "") {
  printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
         detail.empty() ? "" : "  — ", detail.c_str());
  if (!ok) failures++;
}

// The exact frame locksim/test/mcuClock.test.ts asserts, for unix 1754800000.
static const char *LOCKSIM_1C = "55 AA 00 1C 00 08 01 19 08 0A 04 1A 28 07 9C";
static const uint32_t FIXTURE_UNIX = 1754800000UL;

static void runTests() {
  uint8_t out[20];

  // 1. Byte-for-byte against LockSim's fixture.
  size_t n = ozTuyaBuildTimeReply(out, /*local=*/true, /*haveTime=*/true, FIXTURE_UNIX);
  std::string got = hex(out, n);
  check(got == LOCKSIM_1C, "0x1C reply matches LockSim fixture byte-for-byte", got);

  // 2. Checksum is self-consistent (the same gate handleMcuFrame applies on rx).
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += out[i];
  check(sum == out[n - 1], "checksum valid");

  // 3. Weekday: 2025-08-10 was a Sunday -> Tuya 7, not the struct tm 0.
  // Payload starts at frame index 6, so weekday (payload byte 7) is index 13.
  check(out[13] == 7, "weekday is Tuya-encoded (Sunday = 7, not 0)");

  // 4. GMT reply is the 7-byte body with no weekday.
  n = ozTuyaBuildTimeReply(out, /*local=*/false, true, FIXTURE_UNIX);
  check(n == 14 && out[3] == 0x0C && out[5] == 7, "0x0C reply is 7-byte body", hex(out, n));

  // 5. "I do not know" is a REAL answer: full length, flag clear. This is the
  //    difference between a diagnosable module and the silence that hid §2.3.
  n = ozTuyaBuildTimeReply(out, /*local=*/true, /*haveTime=*/false, 0);
  check(n == 15 && out[6] == 0 && out[5] == 8,
        "unknown-time reply is full length with flag 0", hex(out, n));

  // 6. Clock starts unknown — never 1970.
  OzClock c;
  ozClockInit(c);
  check(!ozClockKnown(c) && ozClockNow(c, 1000) == 0, "clock starts UNKNOWN");

  // 7. First sync accepted.
  check(ozClockSet(c, FIXTURE_UNIX, 1000) && ozClockKnown(c), "first sync accepted");

  // 8. Free-runs between syncs.
  check(ozClockNow(c, 1000 + 5000) == FIXTURE_UNIX + 5, "free-runs on the local tick");

  // 9. THE SECURITY RULE. A backwards time is refused, so a replayed or spoofed
  //    beacon cannot un-expire a credential.
  bool back = ozClockSet(c, FIXTURE_UNIX - 86400, 6000);
  check(!back && ozClockNow(c, 6000) >= FIXTURE_UNIX, "backwards time REFUSED",
        "monotonic-forward only (CONTRACT.md:496)");
  check(c.refusedCount == 1, "refusal counted");

  // 10. Forward time accepted.
  check(ozClockSet(c, FIXTURE_UNIX + 3600, 7000), "forward time accepted");

  // 11. Garbage below the sanity floor refused — an uninitialised peer sending
  //     0 must not be able to set us to 1970 and expire everything at once.
  check(!ozClockSet(c, 12345, 8000), "pre-2020 time refused (sanity floor)");

  // 12. THE FORWARD-JUMP CAP. Without it, one spoofed "year 2099" datagram
  //     expires every credential AND permanently bricks the clock, because
  //     every real time afterwards is now backwards. Irreversible from a
  //     single packet.
  OzClock d;
  ozClockInit(d);
  check(ozClockSet(d, FIXTURE_UNIX, 1000), "first sync uncapped (no reference yet)");
  check(!ozClockSet(d, FIXTURE_UNIX + 3600UL * 24 * 365 * 80, 2000),
        "year-2099 jump REFUSED", "cap is what stops the one-packet brick");
  check(ozClockNow(d, 2000) < FIXTURE_UNIX + 60, "clock unharmed after the attempt");

  // 13. A year-long dormancy still resyncs in ONE step — the cap is absurd-
  //     proof, not tight, so a lock out of a drawer does not crawl forward.
  check(ozClockSet(d, FIXTURE_UNIX + 3600UL * 24 * 300, 3000),
        "300-day catch-up accepted in one step");

  // 14. Push frame shape: 0x34 [PUSH][timeType][7 GMT bytes].
  n = ozTuyaBuildTimePush(out, /*local=*/false, true, FIXTURE_UNIX);
  check(n == 16 && out[3] == 0x34 && out[6] == 0x02 && out[7] == 0x00,
        "0x34 time push shape", hex(out, n));
}

// ─────────────────────────────────────────────────────────────────────────────
// --pty: serve a real mcu_time_probe.py over a pseudo-terminal
// ─────────────────────────────────────────────────────────────────────────────
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static void servePty(bool haveTime) {
  int master, slave;
  char name[256];
  if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
    perror("openpty");
    exit(2);
  }
  printf("PTY ready. Point the probe at it:\n\n");
  printf("    python3 mcu_time_probe.py %s 8\n\n", name);
  printf("Serving with clock %s. Ctrl-C to stop.\n", haveTime ? "KNOWN" : "UNKNOWN");
  fflush(stdout);

  OzClock c;
  ozClockInit(c);
  if (haveTime) ozClockSet(c, (uint32_t)time(nullptr), 0);

  uint8_t buf[256];
  size_t bn = 0;
  for (;;) {
    uint8_t b;
    ssize_t r = read(master, &b, 1);
    if (r <= 0) continue;
    // Same header hunt as tuyaWirePump().
    if (bn == 0 && b != 0x55) continue;
    if (bn == 1 && b != 0xAA) { bn = (b == 0x55) ? 1 : 0; continue; }
    if (bn < sizeof(buf)) buf[bn++] = b;
    if (bn < 7) continue;
    size_t len = ((size_t)buf[4] << 8) | buf[5];
    if (bn < 6 + len + 1) continue;

    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < bn; i++) sum += buf[i];
    if (sum != buf[bn - 1]) { printf("RX bad checksum, dropped\n"); bn = 0; continue; }

    printf("RX  %s\n", hex(buf, bn).c_str());
    if (buf[3] == OZ_TUYA_GET_GMT_TIME || buf[3] == OZ_TUYA_GET_LOCAL_TIME) {
      bool local = buf[3] == OZ_TUYA_GET_LOCAL_TIME;
      uint8_t out[20];
      size_t n = ozTuyaBuildTimeReply(out, local, ozClockKnown(c), ozClockNow(c, 0));
      write(master, out, n);
      printf("TX  %s   <- %s time\n", hex(out, n).c_str(), local ? "LOCAL" : "GMT");
    }
    fflush(stdout);
    bn = 0;
  }
}

int main(int argc, char **argv) {
  bool pty = false, unknown = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--pty")) pty = true;
    if (!strcmp(argv[i], "--unknown")) unknown = true;
  }
  if (pty) { servePty(!unknown); return 0; }

  printf("ozkey-21 T2 — firmware codec vs LockSim, host build\n\n");
  runTests();
  printf("\n%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
