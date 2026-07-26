/*
 * RFRangeTest — standalone Thread RF range/loss bench tool (ozkey-09 gap #5,
 * "F6" in the firmware task list).
 * Board  : ESP32-C6 (any C6 board — no display/touch dependency)
 *
 * PURPOSE: measure whether the bridge32<->threadcomm Thread link actually
 * survives realistic residential distance/walls before more is built on
 * top of it (ozkey-09 §6 step 4 — "cheaper to find out now than after two
 * more features are built on an unproven radio link"). Standalone — does
 * NOT depend on F1-F5, BLE provisioning, or NVS. Flash the identical
 * sketch to two C6 boards, flip ROLE_SENDER on one of them, and walk.
 *
 * HOW TO RUN
 *   1. Flash with ROLE_SENDER=1 to one board (stays put, e.g. at the
 *      bridge's planned install spot) — this becomes the Thread Leader
 *      and just transmits numbered pings on a timer.
 *   2. Edit ROLE_SENDER to 0 below and flash the other board (the one you
 *      carry around) — it joins as a Child of the sender and is the one
 *      to watch on the Serial Monitor.
 *   3. Walk the receiver to 5m / 10m / 15m (through the actual walls you
 *      care about) and read the printed loss% + RSSI at each spot. Press
 *      BOOT on the receiver at each new distance to reset counters and
 *      print a checkpoint marker in the log, so the run is easy to split
 *      into per-distance segments after the fact.
 *
 * Uses a fixed, hardcoded Thread dataset on a different channel (20) from
 * bridge32's (15) so this test mesh never collides with — or accidentally
 * joins — a real bridge32/threadcomm pair running on the same bench.
 *
 * RSSI source: otThreadGetParentLastRssi() / otThreadGetParentAverageRssi()
 * — the receiver's link quality to its Thread parent. With exactly two
 * nodes on this test mesh, the receiver's only parent IS the sender, so
 * this is a direct read of the physical RF path between the two boards.
 * (Only valid while the receiver holds Child role; if OpenThread ever
 * promotes it to Router — unlikely with just one other node on the mesh
 * — the calls return an error and the log prints "n/a" instead of a
 * stale number.)
 */

#include <Arduino.h>
#include "OThread.h"
#include "OThreadUDP.h"
#include <ArduinoJson.h>
#include <openthread/thread.h>

// ── Set to 1 on exactly one of the two boards, 0 on the other. Can also be
//    overridden at compile time with -DROLE_SENDER=0 for CI/compile checks.
#ifndef ROLE_SENDER
#define ROLE_SENDER 1
#endif

// ── Fixed test network (deliberately distinct from bridge32's) ────────────
const char TEST_NETWORK_NAME[] = "OZ-RFTEST";
const uint8_t TEST_CHANNEL = 20;
const uint16_t TEST_PAN_ID = 0xBEEF;
const uint8_t TEST_EXTPANID[OT_EXT_PAN_ID_SIZE] = {0xBE, 0xEF, 0xBE, 0xEF, 0xBE, 0xEF, 0xBE, 0xEF};
const uint8_t TEST_NETKEY[OT_NETWORK_KEY_SIZE] = {
  0x52, 0x46, 0x54, 0x45, 0x53, 0x54, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09
};

// Realm-local multicast group + application port for the ping traffic
// itself (separate from bridge32/threadcomm's own OZ_THREAD_GROUP/PORT).
const uint8_t TEST_GROUP_BYTES[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x52, 0x54};  // "RT"
const IPAddress TEST_GROUP(IPv6, TEST_GROUP_BYTES);
const uint16_t TEST_PORT = 5099;
const uint32_t SEND_INTERVAL_MS = 500;

#ifndef USER_BUTTON
#define USER_BUTTON BOOT_PIN
#endif

OThreadUDP testUdp;
DataSet testDataset;

void joinTestNetwork() {
  OThread.begin(false);
#if ROLE_SENDER
  testDataset.initNew();
  testDataset.setNetworkName(TEST_NETWORK_NAME);
  testDataset.setExtendedPanId(TEST_EXTPANID);
  testDataset.setNetworkKey(TEST_NETKEY);
  testDataset.setChannel(TEST_CHANNEL);
  testDataset.setPanId(TEST_PAN_ID);
  OThread.commitDataSet(testDataset);
#else
  // Matches the OpenThread-library "RouterNode" example pattern: only the
  // network key needs to be set for a joiner with a partial dataset — no
  // channel/PAN ID required to attach.
  testDataset.clear();
  testDataset.setNetworkKey(TEST_NETKEY);
  OThread.commitDataSet(testDataset);
#endif
  OThread.networkInterfaceUp();
  OThread.start();

  Serial.printf(
    "[RF-TEST] role=%s joining '%s' ch=%u...\n", ROLE_SENDER ? "SENDER(leader)" : "RECEIVER(child)", TEST_NETWORK_NAME, TEST_CHANNEL
  );
  while (OThread.otGetDeviceRole() < OT_ROLE_CHILD) {
    Serial.print(".");
    delay(1000);
  }
  Serial.printf("\n[RF-TEST] attached as %s\n", OThread.otGetStringDeviceRole());
}

#if ROLE_SENDER
// ─────────────────────────────────────────────────────────────────────────
// SENDER — Thread Leader, transmits {"seq":N,"ts":millis()} on a timer.
// ─────────────────────────────────────────────────────────────────────────
uint32_t seq = 0;
uint32_t lastSendMs = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** RFRangeTest — SENDER (Thread Leader) ***");
  joinTestNetwork();
  if (!testUdp.begin(TEST_PORT)) {
    Serial.println("[RF-TEST] UDP begin failed");
    while (1) delay(1000);
  }
  Serial.printf(
    "[RF-TEST] sending to [%s]:%u every %lu ms\n", TEST_GROUP.toString().c_str(), TEST_PORT, (unsigned long)SEND_INTERVAL_MS
  );
}

void loop() {
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    JsonDocument doc;
    doc["seq"] = seq;
    doc["ts"] = millis();
    String out;
    serializeJson(doc, out);
    if (testUdp.beginPacket(TEST_GROUP, TEST_PORT)) {
      testUdp.write((const uint8_t *)out.c_str(), out.length());
      testUdp.endPacket();
      Serial.printf("[RF-TEST] TX seq=%lu\n", (unsigned long)seq);
    } else {
      Serial.println("[RF-TEST] TX beginPacket failed");
    }
    seq++;
  }
  delay(10);
}

#else
// ─────────────────────────────────────────────────────────────────────────
// RECEIVER — Thread Child, logs RSSI + tracks sequence gaps as loss.
// ─────────────────────────────────────────────────────────────────────────
bool haveFirst = false;
uint32_t nextExpected = 0;
uint32_t received = 0, lost = 0, outOfOrder = 0;

void formatRssi(otError err, int8_t rssi, char *out, size_t outLen) {
  if (err == OT_ERROR_NONE) snprintf(out, outLen, "%d dBm", rssi);
  else snprintf(out, outLen, "n/a");
}

void printSummary() {
  uint32_t total = received + lost;
  float lossPct = total ? (100.0f * lost / total) : 0.0f;
  int8_t avgRssi = 0;
  otError err = otThreadGetParentAverageRssi(OThread.getInstance(), &avgRssi);
  char rssiBuf[16];
  formatRssi(err, avgRssi, rssiBuf, sizeof(rssiBuf));
  Serial.printf(
    "[RF-TEST] rx=%lu lost=%lu ooo=%lu loss=%.1f%% avgParentRssi=%s\n", (unsigned long)received, (unsigned long)lost,
    (unsigned long)outOfOrder, lossPct, rssiBuf
  );
}

void resetCounters(const char *why) {
  haveFirst = false;
  nextExpected = 0;
  received = 0;
  lost = 0;
  outOfOrder = 0;
  Serial.printf("\n=== [RF-TEST] CHECKPOINT (%s) — counters reset ===\n", why);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(USER_BUTTON, INPUT_PULLUP);
  Serial.println("\n*** RFRangeTest — RECEIVER (Thread Child) ***");
  Serial.println("Press BOOT at each new distance to print a summary + reset counters.");
  joinTestNetwork();
  if (!testUdp.beginMulticast(TEST_GROUP, TEST_PORT)) {
    Serial.println("[RF-TEST] UDP beginMulticast failed");
    while (1) delay(1000);
  }
  resetCounters("start");
}

void loop() {
  static uint32_t lastButton = 0;
  if (digitalRead(USER_BUTTON) == LOW && millis() - lastButton > 500) {
    lastButton = millis();
    printSummary();
    resetCounters("button press");
  }

  int n = testUdp.parsePacket();
  if (n > 0) {
    char buf[128];
    int got = testUdp.read(buf, (n < (int)sizeof(buf) - 1) ? n : (int)sizeof(buf) - 1);
    buf[got] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      uint32_t s = doc["seq"] | 0;
      int8_t lastRssi = 0;
      otError err = otThreadGetParentLastRssi(OThread.getInstance(), &lastRssi);
      char rssiBuf[16];
      formatRssi(err, lastRssi, rssiBuf, sizeof(rssiBuf));

      received++;
      if (!haveFirst) {
        haveFirst = true;
        nextExpected = s + 1;
      } else if (s == nextExpected) {
        nextExpected = s + 1;
      } else if (s > nextExpected) {
        lost += (s - nextExpected);
        nextExpected = s + 1;
      } else {
        outOfOrder++;
      }
      Serial.printf("[RF-TEST] RX seq=%lu rssi=%s\n", (unsigned long)s, rssiBuf);
    } else {
      Serial.println("[RF-TEST] payload not valid JSON, dropped");
    }
  }

  static uint32_t lastPeriodic = 0;
  if (millis() - lastPeriodic > 5000) {
    lastPeriodic = millis();
    printSummary();
  }

  delay(10);
}
#endif
