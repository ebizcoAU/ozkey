#!/usr/bin/env python3
"""relayrun.py — F1 relay-only repeatability (ozkey-55 F1, PM nexus-6e 2026-11-06 (b)).

  python3 -I relayrun.py <lock device_id> <lock serial port> <bridge_id> <N> <log prefix> [broker] [site]

Pairs at the door ONCE over direct BLE (the only Mac-BLE step), then repeats
ONLY the relay phase N times on the claimed lock: no wipe, no serial reopen,
no Mac BLE. Every relay round is a fresh EDHOC + unlock-with-Echo + state read
through MQTT → bridge → `relay` characteristic.

Writes, next to <log prefix>:
  <prefix>.log         the round-by-round result
  <prefix>_lock.log    the FULL lock serial, timestamped (one open for the whole run)
  <prefix>_mqtt.log    every downlink/uplink this script saw, timestamped, with
                       uplinks that arrive AFTER a round gave up marked LATE

Precondition: the lock is UNCLAIMED. The bridge may hold it (rev 6) or not.

Environment (all optional):
  BRIDGE_CONSOLE=<port>  unpair the lock on the bridge before the door, pair it
                         after (the rev 6 rig workaround; not needed from
                         bridgeBLE-0.47, which releases an unclaimed lock itself)
  BRIDGE_BOND=1          enrol the bridge's key (its retained presence
                         "ozkie_pub") as a `bridge` bond at the door, label =
                         the bridge id (F1-BRIDGE-BOND-1, rev 7)
  DOOR_AFTER_REBOOT=K    after the relay rounds, K times: reset the lock (serial
                         reopen), wait for the bridge to hold it again, then the
                         owner connects AT THE DOOR over direct BLE and reads
                         lock/state. The rev 7 acceptance check: an idle bridge
                         must not keep the app out.
"""
import os, queue, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ozapp import OzApp, R, uncbor                    # noqa: E402
from blelink import BleLockLink                       # noqa: E402
from mqttrun import PresenceWatch                     # noqa: E402


def ts():
    t = time.time()
    return time.strftime('%H:%M:%S', time.localtime(t)) + f'.{int(t * 1000) % 1000:03d}'


class TeeSerial:
    """One pyserial open for the whole run. A reader thread logs every line with
    a timestamp; blelink still gets read()/write()/reset_input_buffer()."""
    def __init__(self, port, path):
        import serial as pyserial
        self.s = pyserial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.1
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.f = open(path, 'a', buffering=1)
        self.buf, self.lock, self.stop = bytearray(), threading.Lock(), False
        self.t = threading.Thread(target=self._run, daemon=True); self.t.start()

    def _run(self):
        line = b''
        while not self.stop:
            try:
                b = self.s.read(256)
            except Exception as e:                       # noqa: BLE001
                self.f.write(f'{ts()} [tee] serial read error {e!r}\n'); time.sleep(0.5); continue
            if not b:
                continue
            with self.lock:
                self.buf += b
            line += b
            while b'\n' in line:
                one, line = line.split(b'\n', 1)
                self.f.write(f"{ts()} {one.decode('utf-8', 'replace').rstrip()}\n")

    def read(self, n=256):
        end = time.time() + self.s.timeout
        while time.time() < end:
            with self.lock:
                if self.buf:
                    out = bytes(self.buf[:n]); del self.buf[:n]; return out
            time.sleep(0.01)
        return b''

    def write(self, data):
        self.f.write(f'{ts()} [tee] >> {data!r}\n')
        return self.s.write(data)

    def reset_input_buffer(self):
        with self.lock:
            self.buf.clear()

    def mark(self, text):
        self.f.write(f'{ts()} [mark] {text}\n')

    def close(self):
        self.stop = True; self.t.join(1); self.s.close(); self.f.close()


class LoggedRelayLink:
    """mqttrun.MqttRelayLink with every message logged, and with uplinks kept
    for the whole run so an answer that comes after a round gave up is seen."""
    def __init__(self, host, site, bridge, lock_id, path):
        import paho.mqtt.client as mqtt
        self.down = f'ozkie/{site}/bridges/{bridge}/ozkie/{lock_id}'
        self.up = f'ozkie/{site}/locks/{lock_id}/ozkie/{bridge}'
        self.q, self.f, self.round = queue.Queue(), open(path, 'a', buffering=1), 0
        self.waiting = False
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'relayrun-{os.getpid()}')
        self.c.on_message = self._on
        self.c.connect(host, 1883, 30)
        self.c.subscribe(self.up, qos=0)
        self.c.loop_start()
        time.sleep(0.5)

    def _on(self, c, u, m):
        p = bytes(m.payload)
        self.f.write(f"{ts()} r{self.round} UP   type {p[0] if p else -1} {len(p) - 1} B"
                     f"{'' if self.waiting else '  LATE (no round waiting)'}\n")
        self.q.put(p)

    def raw(self, payload):
        self.f.write(f'{ts()} r{self.round} DOWN type {payload[0]} {len(payload) - 1} B\n')
        self.c.publish(self.down, payload, qos=0, retain=False).wait_for_publish(5)

    def send(self, typ, p):
        self.raw(bytes([typ]) + p)

    def recv(self, timeout=2.0):
        self.waiting = True
        try:
            m = self.q.get(timeout=timeout)
        except queue.Empty:
            return None, None
        finally:
            self.waiting = False
        return m[0], m[1:]

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.c.loop_stop(); self.c.disconnect(); self.f.close()


def bridge_pub(host, site, bridge, timeout=5.0):
    """The bridge's ozkie_pub from its retained presence (lab: the rig stands in
    for the app, which reads it from the bridge's BLE info and checks)."""
    import json
    import paho.mqtt.client as mqtt
    got = queue.Queue()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'relayrun-b-{os.getpid()}')
    c.on_message = lambda cl, u, m: got.put(bytes(m.payload))
    c.connect(host, 1883, 30)
    c.subscribe(f'ozkie/{site}/bridges/{bridge}/presence', qos=0)
    c.loop_start()
    try:
        d = json.loads(got.get(timeout=timeout))
    except Exception:                       # noqa: BLE001
        d = {}
    c.loop_stop(); c.disconnect()
    pub = d.get('ozkie_pub')
    return bytes.fromhex(pub) if pub and len(pub) == 64 else None


def main(dev, port, bridge, n, prefix, host, site):
    out = open(prefix + '.log', 'a', buffering=1)
    def say(s):
        print(s, flush=True); out.write(s + '\n')

    # BRIDGE_CONSOLE=<bridge serial port>: keep the bridge off the lock during
    # the door phase. An idle bridge link counts as the app link until its first
    # relay write (oz_gatt_esp32c6.cpp app_links), so the lock advertises BUSY
    # (non-connectable) and the Mac cannot get in — the 2026-11-06 "rig flake".
    # `unpair` stops the bridge reattaching after the reset below; `pair` after
    # the door lets it back in on its next rescan.
    bcon = os.environ.get('BRIDGE_CONSOLE')
    def bridge_cmd(line):
        with open(bcon, 'w') as f:
            f.write(line + '\n')
        say(f'{ts()} bridge console << {line}')
    pw = PresenceWatch(host, site, bridge, dev)
    if bcon:
        bridge_cmd(f'unpair {dev}')
    ser = TeeSerial(port, prefix + '_lock.log')
    t_open = time.time()
    time.sleep(3.0)                         # the one open may reset the lock: let it boot
    say(f'{ts()} relayrun {dev} via {bridge}, N={n}')

    # --- door, once ---
    owner, svc = OzApp('owner'), OzApp('ozlodge')
    k = None
    for attempt in range(3):
        try:
            k = BleLockLink(dev, serial=ser, scan_s=20.0); break
        except Exception as e:              # noqa: BLE001
            say(f'{ts()} door connect attempt {attempt + 1} failed: {type(e).__name__} {e}')
            time.sleep(5)
    if k is None:
        say('DOOR FAILED (RIG-BLE-FLAKE-1): no relay rounds run'); return False
    k.send(0x82, b''); _, iv = k.recv()
    info = uncbor(iv)[0]
    lock_pub = info[3]
    if info[4] != 0:
        say('lock is not UNCLAIMED: oz.wipe CONFIRM first'); return False
    if owner.connect(k, lock_pub) is not None:
        say('door pairing handshake failed'); return False
    owner.mid += 1
    req, x = owner.osc.protect(0x02, 'pair', mid=owner.mid)
    k.send(1, req); k.send(0x80, owner.e.pair_code().to_bytes(4, 'big'))
    t, p = k.recv(timeout=5.0)
    code, _, res = owner.osc.unprotect(p, x)
    if not (code == 0x44 and res[0] == R['BOND_OK']):
        say('door BOND failed'); return False
    owner.slot = 0
    rc, res = owner.call(0x01, 'bond')
    rc, res = owner.call(0x02, 'bond', {0: res[1], 1: 3, 2: svc.pub, 3: 'ozlodge'})
    svc.slot = res[1]
    if os.environ.get('BRIDGE_BOND'):
        bp = bridge_pub(host, site, bridge)
        if bp is None:
            say(f'{ts()} BRIDGE_BOND: {bridge} publishes no ozkie_pub (firmware < 0.47?)'); return False
        rc, res = owner.call(0x01, 'bond')
        rc, res = owner.call(0x02, 'bond', {0: res[1], 1: 4, 2: bp, 3: bridge})
        if not (res and res[0] == R['BOND_OK']):
            say(f'{ts()} BRIDGE_BOND: enrolment refused: {res}'); return False
        say(f'{ts()} bridge bond enrolled: {bridge} key {bp.hex()[:16]}… slot {res[1]}')
    k.close()
    say(f'{ts()} door done: owner slot 0, service slot {svc.slot}')
    ser.mark('door done, Mac BLE closed')
    if bcon:
        bridge_cmd(f'pair {dev}')

    t0 = time.time()
    held = pw.wait_held(after=t_open, timeout=100.0)   # 0.47 holds off an unclaimed lock 60 s
    pw.close()
    say(f'{ts()} bridge holds the lock: {held} after {time.time() - t0:.1f} s')

    # --- relay rounds ---
    m = LoggedRelayLink(host, site, bridge, dev, prefix + '_mqtt.log')
    passes, fails = 0, []
    for r in range(1, n + 1):
        m.round = r
        ser.mark(f'round {r} start')
        m.drain()
        t0 = time.time()
        step = 'edhoc'
        try:
            ok = owner.connect(m, lock_pub) is None
            t_ed = time.time() - t0
            if ok:
                step = 'unlock'
                code, opts, _ = owner.request(0x02, 'lock/unlock')
                ok = code == 0x81 and 252 in opts
            if ok:
                code, _, res = owner.request(0x02, 'lock/unlock', echo=opts[252])
                ok = bool(res) and res[0] == R['UNLOCK_OK']
            if ok:
                step = 'state'
                code, _, _ = owner.request(0x01, 'lock/state')
                ok = code is not None
            err = ''
        except Exception as e:              # noqa: BLE001
            ok, t_ed, err = False, time.time() - t0, f'{type(e).__name__}: {e}'
        if ok:
            passes += 1
            say(f'{ts()} round {r:2d} PASS  edhoc {t_ed:.2f} s, total {time.time() - t0:.2f} s')
        else:
            fails.append(r)
            say(f'{ts()} round {r:2d} FAIL  at {step} {err}')
        ser.mark(f'round {r} {"PASS" if ok else "FAIL at " + step}')
        time.sleep(3.0)                     # spacing; late uplinks still land in _mqtt.log
    time.sleep(3.0)
    m.close()

    # --- the owner at the door after a lock reboot, bridge attached ---
    door_n = int(os.environ.get('DOOR_AFTER_REBOOT') or 0)
    door_ok = 0
    for d in range(1, door_n + 1):
        ser.mark(f'door-after-reboot {d}: resetting the lock')
        pw2 = PresenceWatch(host, site, bridge, dev)
        ser.s.close(); time.sleep(0.5); ser.s.open()        # the reopen resets the lock
        t_rst = time.time()
        held = pw2.wait_held(after=t_rst, timeout=60.0)
        pw2.close()
        time.sleep(4.0)                     # let the bridge finish its bridge-bond EDHOC
        t0 = time.time()
        ok, why = False, ''
        try:
            kk = BleLockLink(dev, serial=ser, scan_s=20.0)
            ok = owner.connect(kk, lock_pub) is None
            if ok:
                code, _, _ = owner.request(0x01, 'lock/state')
                ok = code is not None
            kk.close()
        except Exception as e:              # noqa: BLE001
            why = f'{type(e).__name__} {e}'
        door_ok += ok
        say(f'{ts()} door-after-reboot {d}: bridge held={held}, owner at the door '
            f'{"PASS" if ok else "FAIL " + why} ({time.time() - t0:.1f} s)')
        ser.mark(f'door-after-reboot {d} {"PASS" if ok else "FAIL"}')
        time.sleep(3.0)
    ser.close()
    say(f'TOTAL {passes}/{n} PASS; failed rounds {fails}'
        + (f'; door-after-reboot {door_ok}/{door_n}' if door_n else ''))
    return not fails and door_ok == door_n


if __name__ == '__main__':
    if len(sys.argv) not in (6, 7, 8):
        print(__doc__); sys.exit(2)
    a = sys.argv[1:] + [None, None]
    sys.exit(0 if main(a[0], a[1], a[2], int(a[3]), a[4], a[5] or '10.1.1.42', a[6] or 'lab') else 1)
