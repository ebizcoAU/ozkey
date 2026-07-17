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

**One on-device method, every screen: tap `*` then `5`** (instant, no hold).
`*` on an empty PIN entry shows "RESET? 5=Y"; `5` wipes NVS (config + PINs)
→ reboot to ADVERTISING; any other key cancels. `*` with digits typed just
clears them, so normal PIN retries can't trip it. On the ADVERTISING /
CONNECTING screens the keys aren't drawn but the same touch zones apply
(right-half keypad area: `*` bottom-left, `5` centre).

Separately, removing the lock in BANOI ("Gỡ khoá") makes OZLOCK publish
`{op:"factory_reset"}` on the command topic — an ONLINE lock resets itself;
an offline one needs the `*5` tap.

## B6 — MAOI hotel mode (`mode=ozkey-local`, OZKEYSERV :3200)

Same firmware, different ceremony: no enroll handshake — the lock announces
itself as an UNPAIRED hotel lock and MAOI binds it to a room.

1. Prereqs: Mosquitto `*:1883` + **ozkeyserv `:3200`** (site `hotel`) running;
   cockpit `:3300` optional (observer).
2. Factory-fresh lock (`*5`) → ADVERTISING. MAOI → Hồ sơ DN → **Quản lý khoá
   cửa** → amber "Phát hiện khoá OZLOCK" banner → sheet asks WiFi SSID/pass
   only (host comes from the OZKEYSERV config) → ladder to BROKER_OK, then
   the sheet waits for the MAC in `/locks/unpaired` (≤2 min).
3. Serial: `[PAIR->] unpaired announce (waiting for room assign)` every 20s;
   ozkeyserv logs `Discovered unprovisioned lock <mac> on MQTT`.
4. Bind: Kho → Phòng & Giá → unpaired-lock banner → gắn vào phòng (or the
   room editor "Gắn khoá"). Server sends `provision_assign`; serial shows
   `[PAIR] assigned room <n> (site hotel)` and the lock's strip shows `P.<n>`.
5. Keys: check-in a booking (Đặt Phòng folio) → auto door-PIN (4 digits) →
   `[CRED] slot N stored pin='…'` on the next 30s heartbeat → type PIN + `#`
   → UNLOCKED + ozkeyserv logs the door transaction against the room.
6. Checkout/settle revokes; Quản lý khoá cửa issues master PINs + remote ops.

## Known v0 limits

- v1 plaintext provisioning (bench only) — v2 envelope + trust anchor deferred.
- Broker creds from `enrollment_ack` are stored but unused (lab broker is open).
- Validity windows enforced only once NTP syncs (else PINs accepted).
