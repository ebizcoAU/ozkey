#!/usr/bin/env python3
"""ozkierelay_app.py — the OWNER APP for ozkierelay.test.js (ozkey-56 §7).

  python3 -I ozkierelay_app.py <ozcore tools dir> <lock BLE port> <ozlock API base> <lock id> <bridge id>

Uses ozcore's own RFC-written EDHOC/OSCORE client (tools/ozapp.py) as the app:
  1. pairs at the door over the host lock's BLE port (keypad code, ozkey-48 §3.4);
  2. registers the pairing with ozlock (POST /pairings, ozkie_version 2), as the real app does;
  3. runs EDHOC and OSCORE through ozlock's POST /locks/:id/ozkie only.
Prints one JSON line per check: {"check": name, "ok": bool, ...}; exits 0 when all pass.
"""
import base64, json, os, sys, urllib.error, urllib.request

sys.path.insert(0, sys.argv[1])
from ozapp import OzApp, LockLink, R, uncbor  # noqa: E402

BLE, BASE, LOCK, BRIDGE = int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
# OZLOCK-AUTH-1: the caller key, from the env (never argv, never printed).
API_KEY = os.environ.get('OZLOCK_API_KEY', '')
fails = 0


def check(name, ok, **kw):
    global fails
    fails += 0 if ok else 1
    print(json.dumps({'check': name, 'ok': bool(ok), **kw}), flush=True)


def post(path, body):
    headers = {'content-type': 'application/json'}
    if API_KEY:
        headers['authorization'] = 'Bearer ' + API_KEY
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(), headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')


class HttpLink:
    """ozapp's link shape (send/recv), over ozlock's relay endpoint."""
    def __init__(self, app_id):
        self.app_id, self.last, self.statuses = app_id, None, []

    def send(self, typ, p):
        st, body = post(f'/locks/{LOCK}/ozkie',
                        {'app_id': self.app_id, 'frame_b64': base64.b64encode(bytes([typ]) + p).decode()})
        self.statuses.append((st, body.get('code')))
        fb = body.get('frame_b64') if st == 200 else None
        self.last = (None, None) if not fb else (base64.b64decode(fb)[0], base64.b64decode(fb)[1:])

    def recv(self, timeout=2.0):
        r, self.last = self.last or (None, None), None
        return r


class Owner(OzApp):
    """ozapp's request() with a random 4-byte token (ozkey-48 §2.4.2)."""
    def request(self, code, path, body=None, echo=None, wait=2.0, token=None):
        from ozapp import cbor
        self.mid += 1
        extra = [(252, echo)] if echo else []
        req, x = self.osc.protect(code, path, cbor(body) if body is not None else b'', extra,
                                  mid=self.mid, token=token or os.urandom(4))
        self.link.send(1, req)
        t, p = self.link.recv(timeout=wait)
        if t != 1:
            return None, {}, None
        return self.osc.unprotect(p, x)


owner = Owner('owner')
APP_ID = owner.pub.hex()

# 1. at the door
info = LockLink(BLE); info.send(0x82, b''); _, iv = info.recv(); info.close()
lock_pub = uncbor(iv)[0][3]
k = LockLink(BLE)
check('door: pairing handshake', owner.connect(k, lock_pub) is None)
owner.mid += 1
req, x = owner.osc.protect(0x02, 'pair', mid=owner.mid, token=os.urandom(4))
k.send(1, req)
k.send(0x80, owner.e.pair_code().to_bytes(4, 'big'))
t, p = k.recv()
code, _, res = owner.osc.unprotect(p, x)
check('door: BOND_OK after keypad code', code == 0x44 and res[0] == R['BOND_OK'])
owner.slot = 0
k.close()

# 2. tell ozlock
st, body = post('/pairings', {'app_id': APP_ID, 'device_id': LOCK, 'bridge_id': BRIDGE, 'ozkie_version': 2})
check('ozlock: pairing registered as v2', st == 200 and body.get('ozkie_version') == 2, status=st)

# 3. through ozlock only
link = HttpLink(APP_ID)
try:
    refused = owner.connect(link, lock_pub)
except ValueError as e:
    refused = str(e)
check('relay: EDHOC through ozlock', refused is None, refused=refused, statuses=link.statuses)
if refused is not None:
    print(json.dumps({'app_id': APP_ID, 'fails': fails}), flush=True)
    sys.exit(1)
check('relay: m1 and m3 both 200', [s for s, _ in link.statuses] == [200, 200], statuses=link.statuses)

rc, _, res = owner.request(0x01, 'lock/state')
check('relay: lock/state read', rc == 0x45 and res and res[0] == R['OK'], rc=rc)

rc, opts, _ = owner.request(0x02, 'lock/unlock')
check('relay: unlock → 4.01 + Echo, routed back to this app', rc == 0x81 and 252 in opts, rc=rc)
rc, _, res = owner.request(0x02, 'lock/unlock', echo=opts.get(252))
check('relay: repeat with Echo → UNLOCK_OK', res and res[0] == R['UNLOCK_OK'], rc=rc)

link.statuses.clear()
rc, _, _ = owner.request(0x01, 'lock/state', token=b'\x07')
check('relay: 1-byte token refused by ozlock', link.statuses == [(400, 'token_too_short')], statuses=link.statuses)

print(json.dumps({'app_id': APP_ID, 'fails': fails}), flush=True)
sys.exit(1 if fails else 0)
