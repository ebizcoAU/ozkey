#!/usr/bin/env python3
"""ozprov — raw BLE provision writer for OZLOCK doorlock testing.

App-INDEPENDENT. Talks straight to the doorlock's GATT provision characteristic
so we can exercise the firmware's own refusal paths (bond #0 accept/deny,
malformed app_id, atomic reject) without BANOI deciding for us. The app declining
to send a payload — as BANOI #2 did on 2026-08-02 — is UX, not a security control;
this is how we regression-test the control that actually matters.

The lock only advertises inside a (R) window: short-press BOOT on the lock first,
LCD shows "BLE open 60s", then run this within 60 s.

GATT (doorlock.ino:127-131):
  SVC  4f5a4b31-0001-4c4f-434b-000000000001
  PROV 4f5a4b31-0002-4c4f-434b-000000000001  (WRITE)   <- payload goes here
  STAT 4f5a4b31-0003-4c4f-434b-000000000001  (NOTIFY)  <- ladder comes back here
  INFO 4f5a4b31-0004-4c4f-434b-000000000001  (READ)    <- device_id, pub, fw

Firmware reassembles chunks until the JSON parses (doorlock.ino:1466-1475): first
chunk must start with '{'. We write ATT-sized chunks with response so ordering holds.

Usage:
  ozprov.py info                         # connect, dump INFO, disconnect (no write)
  ozprov.py wifi   --app-id <64hex> [--ssid S --pass P --broker H:P]
  ozprov.py thread --app-id <64hex> --net-key <64hex> --ext-pan <16hex> \
                   --net-name N --channel 15 --pan-id 0xNNNN [--broker H:P]
  ozprov.py raw '<json>'                 # send an arbitrary payload verbatim
  ozprov.py malformed                    # app_id present but not 64 hex -> reject path

Common options:
  --name NAME (adv name to match, default OZLOCK) | --addr AA:BB:.. (skip scan)
  --scan-secs N (default 8) | --chunk N (default 20) | --settle N (default 4)

Exit 0 if a terminal status was observed, 2 if none arrived, 1 on connect failure.
"""
import argparse, asyncio, json, sys, time
from bleak import BleakScanner, BleakClient

SVC  = "4f5a4b31-0001-4c4f-434b-000000000001"
PROV = "4f5a4b31-0002-4c4f-434b-000000000001"
STAT = "4f5a4b31-0003-4c4f-434b-000000000001"
INFO = "4f5a4b31-0004-4c4f-434b-000000000001"

# Ladder strings that end an exchange one way or the other (doorlock.ino).
TERMINAL = {
    "BOND_OK": "accepted", "BOND_DENIED": "REFUSED (bond #0 held by another app_id)",
    "PAYLOAD_REJECTED": "REFUSED (malformed / bad payload)",
    "WIFI_OK": "wifi joined", "THREAD_OK": "thread attached",
    "WIFI_FAIL": "wifi failed", "THREAD_FAIL": "thread failed",
}

def log(tag, msg): print(f"{time.strftime('%H:%M:%S')}  {tag:8} {msg}", flush=True)

async def find(name, addr, scan_secs):
    if addr:
        log("SCAN", f"using --addr {addr}, skipping scan")
        return addr
    log("SCAN", f"looking for '{name}' for {scan_secs}s ...")
    d = await BleakScanner.find_device_by_filter(
        lambda dev, adv: (adv.local_name == name)
        or (SVC in (adv.service_uuids or [])),
        timeout=scan_secs,
    )
    if not d:
        log("SCAN", "NOT FOUND — is the (R) window open? short-press BOOT on the lock.")
        return None
    log("SCAN", f"found {d.address}  rssi-name='{name}'")
    return d.address

async def run(args, payload):
    addr = await find(args.name, args.addr, args.scan_secs)
    if not addr:
        return 1
    got = {"terminal": None, "lines": []}

    def on_status(_h, data: bytearray):
        s = data.decode("utf-8", "replace").strip()
        got["lines"].append(s)
        verdict = TERMINAL.get(s)
        log("<=STAT", s + (f"   << {verdict}" if verdict else ""))
        if verdict:
            got["terminal"] = s

    log("CONN", f"connecting to {addr} ...")
    try:
        async with BleakClient(addr, timeout=15) as cli:
            log("CONN", "connected")
            # INFO first — always safe, and proves the read-without-auth surface.
            try:
                raw = await cli.read_gatt_char(INFO)
                try:
                    info = json.loads(raw.decode())
                    log("=>INFO", json.dumps(info, separators=(",", ":")))
                except Exception:
                    log("=>INFO", raw.hex())
            except Exception as e:
                log("=>INFO", f"(read failed: {e})")

            if payload is None:
                log("DONE", "info-only, no write")
                return 0

            try:
                await cli.start_notify(STAT, on_status)
            except Exception as e:
                log("STAT", f"(notify subscribe failed: {e} — will write blind)")

            blob = json.dumps(payload, separators=(",", ":")).encode()
            n = args.chunk
            chunks = [blob[i:i + n] for i in range(0, len(blob), n)]
            log("=>PROV", f"{len(blob)} bytes in {len(chunks)} chunk(s): {payload}")
            for i, ch in enumerate(chunks):
                await cli.write_gatt_char(PROV, ch, response=True)
                await asyncio.sleep(0.05)
            log("=>PROV", "payload written, waiting for ladder ...")

            deadline = time.time() + args.settle
            while time.time() < deadline and got["terminal"] is None:
                await asyncio.sleep(0.1)

            if got["terminal"] is None:
                log("RESULT", "NO terminal status within settle window")
                log("RESULT", f"lines seen: {got['lines'] or '(none)'}")
                return 2
            log("RESULT", f"terminal = {got['terminal']}  ({TERMINAL[got['terminal']]})")
            return 0
    except Exception as e:
        log("CONN", f"FAILED: {e}")
        return 1

def build_payload(a):
    if a.cmd == "info":
        return None
    if a.cmd == "raw":
        return json.loads(a.json)
    if a.cmd == "malformed":
        # app_id present but not 64 hex -> ozBond0Evaluate -> OZ_BOND_MALFORMED
        return {"app_id": "not-a-real-key", "mode": a.mode, "site_id": a.site,
                "ssid": "x", "network_key": ""}
    p = {"app_id": a.app_id, "mode": a.mode, "site_id": a.site}
    if a.name:   p["name"] = a.name
    if a.broker:
        host, _, port = a.broker.partition(":")
        p["broker_host"] = host
        p["broker_tcp_port"] = int(port or 1883)
    if a.cmd == "wifi":
        p["ssid"] = a.ssid
        if a.password is not None: p["password"] = a.password  # firmware reads doc["password"] (doorlock.ino:1422)
        p["server_ip"] = a.server_ip; p["server_port"] = a.server_port
    elif a.cmd == "thread":
        p["network_key"] = a.net_key          # presence of this selects the Thread arm
        p["network_name"] = a.net_name
        p["ext_pan_id"] = a.ext_pan
        p["channel"] = a.channel
        p["pan_id"] = int(a.pan_id, 0)
    return p

def main():
    ap = argparse.ArgumentParser(description="raw BLE provision writer for OZLOCK")
    ap.add_argument("cmd", choices=["info", "wifi", "thread", "raw", "malformed"])
    ap.add_argument("json", nargs="?", help="for 'raw': the JSON payload")
    ap.add_argument("--app-id", default="")
    ap.add_argument("--mode", default="ozkey-cloud")
    ap.add_argument("--site", default="lab")
    ap.add_argument("--name", default=None, help="provision 'name' field")
    ap.add_argument("--broker", default=None, help="broker HOST:PORT")
    # wifi
    ap.add_argument("--ssid", default="PHAN")
    ap.add_argument("--pass", dest="password", default=None)
    ap.add_argument("--server-ip", default="10.1.1.20")
    ap.add_argument("--server-port", type=int, default=4200)
    # thread
    ap.add_argument("--net-key", dest="net_key", default="")
    ap.add_argument("--ext-pan", dest="ext_pan", default="")
    ap.add_argument("--net-name", dest="net_name", default="OZ-TEST")
    ap.add_argument("--channel", type=int, default=15)
    ap.add_argument("--pan-id", dest="pan_id", default="0x1234")
    # ble
    ap.add_argument("--addr", default=None, help="BLE address, skip scan")
    ap.add_argument("--adv-name", dest="adv_name", default="OZLOCK")
    ap.add_argument("--scan-secs", dest="scan_secs", type=int, default=8)
    ap.add_argument("--chunk", type=int, default=20)
    ap.add_argument("--settle", type=int, default=4)
    a = ap.parse_args()
    a.name_field = a.name
    a.name = a.adv_name  # find() matches on advertised name
    payload = build_payload(a)
    rc = asyncio.run(run(a, payload))
    sys.exit(rc)

if __name__ == "__main__":
    main()
