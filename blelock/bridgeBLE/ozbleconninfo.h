// ozbleconninfo.h — BRIDGE-LAT-1 (2026-11-20): make each held lock's BLE
// connection timing visible over MQTT. NimBLE accepts whatever a peripheral
// asks for (BLEClientCallbacks::onConnParamsUpdateRequest returns true), so
// a lock that requests a long interval or a high slave latency silently makes
// every relay hop wait. Measured: AK1037 relay round trips quantised in ~2 s
// steps (2.4 / 4.4 / 6.3 s). This publishes what the link really runs.
//
// Main task only (MQTT is not safe from the NimBLE host task): read with
// ble_gap_conn_find() from loop(), publish on change, NOT retained, to
//   ozkie/<site>/bridges/<id>/diag  {"lock","conn_itvl_ms","latency","timeout_ms"}
// Included after ozblecentral.h (needs OzBleLock / g_bleLocks).
#ifndef OZBLECONNINFO_H
#define OZBLECONNINFO_H
#include "host/ble_gap.h"

#define OZ_CONNINFO_EVERY_MS 5000UL

// Main-loop cadence: a relay frame waits for the next loop pass in both
// directions (mqttClient.loop() reads downlinks; the tick drains uplinks), so
// a slow pass is relay latency. Every 30 s: passes and the longest pass.
#define OZ_LOOPSTAT_EVERY_MS 30000UL
static void ozBleLoopStatTick() {
  static unsigned long prev = 0, winStart = 0;
  static uint32_t passes = 0, maxGap = 0, over500 = 0;
  const unsigned long now = millis();
  if (prev) {
    const uint32_t gap = now - prev;
    if (gap > maxGap) maxGap = gap;
    if (gap > 500) over500++;
  } else {
    winStart = now;
  }
  prev = now;
  passes++;
  if (now - winStart < OZ_LOOPSTAT_EVERY_MS) return;
  char js[160];
  snprintf(js, sizeof js, "{\"loop_passes\":%lu,\"window_ms\":%lu,\"loop_max_ms\":%lu,\"passes_over_500ms\":%lu,\"fw\":\"%s\"}",
           (unsigned long)passes, (unsigned long)(now - winStart), (unsigned long)maxGap,
           (unsigned long)over500, FW_VERSION);
  Serial.printf("[LOOP] %s\n", js);
  if (mqttClient.connected()) {
    const String t = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/diag";
    mqttClient.publish(t.c_str(), js, false);
  }
  winStart = now; passes = 0; maxGap = 0; over500 = 0;
}

static void ozBleConnInfoTick() {
  ozBleLoopStatTick();
  static unsigned long last = 0;
  static uint32_t seen[OZ_BLE_MAX_LOCKS];          // packed itvl|lat|to, 0 = none yet
  if (millis() - last < OZ_CONNINFO_EVERY_MS) return;
  last = millis();
  uint8_t i = 0;
  for (auto &l : g_bleLocks) {
    const uint8_t k = i++;
    if (!l.inUse || !l.client || !l.client->isConnected() || !l.deviceId.length()) { seen[k] = 0; continue; }
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(l.client->getConnId(), &d) != 0) continue;
    const uint32_t key = ((uint32_t)d.conn_itvl << 16) ^ ((uint32_t)d.conn_latency << 8) ^ d.supervision_timeout;
    if (key == seen[k]) continue;
    seen[k] = key;
    char js[160];
    snprintf(js, sizeof js,
             "{\"lock\":\"%s\",\"conn_itvl_ms\":%.2f,\"latency\":%u,\"timeout_ms\":%u,\"fw\":\"%s\"}",
             l.deviceId.c_str(), d.conn_itvl * 1.25, (unsigned)d.conn_latency,
             (unsigned)d.supervision_timeout * 10u, FW_VERSION);
    Serial.printf("[BLE-CONN] %s\n", js);
    if (mqttClient.connected()) {
      const String t = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/diag";
      mqttClient.publish(t.c_str(), js, false);
    }
  }
}
#endif
