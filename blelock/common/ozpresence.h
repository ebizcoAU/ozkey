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
 * @param via       the BRIDGE's own device_id, when this report is a bridge
 *                  relaying a BLE-paired lock's connect/disconnect on its
 *                  behalf — empty/null for every other producer (a Thread
 *                  lock's own beacon, a Wi-Fi lock's own direct publish, the
 *                  broker's LWT). XF-144 2026-09-02: server needs to know
 *                  WHICH bridge a presence report came from to correct
 *                  `locks.bridge_id` when it drifts from reality (a bridge
 *                  recovering and silently reclaiming a lock the server had
 *                  since reassigned elsewhere) — nothing in the payload
 *                  carried that before. Omitted, not empty-string, when not
 *                  applicable — same convention `reason`/`msg_id` already use.
 */
static String ozBuildLockPresence(const String &deviceId, bool online,
                                  const char *reason, const String &msgId,
                                  const char *fw = nullptr,
                                  const String &via = String()) {
  JsonDocument doc;
  doc["state"] = online ? "online" : "offline";
  doc["id"] = deviceId;
  // Present so a single handler can serve both `locks/+/presence` and
  // `bridges/+/presence` without inferring the device class from the topic.
  doc["role"] = "lock";
  if (reason && *reason) doc["reason"] = reason;
  if (msgId.length()) doc["msg_id"] = msgId;
  if (fw && *fw) doc["fw"] = fw;
  if (via.length()) doc["via"] = via;
  String out;
  serializeJson(doc, out);
  return out;
}

/**
 * Should this outcome be published RETAINED?
 *
 * 🔴 2026-09-01, XF-146 — was `!online` (retain offline only). Found live on
 * the bench: a late/reconnecting subscriber that misses the one-shot online
 * transition has nothing to fall back to except the last RETAINED message on
 * the topic — which, under the old rule, could only ever be an old offline.
 * Bridge 1 held a lock for hours, its last (retained) word on the topic was
 * "offline"; a different bridge later took over and published a genuinely
 * successful "online" — not retained, so it never overwrote the stale
 * snapshot. Confirmed against the live rig, not theoretical (XF-146).
 *
 * Retain BOTH transitions now: presence is a liveness state either
 * direction, and a subscriber should never be able to read a stale opposite
 * state just because it wasn't listening at the exact moment things changed
 * back. Server confirmed this is purely additive on their side —
 * `handleLockPresence()` already treats a retained redelivery identically to
 * a live push (XF-146).
 */
static inline bool ozPresenceShouldRetain(bool online) { (void)online; return true; }
