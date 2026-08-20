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

// ozkie-legacy-v0 — 10 DPs — DEPRECATED (invented map)
static const OzDpEntry OZ_DP_ozkie_legacy_v0[] = {
  {   1, OZ_DIR_BOTH, OZ_DP_FICTION   , "unlock_channel" },
  {   2, OZ_DIR_UP  , OZ_DP_FICTION   , "rfid_card" },
  {   3, OZ_DIR_UP  , OZ_DP_FICTION   , "fingerprint" },
  {   5, OZ_DIR_UP  , OZ_DP_FICTION   , "battery_alarm" },
  {   8, OZ_DIR_UP  , OZ_DP_FICTION   , "access_result" },
  {  21, OZ_DIR_DOWN, OZ_DP_FICTION   , "add_temp_pin" },
  {  22, OZ_DIR_DOWN, OZ_DP_FICTION   , "delete_pin" },
  {  23, OZ_DIR_DOWN, OZ_DP_FICTION   , "add_temp_rfid" },
  {  24, OZ_DIR_DOWN, OZ_DP_FICTION   , "delete_rfid" },
  {  60, OZ_DIR_UP  , OZ_DP_FICTION   , "pairing_request_proposed" },
};

static const OzVerbMap OZ_VERBS_ozkie_legacy_v0[] = {
  { "cred.delete", "pin"                 , OZ_DIR_DOWN ,  22, 0x00, OZ_DP_FICTION },
  { "cred.delete", "pin"                 , OZ_DIR_UP   ,  22, 0x00, OZ_DP_FICTION },
  { "cred.delete", "rfid"                , OZ_DIR_DOWN ,  24, 0x00, OZ_DP_FICTION },
  { "cred.delete", "rfid"                , OZ_DIR_UP   ,  24, 0x00, OZ_DP_FICTION },
  { "cred.put", "pin"                 , OZ_DIR_DOWN ,  21, 0x00, OZ_DP_FICTION },
  { "cred.put", "pin"                 , OZ_DIR_UP   ,  21, 0x00, OZ_DP_FICTION },
  { "cred.put", "rfid"                , OZ_DIR_DOWN ,  23, 0x00, OZ_DP_FICTION },
  { "cred.put", "rfid"                , OZ_DIR_UP   ,  23, 0x00, OZ_DP_FICTION },
  { "event.access", "result"              , OZ_DIR_UP   ,   8, 0x04, OZ_DP_FICTION },
  { "event.battery", nullptr               , OZ_DIR_UP   ,   5, 0x01, OZ_DP_FICTION },
  { "lock.unlock", nullptr               , OZ_DIR_DOWN ,   1, 0x01, OZ_DP_FICTION },
  { "lock.unlock", nullptr               , OZ_DIR_UP   ,   1, 0x01, OZ_DP_FICTION },
};

// ozsim-fullfeature — 8 DPs
static const OzDpEntry OZ_DP_ozsim_fullfeature[] = {
  {   1, OZ_DIR_BOTH, OZ_DP_FICTION   , "unlock_channel" },
  {  53, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "doorbell" },
  {  61, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_password" },
  {  63, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_fingerprint" },
  {  64, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_card" },
  {  69, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_temporary" },
  {  72, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_remote" },
  {  76, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_ble" },
};

static const OzVerbMap OZ_VERBS_ozsim_fullfeature[] = {
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  61, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  63, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  64, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  69, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  72, 0x02, OZ_DP_CONFIRMED },
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  76, 0x02, OZ_DP_CONFIRMED },
  { "event.doorbell", nullptr               , OZ_DIR_UP   ,  53, 0x01, OZ_DP_CONFIRMED },
  { "lock.unlock", "ble"                 , OZ_DIR_DOWN ,  76, 0x02, OZ_DP_CONFIRMED },
  { "lock.unlock", nullptr               , OZ_DIR_UP   ,   1, 0x01, OZ_DP_FICTION },
};

// tuya-ds013-t3 — 34 DPs
static const OzDpEntry OZ_DP_tuya_ds013_t3[] = {
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

static const OzVerbMap OZ_VERBS_tuya_ds013_t3[] = {
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

// tuya-generic-lock — 34 DPs
static const OzDpEntry OZ_DP_tuya_generic_lock[] = {
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

static const OzVerbMap OZ_VERBS_tuya_generic_lock[] = {
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

// tuya-t3-videolock — 4 DPs
static const OzDpEntry OZ_DP_tuya_t3_videolock[] = {
  {  42, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "ble_switch" },
  {  76, OZ_DIR_BOTH, OZ_DP_CONFIRMED , "unlock_ble" },
  { 149, OZ_DIR_BOTH, OZ_DP_UNKNOWN   , "reserved_do_not_select" },
  { 212, OZ_DIR_BOTH, OZ_DP_UNKNOWN   , "initiative_message" },
};

static const OzVerbMap OZ_VERBS_tuya_t3_videolock[] = {
  { "event.access", "cred_id"             , OZ_DIR_UP   ,  76, 0x02, OZ_DP_CONFIRMED },
  { "lock.settings.set", "ble_enabled"         , OZ_DIR_DOWN ,  42, 0x01, OZ_DP_CONFIRMED },
  { "lock.settings.set", "ble_enabled"         , OZ_DIR_UP   ,  42, 0x01, OZ_DP_CONFIRMED },
  { "lock.unlock", "ble"                 , OZ_DIR_DOWN ,  76, 0x02, OZ_DP_CONFIRMED },
};

static const OzProfile OZ_PROFILES[] = {
  { "ozkie-legacy-v0", 2, OZ_DP_ozkie_legacy_v0, (uint16_t)(sizeof(OZ_DP_ozkie_legacy_v0) / sizeof(OzDpEntry)), true, nullptr, OZ_VERBS_ozkie_legacy_v0, 12 },
  { "ozsim-fullfeature", 1, OZ_DP_ozsim_fullfeature, (uint16_t)(sizeof(OZ_DP_ozsim_fullfeature) / sizeof(OzDpEntry)), false, "ozsimfullfeature", OZ_VERBS_ozsim_fullfeature, 9 },
  { "tuya-ds013-t3", 1, OZ_DP_tuya_ds013_t3, (uint16_t)(sizeof(OZ_DP_tuya_ds013_t3) / sizeof(OzDpEntry)), false, "vr4iiuqtyh0q4nix", OZ_VERBS_tuya_ds013_t3, 54 },
  { "tuya-generic-lock", 1, OZ_DP_tuya_generic_lock, (uint16_t)(sizeof(OZ_DP_tuya_generic_lock) / sizeof(OzDpEntry)), false, nullptr, OZ_VERBS_tuya_generic_lock, 54 },
  { "tuya-t3-videolock", 1, OZ_DP_tuya_t3_videolock, (uint16_t)(sizeof(OZ_DP_tuya_t3_videolock) / sizeof(OzDpEntry)), false, "3zlhjdesga1kyy75", OZ_VERBS_tuya_t3_videolock, 4 },
};
static const uint8_t OZ_PROFILE_COUNT = 5;

// The profile the firmware boots with.
//
// 🔴 DELIBERATELY the invented map: doorlock-1.58 ships it and the current
// BANOI build constructs DP 21 frames against it, so defaulting to the real
// catalogue would break the bench and the app on the next flash. Staged
// migration, no flag day (ozkey-28 §1.1).
#define OZ_PROFILE_DEFAULT_ID "ozkie-legacy-v0"

