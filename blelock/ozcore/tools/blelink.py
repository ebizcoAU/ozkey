#!/usr/bin/env python3
"""blelink.py — ozapp.py's LockLink over real BLE (ozkey-55 F5 part 2).

The same interface as ozapp.LockLink (send(type, payload) / recv() / close())
so ozapp's interop scenario runs unchanged against a real lock:

  type 0 / 1 / 2   an OZKIE message (EDHOC / OSCORE / error), framed per
                   ozkey-48 §2.1 and written to the `ozkie` characteristic;
                   replies come back as notifications and are reassembled
  type 0x80        keypad: pairing code (4-byte big-endian) → the lock's
                   serial shell `oz.code N`. No reply (the held response
                   arrives as an OSCORE message, as on the host lock)
  type 0x81        keypad: PIN digits → `oz.key <digits>#`; replies 0x81 with
                   1 byte, 1 = opened, from the lock's "[OZ] keypad: …" line
  type 0x82        read the `info` characteristic → 0x82 with its CBOR value

The 0x8x types stand in for a person at the door, exactly as in hostlock.c;
on a bench lock they go to its USB serial shell (dlc6v2.ino).

Finding the lock: scan actively for the v2 service, match the scan response's
4-byte short id against the device_id, connect, then CHECK the full device_id
in `info` (the short id is only 4 bytes, see ozkey-55). macOS hides BLE MAC
addresses, so the short id is the only pre-connect handle.

Needs `bleak` (BLE) and, for the 0x80/0x81 types, `pyserial`. Both are
imported lazily so the framing can be tested without them.
"""
import asyncio, queue, re, threading, time

SVC   = '4f5a4b32-0001-4c4f-434b-000000000001'
INFO  = '4f5a4b32-0002-4c4f-434b-000000000001'
OZKIE = '4f5a4b32-0003-4c4f-434b-000000000001'
MAX_MSG, MAX_FRAGS, MIN_MTU = 1024, 32, 23

# ── framing (ozkey-48 §2.1), written from the spec, not from oz_frame.c ──────

def fragment(typ, msg, mtu):
    """One message → frames that each fit one write (ATT MTU − 3)."""
    if typ not in (0, 1, 2) or len(msg) > MAX_MSG:
        raise ValueError('bad message')
    room = max(mtu, MIN_MTU) - 3 - 1
    chunks = [msg[i:i + room] for i in range(0, len(msg), room)] or [b'']
    if len(chunks) > MAX_FRAGS:
        raise ValueError('too many fragments')
    out = []
    for n, c in enumerate(chunks):
        more = 1 if n < len(chunks) - 1 else 0
        out.append(bytes([typ << 6 | more << 5 | n]) + c)
    return out

class Reassembler:
    """Frames → whole messages. Fragment 0 always restarts; anything out of
    order or oversized drops the partial message."""
    def __init__(self): self.reset()
    def reset(self): self.buf, self.typ, self.next, self.active = b'', 0, 0, False
    def feed(self, frame):
        if not frame:
            self.reset(); return None
        h = frame[0]; typ, more, n = h >> 6, (h >> 5) & 1, h & 0x1f
        if typ == 3:
            self.reset(); return None
        if n == 0:
            self.buf, self.typ, self.next, self.active = b'', typ, 0, True
        if not self.active or n != self.next or typ != self.typ:
            self.reset(); return None
        self.buf += frame[1:]
        if len(self.buf) > MAX_MSG:
            self.reset(); return None
        self.next += 1
        if more:
            return None
        msg, t = self.buf, self.typ
        self.reset()
        return t, msg

def short_id(device_id):
    """The scan-response short id: the first 4 bytes of the hex after '-'
    (oz_adv_scan_rsp, the same as v1's ozShortId)."""
    h = device_id.split('-', 1)[1]
    return bytes.fromhex(h[:8])

# ── the link ─────────────────────────────────────────────────────────────────

class _Loop:
    """One asyncio loop on a thread, so the link is synchronous like LockLink."""
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.t = threading.Thread(target=self.loop.run_forever, daemon=True); self.t.start()
    def run(self, coro, timeout=30):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)
_loop = None

class BleLockLink:
    """client_factory/serial are injectable so the host test can run without a
    radio; on the bench they default to bleak and pyserial."""
    def __init__(self, device_id, serial_port=None, scan_s=10.0, client=None, serial=None):
        global _loop
        _loop = _loop or _Loop()
        self.device_id, self.q, self.ra = device_id, queue.Queue(), Reassembler()
        self.ser = serial
        if self.ser is None and serial_port:
            import serial as pyserial
            self.ser = pyserial.Serial(serial_port, 115200, timeout=0.2)
        self.client = client or _loop.run(self._find_and_connect(scan_s), timeout=scan_s + 30)
        self.mtu = getattr(self.client, 'mtu_size', MIN_MTU) or MIN_MTU
        _loop.run(self.client.start_notify(OZKIE, self._on_notify))
        got = self._read_info()
        if got is None or got.get(1) != device_id:
            self.close()
            raise RuntimeError(f'connected to the wrong lock: info.device_id = {got and got.get(1)!r}')

    async def _find_and_connect(self, scan_s):
        from bleak import BleakScanner, BleakClient
        want = short_id(self.device_id)
        hit = asyncio.Event(); found = []
        def cb(dev, ad):
            sd = (ad.service_data or {}).get(SVC)
            if sd and len(sd) >= 5 and sd[1:5] == want and not found:
                found.append(dev); hit.set()
        async with BleakScanner(cb, service_uuids=[SVC], scanning_mode='active'):
            try:
                await asyncio.wait_for(hit.wait(), scan_s)
            except asyncio.TimeoutError:
                raise RuntimeError(f'no v2 lock with short id {want.hex()} advertising')
        c = BleakClient(found[0])
        await c.connect()
        return c

    def _on_notify(self, _char, data):
        m = self.ra.feed(bytes(data))
        if m:
            self.q.put(m)

    def _read_info(self):
        raw = bytes(_loop.run(self.client.read_gatt_char(INFO)))
        self._last_info = raw
        try:
            from ozapp import uncbor
            return uncbor(raw)[0]
        except Exception:
            return None

    def _shell(self, line, wait_for=None, timeout=3.0):
        if self.ser is None:
            raise RuntimeError('keypad step needs the lock\'s serial port (serial_port=…)')
        self.ser.reset_input_buffer()
        self.ser.write((line + '\n').encode())
        if not wait_for:
            return None
        end, buf = time.time() + timeout, ''
        while time.time() < end:
            buf += self.ser.read(256).decode('utf-8', 'replace')
            m = re.search(wait_for, buf)
            if m:
                return m
        return None

    def send(self, typ, p):
        if typ in (0, 1, 2):
            for f in fragment(typ, p, self.mtu):
                _loop.run(self.client.write_gatt_char(OZKIE, f, response=True))
        elif typ == 0x80:
            self._shell(f'oz.code {int.from_bytes(p, "big")}')
        elif typ == 0x81:
            m = self._shell(f'oz.key {p.decode()}#', r'\[OZ\] keypad: (open|wrong|locked out)')
            self.q.put((0x81, bytes([1 if m and m.group(1) == 'open' else 0])))
        elif typ == 0x82:
            self._read_info(); self.q.put((0x82, self._last_info))
        else:
            raise ValueError(f'unknown type {typ:#x}')

    def recv(self, timeout=2.0):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None, None

    def close(self):
        try:
            _loop.run(self.client.disconnect(), timeout=10)
        except Exception:
            pass
