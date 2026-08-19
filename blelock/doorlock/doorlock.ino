/*
 * doorlock — Waveshare ESP32-C6 Touch-LCD 1.47" (pin map: blelock/HARDWARE.md)
 *
 * 2026-08-07: refactored to a thin per-board file over
 * blelock/common/ozdoorlock_core.h, the shared crypto/BLE/bond/MCU-
 * forwarding/Wi-Fi/Thread/dispatch logic — previously this file WAS that
 * logic (doorlock19.ino was forked from it 2026-08-06); now both boards
 * include one shared implementation. See ozdoorlock_core.h's own header
 * for exactly what a per-board file must define before including it.
 *
 * This board keeps its original touch driver (real TOUCH_RST/TOUCH_INT
 * pins, CST816 @0x63) and its original linear coordinate transform —
 * unchanged from before the refactor. No behavior change intended; the
 * historical per-firmware-version changelog now lives in
 * ozdoorlock_core.h since it documents the shared logic's history.
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Hardware pins (HARDWARE.md, operator-verified) ──────────────────────────
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_RST 22
#define LCD_BL 23
#define I2C_SDA 18
#define I2C_SCL 19
#define TOUCH_RST 20
#define TOUCH_INT 21
#define TOUCH_ADDR 0x63
#define HAS_TOUCH_INT // real INT/RST pair on this board — usable as a light-sleep wake source

// Tuya MCU bus → LockSim Mode B (wire-verified 2026-07-19)
#define TUYA_TX_PIN 16  // -> CP2102 RXD
#define TUYA_RX_PIN 17  // <- CP2102 TXD

// §0.2 wake lines (Tuya keep-alive contract): active low, answer-before-
// transmit, 10 s serial-idle release. GPIO1-4 reserved (SPI/SD). SRDY on an
// LP pin (deep-sleep-wake capable). GPIO8 is a C6 strapping pin — MRDY
// idles HIGH so boot is unaffected; remap on real lock hw if its MCU pulls
// this line low at reset.
#define SRDY_PIN 7  // MCU → module: "module, wake" / held low = MCU awake
#define MRDY_PIN 8  // module → MCU: "MCU, wake" / held low = module awake

// Mechanical factory-reset button (hold 5s). 2026-08-07: made explicit here —
// this used to silently inherit the ESP32-C6 toolchain's generic BOOT_PIN
// (GPIO9, esp32-hal.h) with no board-specific verification at all, same gap
// as the 1.9" board. Kept at 9 (unchanged behavior) but NOT independently
// confirmed against this board's actual schematic/HARDWARE.md — flag if this
// turns out to be wrong on real hardware, same as SRDY/MRDY's own caveat.
#define USER_BUTTON 9

// ── BGR-corrected palette (panel is BGR) ────────────────────────────────────
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0x001F
#define C_BLUE 0xF800
#define C_GREEN 0x07E0
#define C_AMBER 0x051F
#define C_GREY 0x8410
#define C_DIM 0x39E7

// ── Display geometry + rotation + backlight polarity ───────────────────────
#define PANEL_W 320
#define PANEL_H 172
#define LCD_ROTATION 5
#define LCD_BL_ON  HIGH
#define LCD_BL_OFF LOW

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false /*BGR*/, 172, 320, 34, 0, 34, 0);

// ── Firmware version ────────────────────────────────────────────────────────
// BUMP THE MINOR ON EVERY FLASHED CHANGE (operator directive 2026-08-02).
// Full changelog 1.0-1.12 now lives in ozdoorlock_core.h (shared history).
//   1.13 (2026-08-07): refactored onto the shared ozdoorlock_core.h — no
//        functional change, pure restructuring (see file header).
//   1.14 (2026-08-07): drawn keypad (dlock19-style) replaces the invisible
//        hit-test grid; hex-command readout below the LOCKED/UNLOCKED badge
//        (Serial1/LockSim wire write stays real, hex is additive); Net/IP/
//        Owner text dropped from the LCD to fit the keypad (still in serial
//        + app); USER_BUTTON made an explicit, board-owned #define (was
//        silently inheriting the toolchain's generic BOOT_PIN).
//   1.15 (2026-08-07): UNIFIED VERSION SCHEME — doorlock.ino and
//        doorlock19.ino now share one FW_VERSION/FW_DISPLAY_VERSION string
//        instead of two diverging counters ("doorlock-1.x" vs
//        "doorlock19-1.x"), since they've shared byte-identical core logic
//        since the 2026-08-07 refactor and there's no real reason left to
//        version them separately (operator directive). Also the layout
//        overlap fix and thin color-bar door-status indicator — see
//        ozdoorlock_core.h's own changelog for the shared-logic details.
//   1.16 (2026-08-07): fixed a real bug shipped in 1.15 — drawHexReadout()
//        was still drawing at y=STATUS_H (same y as the color bar), so its
//        black background painted over the bar every redraw (operator-
//        caught: "big red line... overlay and disappear"). Also, after
//        seeing 1.15 on real hardware: status text merged back to one line,
//        color bar shrunk to 30px with the BLE countdown moved onto that
//        row instead of its own line, keypad reduced 4x3->4x2 (no real PIN
//        backend, taps only exercise touch zones), hex readout now 2 lines
//        with the freed space, filled blue keypad buttons + filled amber
//        BLE-open badge (operator: "employ blue amber etc...to improve
//        your UI").
//   1.17 (2026-08-07): third layout pass after seeing 1.16 on real hardware
//        — status text + color bar + BLE countdown merged onto ONE row
//        (was two: text alone, then bar+BLE), bar shrunk 30px->60px per
//        operator spec but now paired with compact "BLE Ns" text (not "BLE
//        open Ns", which didn't fit this row's trailing space) so it still
//        fits; hex readout is now one bigger line (textSize 2, was two
//        lines of smaller text) trading max visible bytes for readability.
//        Also fixed the 3s periodic refresh causing visible flicker
//        (operator: "REMOVE THE 3S screen update..it causes bad
//        flicker..can u update a word rather than a whole screen") — split
//        drawStatusLine() out of drawOperational() so the periodic tick
//        repaints only that one row directly, never a full fillScreen().
//   1.18 (2026-08-07): fourth pass on the hex readout after real-hardware
//        feedback — textSize back to 1 (size 2 didn't fit horizontally,
//        "1 step smaller"), and the line now leads with the lock's own name
//        (first 6 chars of cfgName, falling back to deviceId, same
//        convention as drawJoining()) before the hex bytes, using the ~100px
//        of spare width the smaller text left on that row.
//   1.19 (2026-08-07): lock name on the hex line widened 6->10 chars, and
//        C_GREY swapped for C_WHITE (operator: "as I cant hardly see it" —
//        grey was too low-contrast on this panel).
//   1.20 (2026-08-07): final layout pass — line 1 now carries everything
//        (version/transport/role-or-IP/lock name/BLE countdown far right);
//        the LOCKED/UNLOCKED color bar moved off line 1 entirely, now a
//        40px bar on line 2 next to the hex readout (name removed from
//        line 2 to avoid showing it twice).
//   1.21 (2026-08-07): BLE-open countdown gets a filled amber badge + white
//        text again (operator: "BLE 20s in amber background white text"),
//        reapplied after the status line was rebuilt.
//   1.22 (2026-08-08): ozkey-13 F1 — `ozControlTry()`'s open/verify core
//        factored into `ozControlOpen()` (bond lookup, key derive, envelope
//        open) + `ozControlVerifyAndDispatch()` (challenge/counter/frame
//        checks, dispatch), so the MQTT sealed-envelope path (F2, not yet
//        wired) can share it with BLE `control` instead of duplicating the
//        crypto. Zero behavior change on BLE — same gates, same order, same
//        UNLOCK_DENIED points. `ozdoorlock_core.h` only; no UI change.
//   1.23 (2026-08-08): ozkey-13 F2-F5 — MQTT command topic now accepts
//        `envelope_hex` (sealed, F2), opened via the F1 core with no live
//        challenge (counter-only freshness, §5). `ozControlDispatch()`
//        extended to forward DP 21-24 (temp PIN/RFID add/delete) alongside
//        DP 1, reusing `ozDpForwardable()`'s existing allow-list instead of
//        a second one (F3); role-gated to bond #0, same admin-only bar as
//        101/102/103 — a member's DP 21-24 attempt gets UNLOCK_DENIED (F4).
//        Legacy `payload_hex` (unauthenticated pure-forward) kept working
//        unchanged for pre-1.23 servers during rollout (F5) — `envelope_hex`
//        wins if a server sends both. NOT YET HARDWARE-VERIFIED — compiled
//        + bench-tooling ready (`ozctl.py mqtt-grant`/`mqtt-delete`), needs
//        a real flash + live test.
//   1.24 (2026-08-08): ozkey-13 §8 F7 — the Thread UDP relay's receive half
//        (`pollThreadUdp()`) gets the same `envelope_hex` branch F2 added to
//        `onMqttMessage()`, routed through the identical F1 core
//        (`ozControlOpen`/`ozControlVerifyAndDispatch`, no live challenge).
//        Closes the gap found bench-testing F2/F3: every bench lock is
//        Thread-connected, so sealed grants/deletes could not reach ANY of
//        them without this — F2 alone only ever covered a WiFi-direct lock.
//        Legacy `payload` pure-forward unchanged when `envelope_hex` is
//        absent. Also fixes a real bug: FW_DISPLAY_VERSION was stuck at
//        "V1.21" through the 1.22/1.23 bumps (FW_VERSION moved, the on-
//        screen badge didn't) — exactly the two-versions-disagreeing trap
//        the 1.2 entry above was written to prevent. NOT YET HARDWARE-
//        VERIFIED.
// 1.25   ozkey-17 F8 + U0. F8: the plaintext inside a sealed envelope is now
//        OZKIE semantic JSON ({"kind":"unlock"}, {"kind":"grant_pin",...}) and
//        the Tuya 55 AA frame is built ON THE LOCK, immediately before the MCU
//        write — ozkey-13 had moved frame composition from the server to the
//        app and encrypted it, but a Tuya frame still crossed three network
//        hops as ciphertext. Legacy Tuya-frame plaintext still accepted
//        (discriminated on '{' vs 0x55); that path goes once the app's
//        semantic sender ships. U0: per-bond outbound counter for lock->app
//        sealing, persisted in OZ_BOND_REC's 6 spare bytes as 48-bit, block-
//        reserved (OZ_TX_RESERVE) so it costs one NVS write per 64 sends
//        rather than one per send. OZ_UDP_RX_BUF 512 -> 1024 (semantic JSON is
//        2-3x the Tuya frame it replaces once hex-encoded).
//        U1: the lock can now SPEAK, not only answer. ozUplinkSend() seals
//        lock->app JSON with ozEnvSeal()+ozEnvKey(appToLock=false) — both built
//        in ozkey-06 and never once called outside a self-test — and emits it
//        over MQTT (topicUplink, separate from heartbeat/log so the server can
//        route sealed content without parsing it) or Thread UDP to a new
//        uplink port 5053. ozNotifyRosterChanged() fires on member enrol and
//        bond revoke, pushed to every admin bond: the event whose absence
//        produced XF-75/77/78, where a roster change was real but unobservable
//        until someone next stood at the door.
// 1.31: ozkey-19 v2 R1/R2/R4 — the uplink burst is gone. One MAC-acknowledged
//       unicast datagram replaces 9 mostly-multicast copies; the peer address
//       persists in NVS so a reboot cannot demote the lock to unacknowledged
//       multicast; ff03::4f5a dropped (dead in both directions, errno=125).
// 1.32: ozkey-19 v2 R5 — roster_epoch. Monotonic, NVS-persisted, bumped in
//       ozNotifyRosterChanged() (the single choke point for roster mutations),
//       carried on roster_changed, the heartbeat, and query_roster responses.
//       This is the CORRECTNESS mechanism: the push is a latency optimisation,
//       the epoch is what lets an app notice a missed change with no push
//       having succeeded.  NOT ON THE BENCH BOARD YET — DoorA runs 1.31, so
//       R1 can be measured in isolation before R5 is added.
// 1.66: ensureMqtt() no longer stops the door for 18 seconds at a time.
//       Everything the lock does runs on the loop task, so a blocking broker
//       dial freezes touch, the screen, the MCU wire pump AND the factory-
//       reset gesture. The budget was TCP connect (3 s, NetworkClient's
//       WIFI_CLIENT_DEF_CONN_TIMEOUT_MS) + CONNACK wait (15 s, PubSubClient's
//       MQTT_SOCKET_TIMEOUT), re-entered every 4 s — so a Wi-Fi lock with an
//       unreachable broker was unresponsive ~18 s out of every 22, with no
//       physical way out because the reset button is polled in that same loop.
//       Now: both phases bounded (2 s + 2 s), retry backs off 4 s -> 60 s, and
//       the stall duration is printed so it can never hide again. Backoff
//       resets on connect success, on Wi-Fi coming up, and on sleep wake.
//       Unchanged against a reachable broker. Found while chasing the
//       operator's "panel takes 5-10 s to answer a touch".
// 1.67: the pairing screen stopped lying about whether it is discoverable
//       (operator, 2026-08-14). drawAdvertising() printed "ADVERTISING..."
//       unconditionally, so once the 2-minute post-boot grace lapsed the panel
//       kept claiming the lock was pairable for the rest of its life. The
//       redraw was never broken — the grace-lapse handler and openBleWindow()
//       both set screenDirty — the text simply never consulted
//       bleAdvertisingAllowed(). Now: AMBER "ADVERTISING..." when you can pair,
//       WHITE "NOT ADVERTISING" when you cannot, matching the operational
//       screen's amber-means-live-and-time-limited convention.
// 1.70: ANY keypad key opens the pairing window, not just '#' (operator,
//       2026-08-14) — reverses the 2026-08-11 '#'-only rule; the amber '#'
//       highlight goes with it, because a key that looks different but behaves
//       the same is the 1.67 lying-screen bug in another costume. Plus the
//       instruments for the operator's "10-20 s before the panel responds":
//       touchRead() now samples a tap on its FIRST down poll instead of its
//       second (a single-poll tap used to be discarded in total silence), and
//       the three previously-invisible ways a tap could vanish — I2C NACK, a
//       controller reporting count=0, and our own two-sample rule — all print
//       on change. setup() is now timed stage by stage, because the panel is
//       deaf until setup() returns and nothing had ever measured how long that
//       is. See common/ozdoorlock_core.h for the full reasoning.
// 1.90: the BLE window close log stops lying. It printed a hardcoded "60s
//       elapsed" on every close, left over from before 1.85 shortened the
//       window to 30 s — so a capture taken to CONFIRM the window length
//       reported exactly double it. Behaviour was always correct and is
//       unchanged (BLE_WINDOW_MS has been the single source since 1.85);
//       only the message was stale. Now derived from BLE_WINDOW_MS. Found
//       while bench-verifying the doorbell window/cooldown on LockA, where
//       the measured 01:47:03→01:47:33 close disagreed with its own log line.
// 1.91: BENCH UNBLOCK — ozsim-fullfeature now carries fiction DP 1 in `extra`,
//       so app unlock works on the bench again. PID discovery working is what
//       broke it: the lock correctly switched off ozkie-legacy-v0 onto a
//       real-shaped profile, and DP 1 (`unlock_channel`) is OUR INVENTION that
//       no real profile carries — so ozDpForwardable(1) went false and every
//       app unlock died as "[FWD] REJECTED DP 1". A real DS013-T3 fails the
//       same way. No firmware logic changed; this is the regenerated profile
//       table only. See the $comment in profiles/products/ozsim-fullfeature.json
//       — the real fix is a supplier gap (DP 10 is reserved/blocked, DP 72 is
//       an event report, not a command), not this line.
// 1.92: the MCU reports with 0x07, and we only ever parsed 0x06. The Tuya
//       general variant is a TWO-command-word protocol — 0x06 module-issues,
//       0x07 MCU-reports — and the supplier's own instruction table says so
//       for every DP. A real lock's reports (doorbell, access events, battery
//       alarm, credential reports) would ALL have been dropped as an unparsed
//       "cmd 0x7". Inbound now accepts 0x07, and still accepts 0x06 until
//       LockSim is flipped. Outbound is unchanged and correct at 0x06.
//       Invisible until now because locksim's DP_REPORT is also 0x06: the
//       bench agreed with itself and disagreed with the supplier — the same
//       shape as the DP 1 fiction. See ozkey-39 §1.1, XF-110 §10.2.
// 1.93: the lock now UNDERSTANDS the real access-event DPs — 61 unlock_password,
//       63 unlock_fingerprint, 64 unlock_card, 72 unlock_remote, 73
//       unlock_remote_voice, 76 unlock_ble. They carry a cred_id, so an audit
//       line can finally say WHICH credential opened the door; our fiction
//       never could (DP 2 was a raw card UID, DP 3 a bare bool). Until now all
//       six fell through to UNCLASSIFIED, so on a real lock every access event
//       would have been invisible while the invented DPs carried the whole
//       trail — the same shape as XF-110's DP 1. Gated on ozDpFind() so a lock
//       only honours DPs its own profile selects. REPORT-ONLY: credential
//       WRITES are DP 13/14/15 and remain supplier-blocked (ozkey-39 §2).
// 1.94: BLE unlock now issues the REAL DP 76 `unlock_ble`, and the BLE window
//       doubles 30s -> 60s (cooldown stays 120s).
//
//       DP 76 is issued ONLY when the sealed unlock arrived over BLE and the
//       active profile carries it — the DP asserts HOW the door opened, so a
//       lock must not claim Bluetooth for an MQTT/Thread command. An explicit
//       `viaBle` flag is plumbed through the sealed-dispatch path rather than
//       inferred from `hasChallenge`, which happened to correlate. Network
//       unlock stays on fiction DP 1: DP 10 is supplier-blocked and DP 72 is
//       an event REPORT, not a command (ozkey-39 §3.5).
//
//       🔴 Two gates asked `dp != 1` as a stand-in for "is this an unlock" —
//       correct only while unlock WAS DP 1. Left alone, DP 76 would have
//       denied every MEMBER the one verb they are allowed, and made the
//       latency-critical verb wait on an MCU ack. Both now ask `isUnlock`.
//       The legacy raw-DP path still uses `dp != 1` and correctly so: there
//       the number comes off the wire, not from us.
//
//       Window: operator measured that BLE advertising takes ~15s before the
//       app can SEE the lock. A 30s window therefore left ~15s to unlock a
//       phone, open the app and tap — it was closing underneath people at the
//       door, and BOOT is on the INSIDE of a production lock. I first tried
//       halving the COOLDOWN; the operator's measurement showed the window
//       was the real fault. 60s window, cooldown unchanged at 120s. The
//       "Ns elapsed" log is derived from BLE_WINDOW_MS (1.90) so it follows
//       automatically — a hardcoded string would have started lying again.
#define FW_VERSION "doorlock-1.98"

// ── FW_DISPLAY_VERSION is DERIVED, never hand-maintained (2026-08-12) ────────
//
// It used to be a second literal kept "in step with FW_VERSION above". It was
// not: it sat at V1.21 through the 1.22/1.23 bumps here, and the identical
// trap in bridge32 let its badge drift fourteen versions stale. That is three
// occurrences of one bug across two firmwares, and the instruction to "change
// both, every time" is what re-armed it each time.
//
// FW_VERSION is the single source (operator, 2026-08-02). Derive the badge and
// there is nothing left to forget. bridge32 closed this in 1.32; this is the
// doorlock half, done at the next bump exactly as that commit said it should be.
//
// "doorlock-1.57" -> "V1.57". Falls back to the whole string if the dash ever
// goes missing — wrong, but visibly wrong rather than silently stale.
static const char *fwDisplayVersion() {
  static char buf[16];
  const char *dash = strchr(FW_VERSION, '-');
  snprintf(buf, sizeof(buf), "V%s", dash ? dash + 1 : FW_VERSION);
  return buf;
}
#define FW_DISPLAY_VERSION fwDisplayVersion()

// This board shows no startup splash (never did, pre-refactor) — no-op so
// the shared core's unconditional drawSplash() call is a well-defined hook.
void drawSplash() {}

void touchInit() {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(100);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  Wire.beginTransmission(TOUCH_ADDR);
  int err = Wire.endTransmission();
  Serial.printf("[TOUCH] probe 0x%02X err=%d %s\n", TOUCH_ADDR, err,
                err == 0 ? "(ACK ok)" : "(NO ACK — touch dead)");
}

// Original simple linear transform — unchanged from before the refactor.
// NOTE: x is not clamped here (never was); only y is. Preserved exactly,
// not "fixed", to keep this refactor behavior-neutral.
void mapTouchRaw(int rawX, int rawY, int &x, int &y) {
  x = PANEL_W - rawY;
  y = 180 - (rawX * 6) / 5;
  if (y < 0) y = 0;
  if (y > PANEL_H - 1) y = PANEL_H - 1;
}

#include "../common/ozdoorlock_core.h"
