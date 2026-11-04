#!/usr/bin/env python3
"""Host test for blelink.py (ozkey-55 F5 part 2), no radio needed.

1. Framing against ozkey-48 §2.1: fragment/reassemble round trips at MTU 23
   and 247 up to 1024 B, the 1025 B and > 32-fragment refusals, fragment-0
   restart, out-of-order and type-change drops, the short id.
2. ozapp.py's whole interop scenario with every BLE link replaced by a
   BleLockLink over a FAKE GATT client. The fake splits nothing itself: it
   reassembles the link's writes with its own reassembler, forwards each whole
   message to the real ozhostlock engine, and fragments the replies at the
   negotiated MTU into notifications. So the link's framing, notification
   reassembly, info read, keypad-over-serial and sync facade are all exercised
   against the real lock engine. The relay port stays on TCP: the relay is the
   MQTT path, not BLE.

  python3 -I test_blelink.py <path>/ozhostlock
"""
import os, socket, sys, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blelink, ozapp
TCP_LINK = ozapp.LockLink              # captured before the scenario patches it

fails = 0
def chk(name, cond):
    global fails
    fails += 0 if cond else 1
    print(f"  {'ok  ' if cond else 'FAIL'} {name}")

def framing():
    # At MTU 23 a frame holds 19 B, so 32 fragments hold 608 B; at 247, 1024.
    for mtu, top in ((23, 608), (247, 1024)):
        for n in (0, 1, 19, 20, 243, 244, top - 1, top):
            msg = os.urandom(n)
            fr = blelink.fragment(1, msg, mtu)
            ra, out = blelink.Reassembler(), None
            for f in fr:
                chk_len = len(f) <= mtu - 3
                if not chk_len: break
                out = ra.feed(f) or out
            if not (chk_len and out == (1, msg)):
                chk(f'round trip mtu {mtu} len {n}', False)
    chk('round trips at MTU 23 (to 608 B) and 247 (to 1024 B), every frame ≤ MTU − 3', fails == 0)
    for bad in (lambda: blelink.fragment(1, bytes(1025), 247),
                lambda: blelink.fragment(1, bytes(609), 23)):       # 33 fragments > 32
        try: bad(); chk('oversize refused', False)
        except ValueError: chk('oversize refused', True)
    ra = blelink.Reassembler()
    a, b = blelink.fragment(0, bytes(range(40)), 23)[:2], blelink.fragment(0, b'xy', 23)
    ra.feed(a[0]); chk('fragment 0 restarts a half message', ra.feed(b[0]) == (0, b'xy'))
    f = blelink.fragment(1, bytes(60), 23)
    ra.feed(f[0]); chk('out-of-order fragment drops it', ra.feed(f[2]) is None and not ra.active)
    g = blelink.fragment(1, bytes(30), 23); h = blelink.fragment(0, bytes(30), 23)
    ra.feed(g[0]); chk('type change mid-message drops it', ra.feed(h[1]) is None and not ra.active)
    chk('short id = first 4 bytes after the dash (as v1/oz_adv)',
        blelink.short_id('ozk-acebe63acab8') == bytes.fromhex('acebe63a'))

class FakeGatt:
    """A BLE central's view of the lock, backed by one TCP session to
    ozhostlock (one TCP connection = one BLE connection there)."""
    def __init__(self, port, mtu):
        self.mtu_size, self.link, self.ra = mtu, TCP_LINK(port), blelink.Reassembler()
        self.cb, self.stop = None, False
        self.t = threading.Thread(target=self._pump, daemon=True)
    def _pump(self):
        while not self.stop:
            t, p = self.link.recv(timeout=0.1)
            if t is None: continue
            if t in (0, 1, 2):
                for f in blelink.fragment(t, p, self.mtu_size):
                    self.cb(None, bytearray(f))
            else:
                self.side.append((t, p))
    async def start_notify(self, uuid, cb):
        assert uuid == blelink.OZKIE
        self.cb, self.side = cb, []; self.t.start()
    async def write_gatt_char(self, uuid, data, response=True):
        assert uuid == blelink.OZKIE and len(data) <= self.mtu_size - 3
        m = self.ra.feed(bytes(data))
        if m: self.link.send(*m)
    async def read_gatt_char(self, uuid):
        assert uuid == blelink.INFO
        self.link.send(0x82, b'')
        for _ in range(50):
            for i, (t, p) in enumerate(self.side):
                if t == 0x82: del self.side[i]; return bytearray(p)
            await __import__('asyncio').sleep(0.02)
        raise RuntimeError('no info')
    async def disconnect(self):
        self.stop = True; self.link.close()

class FakeSerial:
    """The lock's serial shell: oz.code / oz.key become ozhostlock's keypad
    frames, and the PIN result comes back as dlc6v2's log line."""
    def __init__(self, gatt): self.g, self.out = gatt, b''
    def reset_input_buffer(self): self.out = b''
    def write(self, data):
        line = data.decode().strip()
        if line.startswith('oz.code '):
            self.g.link.send(0x80, int(line[8:]).to_bytes(4, 'big'))
        elif line.startswith('oz.key ') and line.endswith('#'):
            self.g.link.send(0x81, line[7:-1].encode())
            import time
            for _ in range(100):
                hit = [p for t, p in self.g.side if t == 0x81]
                if hit:
                    self.g.side[:] = [x for x in self.g.side if x[0] != 0x81]
                    self.out += b'[OZ] keypad: ' + (b'open' if hit[0] == b'\x01' else b'wrong') + b'\n'
                    return
                time.sleep(0.02)
    def read(self, n):
        d, self.out = self.out[:n], self.out[n:]; return d

def interop(exe, mtu):
    real = TCP_LINK
    ble_port = 17001                    # ozapp.interop's BLE port
    def link(port, *a, **k):
        if port != ble_port:
            return real(port, *a, **k)  # the relay (MQTT path) stays TCP
        g = FakeGatt(port, mtu)
        return blelink.BleLockLink('ozk-host0001', client=g, serial=FakeSerial(g))
    ozapp.LockLink = link
    try:
        print(f'  -- ozapp interop over BleLockLink, MTU {mtu} --')
        return ozapp.interop(exe)
    finally:
        ozapp.LockLink = real

if __name__ == '__main__':
    print('framing:'); framing()
    if len(sys.argv) >= 2:
        for mtu in (247, 23):
            if not interop(sys.argv[1], mtu):
                chk(f'interop scenario at MTU {mtu}', False)
            else:
                chk(f'interop scenario at MTU {mtu}', True)
    print('PASS' if fails == 0 else f'{fails} FAILED')
    sys.exit(1 if fails else 0)
