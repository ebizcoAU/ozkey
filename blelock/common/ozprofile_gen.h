// GENERATED FILE — DO NOT EDIT.
//
// Source: profiles/*.json. Regenerate with:
//     python3 blelock/tools/gen_profile.py
// Verify it is current with:
//     python3 blelock/tools/gen_profile.py --check
//
// Editing this file by hand reintroduces exactly the failure the profile
// layer exists to prevent: firmware and LockSim holding different ideas of
// what a DP number means (ozkey-27 §2.1).
#pragma once
#include <stdint.h>

enum OzDpDir : uint8_t { OZ_DIR_UP = 0, OZ_DIR_DOWN = 1, OZ_DIR_BOTH = 2 };
enum OzDpStatus : uint8_t {
  OZ_DP_CONFIRMED = 0, // type AND payload semantics documented
  OZ_DP_RESERVED  = 1, // DP known, payload layout NOT supplied -> UNSUPPORTED
  OZ_DP_UNKNOWN   = 2, // seen but unestablished -> log id+len only
  OZ_DP_FICTION   = 3, // we invented it. ozkie-legacy-v0 only.
};

struct OzDpEntry {
  uint16_t   dp;
  OzDpDir    dir;
  OzDpStatus status;
  const char *name;
};

struct OzVerbMap {
  const char *verb;   // OZKIE verb, e.g. "lock.unlock"
  const char *field;  // sub-type, e.g. "pin"/"ble"; nullptr if none
  OzDpDir     dir;    // OZ_DIR_DOWN = command, OZ_DIR_UP = report
  uint16_t    dp;
  uint8_t     type;   // Tuya wire type — 0x00 RAW .. 0x05 BITMAP
  OzDpStatus  status; // RESERVED here means: known DP, unusable payload
};

struct OzProfile {
  const char       *id;
  uint16_t          rev;   // ozkey-28 §3.6 — device.info reports this
  const OzDpEntry  *entries;
  uint16_t          count;
  bool              deprecated;
  // Tuya product ID, from profiles/products/*.json supplier.pid.
  // This is what the DL MCU reports to command 0x01, so it is how a
  // lock identifies ITSELF instead of being told what it is.
  // nullptr where we have no PID (our own invented map).
  const char       *tuya_pid;
  // Verb resolver table for THIS product — see OzVerbMap.
  const OzVerbMap  *verbs;
  uint16_t          verb_count;
};

// tuya-ladin-f7-t3 — 4 DPs
static const OzDpEntry OZ_DP_tuya_ladin_f7_t3[] = {
  {  42, OZ_DIR_BOTH, OZ_DP_UNKNOWN   , "ble_switch" },
  {  76, OZ_DIR_BOTH, OZ_DP_UNKNOWN   , "unlock_ble" },
  { 149, OZ_DIR_BOTH, OZ_DP_UNKNOWN   , "reserved_do_not_select" },
  { 212, OZ_DIR_UP  , OZ_DP_UNKNOWN   , "initiative_message" },
};

static const OzVerbMap OZ_VERBS_tuya_ladin_f7_t3[] = {
  { "event.access", nullptr               , OZ_DIR_UP   ,  76, 0x02, OZ_DP_UNKNOWN },
  { "lock.unlock", "ble"                 , OZ_DIR_DOWN ,  76, 0x02, OZ_DP_UNKNOWN },
};

// tuya-luona-ds013-t3 — 34 DPs
static const OzDpEntry OZ_DP_tuya_luona_ds013_t3[] = {
  {   9, OZ_DIR_BOTH, OZ_DP_RESERVED  , "remote_no_pw_unlock_setting" },
  {  10, OZ_DIR_BOTH, OZ_DP_RESERVED  , "remote_unlock" },
  {  11, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "connection_mode" },
  {  13, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_unlock_method_add" },
  {  14, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_unlock_method_delete" },
  {  15, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_unlock_method_modify" },
  {  16, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_password_add" },
  {  17, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_password_delete" },
  {  18, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_password_modify" },
  {  19, OZ_DIR_BOTH, OZ_DP_RESERVED  , "bulk_unlock_method_sync" },
  {  21, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "navigation_volume" },
  {  23, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "auto_lock" },
  {  24, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "auto_lock_delay" },
  {  42, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "ble_switch" },
  {  45, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "battery_percentage" },
  {  47, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "bolt_state" },
  {  52, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "opened_from_inside" },
  {  53, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "doorbell" },
  {  54, OZ_DIR_BOTH, OZ_DP_RESERVED  , "device_info" },
  {  60, OZ_DIR_UP  , OZ_DP_CONFIRMED , "alarm" },
  {  61, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_password" },
  {  63, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_fingerprint" },
  {  64, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_card" },
  {  69, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_temporary" },
  {  72, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_remote" },
  {  73, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_remote_voice" },
  {  74, OZ_DIR_BOTH, OZ_DP_RESERVED  , "unlock_combination_record" },
  {  76, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_ble" },
  {  86, OZ_DIR_BOTH, OZ_DP_RESERVED  , "offline_password_params" },
  {  87, OZ_DIR_UP  , OZ_DP_RESERVED  , "offline_password_clear_single_report" },
  {  88, OZ_DIR_UP  , OZ_DP_RESERVED  , "offline_password_clear_all_report" },
  {  89, OZ_DIR_UP  , OZ_DP_RESERVED  , "offline_password_unlock_report" },
  {  98, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "hijack_alarm" },
  { 156, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "wifi_connection_strategy" },
};

static const OzVerbMap OZ_VERBS_tuya_luona_ds013_t3[] = {
  { "cred.clear", "all"                 , OZ_DIR_DOWN ,  88, 0x00, OZ_DP_RESERVED },
  { "cred.clear", "one"                 , OZ_DIR_DOWN ,  87, 0x00, OZ_DP_RESERVED },
  { "cred.delete", "pin"                 , OZ_DIR_DOWN ,  17, 0x00, OZ_DP_RESERVED },
  { "cred.delete", "pin"                 , OZ_DIR_UP   ,  17, 0x00, OZ_DP_RESERVED },
  { "cred.delete", "rfid"                , OZ_DIR_DOWN ,  14, 0x00, OZ_DP_RESERVED },
  { "cred.delete", "rfid|fingerprint"    , OZ_DIR_UP   ,  14, 0x00, OZ_DP_RESERVED },
  { "cred.modify", "pin"                 , OZ_DIR_DOWN ,  18, 0x00, OZ_DP_RESERVED },
  { "cred.modify", "rfid"                , OZ_DIR_DOWN ,  15, 0x00, OZ_DP_RESERVED },
  { "cred.put", nullptr               , OZ_DIR_UP   ,  15, 0x00, OZ_DP_RESERVED },
  { "cred.put", "offline_pin"         , OZ_DIR_DOWN ,  86, 0x03, OZ_DP_RESERVED },
  { "cred.put", "offline_pin"         , OZ_DIR_UP   ,  86, 0x03, OZ_DP_RESERVED },
  { "cred.put", "pin"                 , OZ_DIR_DOWN ,  16, 0x00, OZ_DP_RESERVED },
  { "cred.put", "pin"                 , OZ_DIR_UP   ,  16, 0x00, OZ_DP_RESERVED },
  { "cred.put", "pin"                 , OZ_DIR_UP   ,  18, 0x00, OZ_DP_RESERVED },
  { "cred.put", "rfid"                , OZ_DIR_DOWN ,  13, 0x00, OZ_DP_RESERVED },
  { "cred.put", "rfid|fingerprint"    , OZ_DIR_UP   ,  13, 0x00, OZ_DP_RESERVED },
  { "cred.sync", nullptr               , OZ_DIR_DOWN ,  19, 0x00, OZ_DP_RESERVED },
  { "cred.sync", nullptr               , OZ_DIR_UP   ,  19, 0x00, OZ_DP_RESERVED },
  { "device.info", nullptr               , OZ_DIR_DOWN ,  54, 0x00, OZ_DP_RESERVED },
  { "device.info", nullptr               , OZ_DIR_UP   ,  54, 0x00, OZ_DP_RESERVED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  61, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  63, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  64, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  69, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  72, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  73, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  76, 0x02, OZ_DP_CONFIRMED },
  { "event.access", nullptr               , OZ_DIR_UP   ,  74, 0x00, OZ_DP_RESERVED },
  { "event.access", nullptr               , OZ_DIR_UP   ,  89, 0x00, OZ_DP_RESERVED },
  { "event.alarm", "type"                , OZ_DIR_UP   ,  60, 0x04, OZ_DP_CONFIRMED },
  { "event.battery", "percent"             , OZ_DIR_UP   ,  45, 0x02, OZ_DP_CONFIRMED },
  { "event.bolt", "locked"              , OZ_DIR_UP   ,  47, 0x01, OZ_DP_CONFIRMED },
  { "event.cred_cleared", nullptr               , OZ_DIR_UP   ,  87, 0x00, OZ_DP_RESERVED },
  { "event.cred_cleared", nullptr               , OZ_DIR_UP   ,  88, 0x00, OZ_DP_RESERVED },
  { "event.doorbell", nullptr               , OZ_DIR_UP   ,  53, 0x01, OZ_DP_CONFIRMED },
  { "event.duress", nullptr               , OZ_DIR_UP   ,  98, 0x01, OZ_DP_CONFIRMED },
  { "event.inside_open", nullptr               , OZ_DIR_UP   ,  52, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock"            , OZ_DIR_DOWN ,  23, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock"            , OZ_DIR_UP   ,  23, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock_delay"      , OZ_DIR_DOWN ,  24, 0x02, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock_delay"      , OZ_DIR_UP   ,  24, 0x02, OZ_DP_CONFIRMED },
  { "lock.settings.set", "ble_enabled"         , OZ_DIR_DOWN ,  42, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "ble_enabled"         , OZ_DIR_UP   ,  42, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "conn_mode"           , OZ_DIR_DOWN ,  11, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "conn_mode"           , OZ_DIR_UP   ,  11, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "volume"              , OZ_DIR_DOWN ,  21, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "volume"              , OZ_DIR_UP   ,  21, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "wifi_strategy"       , OZ_DIR_DOWN , 156, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "wifi_strategy"       , OZ_DIR_UP   , 156, 0x04, OZ_DP_CONFIRMED },
  { "lock.settings.set", "remote_no_pw_unlock" , OZ_DIR_DOWN ,   9, 0x00, OZ_DP_RESERVED },
  { "lock.settings.set", "remote_no_pw_unlock" , OZ_DIR_UP   ,   9, 0x00, OZ_DP_RESERVED },
  { "lock.unlock", "ble"                 , OZ_DIR_DOWN ,  76, 0x02, OZ_DP_CONFIRMED },
  { "lock.unlock", nullptr               , OZ_DIR_UP   ,  10, 0x00, OZ_DP_RESERVED },
  { "lock.unlock", "remote"              , OZ_DIR_DOWN ,  10, 0x00, OZ_DP_RESERVED },
};

// tuya-wifi-lock-pro — 42 DPs
static const OzDpEntry OZ_DP_tuya_wifi_lock_pro[] = {
  {   1, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_fingerprint" },
  {   2, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_password" },
  {   3, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_temporary" },
  {   5, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_card" },
  {   6, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_face" },
  {   7, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_key" },
  {   8, OZ_DIR_UP  , OZ_DP_CONFIRMED , "alert_records" },
  {   9, OZ_DIR_UP  , OZ_DP_CONFIRMED , "remote_unlock_request" },
  {  10, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "arm_away" },
  {  11, OZ_DIR_UP  , OZ_DP_CONFIRMED , "battery_state" },
  {  12, OZ_DIR_UP  , OZ_DP_CONFIRMED , "battery_percentage" },
  {  13, OZ_DIR_UP  , OZ_DP_CONFIRMED , "double_lock_state" },
  {  14, OZ_DIR_UP  , OZ_DP_CONFIRMED , "child_lock" },
  {  15, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_remote_app" },
  {  16, OZ_DIR_UP  , OZ_DP_CONFIRMED , "duress_alarm" },
  {  17, OZ_DIR_UP  , OZ_DP_CONFIRMED , "unlock_from_inside" },
  {  18, OZ_DIR_UP  , OZ_DP_CONFIRMED , "door_state" },
  {  19, OZ_DIR_UP  , OZ_DP_CONFIRMED , "doorbell_call" },
  {  21, OZ_DIR_UP  , OZ_DP_CONFIRMED , "liftup_double_lock" },
  {  25, OZ_DIR_UP  , OZ_DP_RESERVED  , "sync_fingerprints" },
  {  26, OZ_DIR_UP  , OZ_DP_RESERVED  , "sync_codes" },
  {  27, OZ_DIR_UP  , OZ_DP_RESERVED  , "sync_cards" },
  {  32, OZ_DIR_UP  , OZ_DP_RESERVED  , "offline_password_unlock_report" },
  {  33, OZ_DIR_UP  , OZ_DP_RESERVED  , "offline_password_clear_report" },
  {  34, OZ_DIR_UP  , OZ_DP_CONFIRMED , "query_added_method" },
  {  35, OZ_DIR_DOWN, OZ_DP_CONFIRMED , "added_method" },
  {  37, OZ_DIR_DOWN, OZ_DP_CONFIRMED , "added_response" },
  {  38, OZ_DIR_UP  , OZ_DP_CONFIRMED , "remote_delete_allowed" },
  {  39, OZ_DIR_DOWN, OZ_DP_CONFIRMED , "remote_delete_fingerprint" },
  {  40, OZ_DIR_UP  , OZ_DP_CONFIRMED , "remote_delete_response" },
  {  41, OZ_DIR_DOWN, OZ_DP_CONFIRMED , "remote_delete_code" },
  {  43, OZ_DIR_DOWN, OZ_DP_CONFIRMED , "remote_delete_card" },
  {  44, OZ_DIR_UP  , OZ_DP_RESERVED  , "unlock_combination_record" },
  {  48, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "forced_double_lock" },
  {  49, OZ_DIR_BOTH, OZ_DP_RESERVED  , "remote_unlock_key_config" },
  {  50, OZ_DIR_BOTH, OZ_DP_RESERVED  , "remote_unlock" },
  {  55, OZ_DIR_UP  , OZ_DP_CONFIRMED , "lock_state" },
  {  57, OZ_DIR_UP  , OZ_DP_RESERVED  , "locking_records" },
  {  58, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "lock_always_on" },
  {  68, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "auto_lock" },
  {  69, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "auto_lock_delay" },
  {  76, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "fill_light" },
};

static const OzVerbMap OZ_VERBS_tuya_wifi_lock_pro[] = {
  { "cred.delete", "card"                , OZ_DIR_DOWN ,  43, 0x02, OZ_DP_CONFIRMED },
  { "cred.delete", "fingerprint"         , OZ_DIR_DOWN ,  39, 0x02, OZ_DP_CONFIRMED },
  { "cred.delete", "pin"                 , OZ_DIR_DOWN ,  41, 0x02, OZ_DP_CONFIRMED },
  { "cred.put", "method"              , OZ_DIR_DOWN ,  35, 0x04, OZ_DP_CONFIRMED },
  { "cred.put", "response"            , OZ_DIR_DOWN ,  37, 0x01, OZ_DP_CONFIRMED },
  { "cred.put", "remote_key"          , OZ_DIR_DOWN ,  49, 0x00, OZ_DP_RESERVED },
  { "cred.query", nullptr               , OZ_DIR_UP   ,  34, 0x01, OZ_DP_CONFIRMED },
  { "cred.sync", "card"                , OZ_DIR_UP   ,  27, 0x00, OZ_DP_RESERVED },
  { "cred.sync", "fingerprint"         , OZ_DIR_UP   ,  25, 0x00, OZ_DP_RESERVED },
  { "cred.sync", "pin"                 , OZ_DIR_UP   ,  26, 0x00, OZ_DP_RESERVED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   1, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   2, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   3, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   5, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   6, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "hw_id"               , OZ_DIR_UP   ,   7, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "member_id"           , OZ_DIR_UP   ,  15, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "combination"         , OZ_DIR_UP   ,  44, 0x00, OZ_DP_RESERVED },
  { "event.access", "locking"             , OZ_DIR_UP   ,  57, 0x00, OZ_DP_RESERVED },
  { "event.access", "offline_pin"         , OZ_DIR_UP   ,  32, 0x00, OZ_DP_RESERVED },
  { "event.alarm", "type"                , OZ_DIR_UP   ,   8, 0x04, OZ_DP_CONFIRMED },
  { "event.always_on", nullptr               , OZ_DIR_UP   ,  58, 0x01, OZ_DP_CONFIRMED },
  { "event.armed", nullptr               , OZ_DIR_UP   ,  10, 0x01, OZ_DP_CONFIRMED },
  { "event.autolock", nullptr               , OZ_DIR_UP   ,  68, 0x01, OZ_DP_CONFIRMED },
  { "event.autolock", "delay"               , OZ_DIR_UP   ,  69, 0x02, OZ_DP_CONFIRMED },
  { "event.battery", "level"               , OZ_DIR_UP   ,  11, 0x04, OZ_DP_CONFIRMED },
  { "event.battery", "percent"             , OZ_DIR_UP   ,  12, 0x02, OZ_DP_CONFIRMED },
  { "event.bolt", "double_locked"       , OZ_DIR_UP   ,  13, 0x01, OZ_DP_CONFIRMED },
  { "event.bolt", "forced"              , OZ_DIR_UP   ,  48, 0x01, OZ_DP_CONFIRMED },
  { "event.bolt", "liftup"              , OZ_DIR_UP   ,  21, 0x01, OZ_DP_CONFIRMED },
  { "event.bolt", "locked"              , OZ_DIR_UP   ,  55, 0x01, OZ_DP_CONFIRMED },
  { "event.child_lock", nullptr               , OZ_DIR_UP   ,  14, 0x01, OZ_DP_CONFIRMED },
  { "event.cred_cleared", "response"            , OZ_DIR_UP   ,  40, 0x01, OZ_DP_CONFIRMED },
  { "event.cred_cleared", nullptr               , OZ_DIR_UP   ,  33, 0x00, OZ_DP_RESERVED },
  { "event.cred_policy", nullptr               , OZ_DIR_UP   ,  38, 0x01, OZ_DP_CONFIRMED },
  { "event.door", nullptr               , OZ_DIR_UP   ,  18, 0x04, OZ_DP_CONFIRMED },
  { "event.doorbell", nullptr               , OZ_DIR_UP   ,  19, 0x01, OZ_DP_CONFIRMED },
  { "event.duress", nullptr               , OZ_DIR_UP   ,  16, 0x01, OZ_DP_CONFIRMED },
  { "event.fill_light", nullptr               , OZ_DIR_UP   ,  76, 0x01, OZ_DP_CONFIRMED },
  { "event.inside_open", nullptr               , OZ_DIR_UP   ,  17, 0x01, OZ_DP_CONFIRMED },
  { "event.unlock_request", "countdown"           , OZ_DIR_UP   ,   9, 0x02, OZ_DP_CONFIRMED },
  { "lock.settings.set", "always_on"           , OZ_DIR_DOWN ,  58, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "arm_away"            , OZ_DIR_DOWN ,  10, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock"            , OZ_DIR_DOWN ,  68, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "autolock_delay"      , OZ_DIR_DOWN ,  69, 0x02, OZ_DP_CONFIRMED },
  { "lock.settings.set", "fill_light"          , OZ_DIR_DOWN ,  76, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "forced_double_lock"  , OZ_DIR_DOWN ,  48, 0x01, OZ_DP_CONFIRMED },
  { "lock.unlock", "remote"              , OZ_DIR_DOWN ,  50, 0x00, OZ_DP_RESERVED },
};

static const OzProfile OZ_PROFILES[] = {
  { "tuya-ladin-f7-t3", 1, OZ_DP_tuya_ladin_f7_t3, (uint16_t)(sizeof(OZ_DP_tuya_ladin_f7_t3) / sizeof(OzDpEntry)), false, nullptr, OZ_VERBS_tuya_ladin_f7_t3, 2 },
  { "tuya-luona-ds013-t3", 2, OZ_DP_tuya_luona_ds013_t3, (uint16_t)(sizeof(OZ_DP_tuya_luona_ds013_t3) / sizeof(OzDpEntry)), false, "vr4iiuqtyh0q4nix", OZ_VERBS_tuya_luona_ds013_t3, 54 },
  { "tuya-wifi-lock-pro", 1, OZ_DP_tuya_wifi_lock_pro, (uint16_t)(sizeof(OZ_DP_tuya_wifi_lock_pro) / sizeof(OzDpEntry)), false, nullptr, OZ_VERBS_tuya_wifi_lock_pro, 48 },
};
static const uint8_t OZ_PROFILE_COUNT = 3;

// The profile the firmware boots with when the build did not pin one.
//
// 🔴 AN UNPINNED BUILD IS NOW A CONFIGURATION ERROR, not a normal case.
// It used to default to `ozkie-legacy-v0`, our invented map, which was
// deleted 2026-08-20 — every lock now runs a REAL per-product profile.
//
// Tuya's own Wi-Fi Lock Pro map is the least-wrong default: it is a real
// published standard rather than a fiction. It is still a GUESS for any
// specific lock — DP 76 is `fill_light` here and `unlock_ble` on Luona —
// so ozProfileBegin() says so loudly at boot. Pin with PROFILE=.
#define OZ_PROFILE_DEFAULT_ID "tuya-wifi-lock-pro"

