# blelock v0 — flash & bench-test guide (ozkey-08 §10.7 milestones)

## Flash

**Arduino IDE**: open `blelock/blelock.ino`, board **ESP32C6 Dev Module**, set
**Flash Size: 8MB** + **Partition Scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"**
(sketch is 1.48MB — the default 4MB/1.2MB-app scheme is too small), then Upload.

**CLI** (same result) — one-shot script: `blelock/flash.sh [4M|8M|16M] [sketch-dir]`
(defaults: 8M, this sketch; auto-picks `/dev/cu.usbmodem*`, ends in the serial
monitor, Ctrl+C to exit). Or by hand:
```sh
arduino-cli compile --fqbn "esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB" \
  ~/Documents/Dev/ozkey/blelock/blelock
arduino-cli upload  --fqbn "esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB" \
  -p /dev/cu.usbmodem*  ~/Documents/Dev/ozkey/blelock/blelock
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200
```

Prereqs: Mosquitto (`*:1883`) + ozlockserv `:4200` running (`node --watch`).

## B1 — BLE + GATT (prove with nRF Connect on a phone)

1. Boot → screen shows **OZLOCK** + `device_id ozk-<machex>` + "Mo BANOI…".
2. nRF Connect → scan → connect **OZLOCK** → service `4f5a4b31-0001-…`.
3. Read `…0004` (info) → `{"device_id":"ozk-…","mac":"…","fw":"blelock-0.1"}`.
4. Subscribe `…0003` (status) → shows `BLE_OK` on connect.

## B2 — provision → Wi-Fi → MQTT → ENROLLED

1. **Register the pairing first** (BANOI's job in B4; curl for the bench —
   device_id from the lock's screen):
```sh
curl -s -X POST http://10.1.1.21:4200/ozlock/api/pairings \
  -H 'Content-Type: application/json' \
  -d '{"app_id":"bench-curl","device_id":"ozk-<machex>","label":"Cua truoc"}'
```
2. nRF Connect → write to `…0002` (provision), **UTF-8 text** (one write; if
   your phone caps write length, send it in chunks — any chunk starting `{`
   restarts the buffer):
```json
{"v":1,"mode":"ozkey-cloud","ssid":"<SSID>","password":"<PASS>","broker_host":"10.1.1.21","broker_tcp_port":1883,"server_ip":"10.1.1.21","server_port":4200,"device_id":"ozk-<machex>","site_id":"lab","name":"Cua truoc","heartbeat_s":60}
```
3. Watch status notifies: `WIFI_JOINING → WIFI_OK → BROKER_JOINING → BROKER_OK → ENROLLED`;
   screen shows **name + IP** during joining, then flips to the **keypad**.
4. ozlockserv terminal: `ENROLLED ozk-… (…mac…) -> site 'lab' as "Cua truoc"`.
5. Reboot the board → it reconnects straight to OPERATIONAL from NVS (no BLE).

## B3 — KEYPIN → keypad unlock → log

1. Issue a PIN (what BANOI's Cấp mã calls):
```sh
curl -s -X POST http://10.1.1.21:4200/ozlock/api/locks/ozk-<machex>/grants \
  -H 'Content-Type: application/json' \
  -d '{"user_name":"Bench Guest","type":"pin","raw_value":"2468","slot_number":1}'
```
2. The queued frame flushes on the next heartbeat (≤60 s; serial shows
   `[MQTT<-] … payload_hex` then `[CRED] slot 1 stored`).
3. Keypad: type `2468` `#` → **MO CUA** (blue, 5 s auto-relock) → ozlockserv
   log: `Door GRANTED — PIN slot 1 @ "Cua truoc"`.
4. Wrong PIN ×1 → `SAI MA` + denied log; ×5 → 60 s lockout screen.
5. Revoke: `curl -X DELETE .../locks/ozk-<machex>/grants/<gid>` → next
   heartbeat → `[CRED] slot 1 revoked` → PIN no longer opens.
6. Remote unlock: `curl -X POST .../locks/ozk-<machex>/unlock` → flushes on
   heartbeat → MO CUA.

## Factory reset

Three ways, all wipe NVS (config + PINs) → reboot to ADVERTISING:

1. **From BANOI** — "Gỡ khoá khỏi BANOI": the app calls `DELETE /locks/:id`,
   OZLOCK publishes `{op:"factory_reset"}` on the command topic, an ONLINE
   lock resets itself (an offline lock misses it — use the keypad way).
2. **Keypad (OPERATIONAL)** — press **#** with an empty PIN → "RESET? 5=Y"
   → press **5** = instant reset. Any other key cancels. (No more 5 s hold.)
3. **Any other screen** — hold anywhere 10 s (escape hatch).

## Known v0 limits

- v1 plaintext provisioning (bench only) — v2 envelope + trust anchor deferred.
- Broker creds from `enrollment_ack` are stored but unused (lab broker is open).
- Validity windows enforced only once NTP syncs (else PINs accepted).
