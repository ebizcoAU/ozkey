// ozbcmdglue.h — BRIDGE-CMD-AUTH-1 (bridge, step 2): signed bridge commands in
// bridgeBLE (CONTRACT-BRIDGE.md "MQTT signed bridge commands" v1). Included by
// bridgeBLE.ino just before mqttMessageReceived(); the verifier is ozbcmd.h.
//
// - Subscribes to ozkie/<site>/bridges/<id>/cmd only. The legacy JSON
//   …/command topics are NOT subscribed and anything arriving there is dropped:
//   with a site key the bridge never acts on them (no downgrade); without one
//   it accepts no MQTT command at all (BLE commissioning or the bench console).
// - A verified command is executed by the SAME per-op code as before
//   (handleCommandJson), from a JSON document rebuilt from the verified fields;
//   factory_reset / unpair are authorised by the signature (g_bcmdAuthorised).
// - The site public key and the counter live in NVS ("bridge32": bc_pub,
//   bc_ctr), so factoryReset() erases them with the rest.
// - Refusals are counted by reason on …/diag (not retained).
// - The retain flag: PubSubClient's callback does not expose it, so a retained
//   frame is handled by the counter rule (CONTRACT delta, agreed with ozkey-srv).
#ifndef OZBCMDGLUE_H
#define OZBCMDGLUE_H
#include "ozbcmd.h"

static OzBcmdState g_bcmd;
static bool g_bcmdAuthorised = false;       // true only while a signed command runs
static uint32_t g_bcmdRefused[OZB_N_RESULTS];
static uint32_t g_bcmdLegacyDropped = 0;
static bool g_bcmdDiagDirty = false;
String mqttCmdSignedTopic;

static void handleCommandJson(JsonDocument &doc);   // bridgeBLE.ino: the per-op code

static int ozHexBytes(const String &h, uint8_t *out, size_t n) {
  if (h.length() != 2 * n) return 0;
  for (size_t i = 0; i < n; i++) {
    char b[3] = { h[2 * i], h[2 * i + 1], 0 };
    char *end;
    out[i] = (uint8_t)strtoul(b, &end, 16);
    if (*end) return 0;
  }
  return 1;
}

static void ozBcmdLoad() {
  prefs.begin("bridge32", true);
  uint8_t pub[32];
  const bool has = prefs.getBytes("bc_pub", pub, sizeof pub) == sizeof pub;
  const uint64_t ctr = prefs.getULong64("bc_ctr", 0);
  prefs.end();
  memset(&g_bcmd, 0, sizeof g_bcmd);
  if (has) ozBcmdProvision(&g_bcmd, pub, ctr);
  Serial.printf("[BCMD] signed commands: site key %s, ctr %llu\n", has ? "SET" : "NOT set (no MQTT commands)",
                (unsigned long long)ctr);
}

static bool ozBcmdSaveCtr(uint64_t ctr) {
  prefs.begin("bridge32", false);
  const bool ok = prefs.putULong64("bc_ctr", ctr) == sizeof(uint64_t);
  prefs.end();
  return ok;
}

// Commissioning (provision payload in the claim window) or the bench console.
static bool ozBcmdSetKey(const uint8_t pub[32], uint64_t floor) {
  prefs.begin("bridge32", false);
  const bool ok = prefs.putBytes("bc_pub", pub, 32) == 32 && prefs.putULong64("bc_ctr", floor) == sizeof(uint64_t);
  prefs.end();
  if (ok) ozBcmdProvision(&g_bcmd, pub, floor);
  Serial.printf("[BCMD] site key %s (ctr floor %llu)\n", ok ? "SET" : "NOT saved (NVS)", (unsigned long long)floor);
  return ok;
}

static void ozBcmdDiag() {
  if (!g_bcmdDiagDirty || !mqttClient.connected()) return;
  g_bcmdDiagDirty = false;
  char js[200];
  snprintf(js, sizeof js,
           "{\"cmd_refused\":{\"sig\":%lu,\"bridge\":%lu,\"replay\":%lu,\"parse\":%lu,\"nokey\":%lu,\"op\":%lu},"
           "\"legacy_dropped\":%lu}",
           (unsigned long)g_bcmdRefused[OZB_E_SIG], (unsigned long)g_bcmdRefused[OZB_E_BRIDGE],
           (unsigned long)g_bcmdRefused[OZB_E_REPLAY], (unsigned long)g_bcmdRefused[OZB_E_PARSE],
           (unsigned long)g_bcmdRefused[OZB_E_NOKEY], (unsigned long)g_bcmdRefused[OZB_E_OP],
           (unsigned long)g_bcmdLegacyDropped);
  const String t = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/diag";
  mqttClient.publish(t.c_str(), js, false);
}

static void ozBcmdExecute(const OzBcmd &c) {
  JsonDocument doc;
  switch (c.op) {
    case OZB_OP_PAIR_LOCK:    doc["op"] = "pair_lock";    doc["id"] = c.id; break;
    case OZB_OP_UNPAIR_LOCK:  doc["op"] = "unpair_lock";  doc["id"] = c.id; break;
    case OZB_OP_RELEASE_LOCK: doc["op"] = "release_lock"; doc["id"] = c.id; break;
    case OZB_OP_SCAN:
      doc["op"] = "scan";
      if (c.id[0]) doc["target"] = c.id;
      doc["duration_s"] = c.duration_s;
      break;
    case OZB_OP_FACTORY_RESET: doc["op"] = "factory_reset"; break;
    case OZB_OP_UNPAIR:        doc["op"] = "unpair"; break;
    case OZB_OP_TIME: {
      // The contract: signed time only moves the bridge clock FORWARD.
      const time_t now = time(nullptr);
      if ((time_t)c.utc <= now) {
        Serial.printf("[BCMD] time %llu not ahead of the clock (%ld): ignored\n",
                      (unsigned long long)c.utc, (long)now);
        return;
      }
      doc["utc"] = (uint32_t)c.utc;
      break;
    }
    case OZB_OP_TZ: doc["tz"] = c.tz_min; break;    // the existing tz path (panel only)
    case OZB_OP_SET_SITE_KEY: {
      prefs.begin("bridge32", false);
      const bool ok = prefs.putBytes("bc_pub", c.new_pub, 32) == 32;
      prefs.end();
      if (ok) ozBcmdRotate(&g_bcmd, c.new_pub);
      Serial.printf("[BCMD] set_site_key: %s\n", ok ? "new key in NVS, rotated" : "NVS write FAILED, old key kept");
      return;
    }
    default: return;
  }
  g_bcmdAuthorised = true;        // the signature authorises factory_reset / unpair
  handleCommandJson(doc);
  g_bcmdAuthorised = false;
}

// Every MQTT message that is not an OZKIE v2 downlink.
static void ozBcmdOnMqtt(const char *topic, const byte *payload, unsigned int len) {
  if (!mqttCmdSignedTopic.length() || strcmp(topic, mqttCmdSignedTopic.c_str()) != 0) {
    g_bcmdLegacyDropped++;
    g_bcmdDiagDirty = true;
    Serial.printf("[BCMD] unsigned message on %s dropped (signed commands only)\n", topic);
    return;
  }
  OzBcmd c;
  const int r = ozBcmdCheck(&g_bcmd, deviceId.c_str(), payload, len, &c);
  if (r == OZB_DUP) return;                    // a QoS 1 redelivery: silent
  if (r != OZB_OK) {
    g_bcmdRefused[r]++;
    g_bcmdDiagDirty = true;
    Serial.printf("[BCMD] REFUSED (%s), %u B\n", ozBcmdResultName(r), len);
    return;
  }
  if (c.persist) {
    if (!ozBcmdSaveCtr(c.ctr)) {
      Serial.println("[BCMD] ctr NOT saved (NVS): not acting");
      return;
    }
    ozBcmdSaved(&g_bcmd, c.ctr);
  }
  Serial.printf("[BCMD] accepted op %d ctr %llu\n", c.op, (unsigned long long)c.ctr);
  ozBcmdExecute(c);
}
#endif
