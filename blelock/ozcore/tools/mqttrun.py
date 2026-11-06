#!/usr/bin/env python3
"""mqttrun.py — OZKIE v2 through a REAL bridge, with this Mac standing in for
ozlock (ozkey-55 F1, ozkey-48 rev 6 §2.4). The server-side relay isn't built
yet; this script publishes the downlink and reads the uplink itself.

  python3 -I mqttrun.py <lock device_id> <lock serial port> <bridge_id> [broker] [site]

  e.g.  mqttrun.py ozk-acebe63acab8 /dev/cu.usbmodem1431401 ozb-206ef116f390 10.1.1.42 lab

Preconditions: the lock is UNCLAIMED (`oz.wipe CONFIRM`), the bridge runs the
F1 firmware, is on site <site>, and has the lock on its allowlist
(`pair <device_id>` on its console). The bridge holds the lock's relay link.

The owner pairs at the door over direct BLE (the second connection slot), then
everything else goes through MQTT → bridge → relay characteristic → lock.
"""
import os, queue, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ozapp import OzApp, R, uncbor                    # noqa: E402
from blelink import BleLockLink                       # noqa: E402


class MqttRelayLink:
    """Same send/recv shape as BleLockLink, for types 0..2 only. The payload on
    the wire is type(1) ‖ whole message; the bridge does the fragmenting."""
    def __init__(self, host, site, bridge_id, lock_id):
        import paho.mqtt.client as mqtt
        self.down = f'ozkie/{site}/bridges/{bridge_id}/ozkie/{lock_id}'
        self.up = f'ozkie/{site}/locks/{lock_id}/ozkie/{bridge_id}'
        self.q = queue.Queue()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'mqttrun-{os.getpid()}')
        self.c.on_message = lambda c, u, m: self.q.put(bytes(m.payload))
        self.c.connect(host, 1883, 30)
        self.c.subscribe(self.up, qos=0)
        self.c.loop_start()
        time.sleep(0.5)

    def raw(self, payload):
        self.c.publish(self.down, payload, qos=0, retain=False).wait_for_publish(5)

    def send(self, typ, p):
        if typ not in (0, 1, 2):
            raise ValueError(f'type {typ:#x} has no relay path')
        self.raw(bytes([typ]) + p)

    def recv(self, timeout=2.0):
        try:
            m = self.q.get(timeout=timeout)
        except queue.Empty:
            return None, None
        return m[0], m[1:]

    def drain(self, s=0.5):
        time.sleep(s)
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.c.loop_stop(); self.c.disconnect()


def main(dev, port, bridge, host, site):
    import serial as pyserial
    ser = pyserial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    time.sleep(3.0)                      # the open reset the lock: let it boot
    ser.reset_input_buffer()
    global _ser_for_dump
    _ser_for_dump = ser

    fails = 0

    def chk(name, cond):
        nonlocal fails
        fails += 0 if cond else 1
        print(f"  {'ok  ' if cond else 'FAIL'} {name}", flush=True)

    owner, svc = OzApp('owner'), OzApp('ozlodge')

    # --- at the door: first pairing + service bond (direct BLE, app slot) ---
    for attempt in range(3):
        try:
            k = BleLockLink(dev, serial=ser, scan_s=20.0); break
        except RuntimeError:
            if attempt == 2: raise
            time.sleep(3)
    k.send(0x82, b''); _, iv = k.recv()
    info = uncbor(iv)[0]
    lock_pub = info[3]
    chk('door: unclaimed', info[4] == 0)
    chk('door: pairing handshake', owner.connect(k, lock_pub) is None)
    owner.mid += 1
    req, x = owner.osc.protect(0x02, 'pair', mid=owner.mid)
    k.send(1, req); k.send(0x80, owner.e.pair_code().to_bytes(4, 'big'))
    t, p = k.recv(timeout=5.0)
    ok = p is not None
    if ok:
        code, _, res = owner.osc.unprotect(p, x)
        ok = code == 0x44 and res[0] == R['BOND_OK']
    chk('door: BOND_OK', ok)
    owner.slot = 0
    rc, res = owner.call(0x01, 'bond')
    rc, res = owner.call(0x02, 'bond', {0: res[1], 1: 3, 2: svc.pub, 3: 'ozlodge'})
    chk('door: enrol service bond', res and res[0] == R['BOND_OK'])
    svc.slot = res[1]
    k.close()

    # --- remote, through the bridge ---
    m = MqttRelayLink(host, site, bridge, dev)
    print(f'downlink {m.down}\nuplink   {m.up}', flush=True)
    t0 = time.time()
    chk('relay: owner EDHOC via bridge', owner.connect(m, lock_pub) is None)
    print(f'    EDHOC round trip {time.time() - t0:.2f} s', flush=True)
    code, opts, _ = owner.request(0x02, 'lock/unlock')
    chk('relay: unlock → 4.01 + Echo', code == 0x81 and 252 in opts)
    t0 = time.time()
    code, _, res = owner.request(0x02, 'lock/unlock', echo=opts[252])
    chk('relay: repeat with Echo unlocks', res and res[0] == R['UNLOCK_OK'])
    print(f'    unlock round trip {time.time() - t0:.2f} s', flush=True)
    code, opts, res = owner.request(0x01, 'lock/state')
    chk('relay: state read', code is not None)
    chk('relay: service context', svc.connect(m, lock_pub) is None)
    rc, res = svc.call(0x02, 'lock/unlock')
    chk('relay: service cannot unlock', res and res[0] == R['FORBIDDEN'])

    # Bridge drops: unknown type, empty, oversize (>1+1024). None may reach the
    # lock or come back; the owner context must still work afterwards.
    m.drain()
    m.raw(b'\x03' + b'x' * 20); m.raw(b'\x01'); m.raw(b'\x01' + b'\0' * 1025)
    t, p = m.recv(timeout=2.0)
    chk('bridge: bad type / short / oversize produce no uplink', p is None)
    code, opts, res = owner.request(0x01, 'lock/state')
    chk('relay: context intact after the drops', code is not None)
    m.close()

    ser.close()
    print('PASS' if fails == 0 else f'FAIL ({fails})', flush=True)
    return fails == 0


_ser_for_dump = None


def dump_serial():
    if _ser_for_dump is None:
        return
    time.sleep(1.5)
    raw = b''
    while True:
        chunk = _ser_for_dump.read(4096)
        if not chunk:
            break
        raw += chunk
    print('--- lock serial ---')
    print(raw.decode('utf-8', 'replace')[-4000:])


if __name__ == '__main__':
    if len(sys.argv) not in (4, 5, 6):
        print(__doc__); sys.exit(2)
    a = sys.argv[1:] + [None, None]
    try:
        sys.exit(0 if main(a[0], a[1], a[2], a[3] or '10.1.1.42', a[4] or 'lab') else 1)
    except Exception:
        import traceback
        traceback.print_exc(limit=2)
        dump_serial()
        sys.exit(1)
