// ozpresence.h — the ONE definition of a lock's presence payload.
//
// WHY THIS FILE EXISTS
// --------------------
// `ozkie/<site>/locks/<device_id>/presence` has TWO producers:
//
//   • a Wi-Fi lock, publishing for itself over its own MQTT session, and
//   • bridge32, publishing on behalf of a Thread lock that has no MQTT session.
//
// On 2026-08-19 those two diverged within hours of both existing. The Wi-Fi path
// emitted {state, id, role, reason, msg_id}; the Thread path forwarded the
// lock's internal Thread datagram verbatim — {from, kind, state, reason} — so
// the same wildcard subscription delivered two different schemas, one of them
// RETAINED and therefore redelivered on every subscribe forever.
//
// That is the same failure the `profiles/` layer was built to end, one protocol
// down: two ends of a wire each holding their own idea of a format, agreeing
// only by everyone remembering to. The fix is the same — remove the ability to
// disagree. Both producers call ozBuildLockPresence() and there is no second
// place where the shape is written down.
//
// 🔴 DO NOT construct a lock presence payload by hand. If a field needs adding,
// add it here and both transports get it in the same build.
//
// See XF-114 §14, XF-115, ozkey-41 §2.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Canonical `reason` values. Strings rather than an enum because they cross a
// wire to consumers in three other languages; the constants exist so a typo is a
// compile error on THIS side at least.
#define OZ_PRESENCE_RESET        "factory_reset"        // wiped — retained
#define OZ_PRESENCE_RESET_DENIED "factory_reset_denied" // refused — NOT retained
#define OZ_PRESENCE_NO_BOND      "no_bond"              // sender unknown to us
#define OZ_PRESENCE_LWT          "lwt"                  // broker-published Will
#define OZ_PRESENCE_ONLINE       ""                     // alive; no reason field

/**
 * Build the canonical lock-presence payload.
 *
 * @param deviceId  the lock's own id — NOT the publisher's. When bridge32 sends
 *                  this for a Thread child it must pass the CHILD's id, or the
 *                  message names the wrong device.
 * @param online    true -> "online"; false -> "offline".
 * @param reason    one of the OZ_PRESENCE_* values. Empty/null omits the field
 *                  entirely rather than emitting "" — a consumer testing for
 *                  presence-of-key must not see an empty string as a reason.
 * @param msgId     the request this outcome answers, or empty. Omitted when
 *                  empty: a reset nobody requested over the network (the BOOT
 *                  hold, the DL MCU's own button) has no id to name, and
 *                  inventing one would be worse than saying nothing.
 * @param fw        optional firmware version, for the `online` case. Empty to
 *                  omit.
 */
static String ozBuildLockPresence(const String &deviceId, bool online,
                                  const char *reason, const String &msgId,
                                  const char *fw = nullptr) {
  JsonDocument doc;
  doc["state"] = online ? "online" : "offline";
  doc["id"] = deviceId;
  // Present so a single handler can serve both `locks/+/presence` and
  // `bridges/+/presence` without inferring the device class from the topic.
  doc["role"] = "lock";
  if (reason && *reason) doc["reason"] = reason;
  if (msgId.length()) doc["msg_id"] = msgId;
  if (fw && *fw) doc["fw"] = fw;
  String out;
  serializeJson(doc, out);
  return out;
}

/**
 * Should this outcome be published RETAINED?
 *
 * Retain iff the lock actually went away. The distinction is bridge32's original
 * rule and it matters: a refusal is an EVENT, not a liveness state — retaining
 * one would replay "I refused" to every future subscriber long after the request
 * that caused it is gone.
 *
 * Derived from `online` rather than passed in, so the rule lives in one place
 * and a caller cannot get it wrong.
 */
static inline bool ozPresenceShouldRetain(bool online) { return !online; }
