#!/usr/bin/env python3
"""ozkierelay_ak.py — the OWNER APP for a REAL lock behind a real bridge, through ozlock only.

  python3 -I ozkierelay_ak.py <ozcore tools dir> <owner file> <ozlock API base> <N> [log file]

The lock was claimed at the door by relayrun.py (OWNER_SAVE=<owner file>), which
wrote JSON: {"priv": hex, "slot": int, "lock_pub": hex, "device_id": str, "bridge_id": str}.
This client:
  1. registers the pairing with ozlock (POST /pairings, ozkie_version 2), as the real app does;
  2. N rounds, each through POST /locks/:id/ozkie only: a fresh EDHOC, lock/state,
     unlock (4.01 + Echo), the repeat with Echo (UNLOCK_OK).
It opens no BLE, no serial port and no MQTT: it cannot reach the lock except via ozlock.
Prints one JSON line per check; exits 0 when every round passed.
"""
import base64, json, os, sys, time, urllib.error, urllib.request

sys.path.insert(0, sys.argv[1])
from ozapp import OzApp, R, ccs, x25519_pub  # noqa: E402

OWNER_FILE, BASE, N = sys.argv[2], sys.argv[3].rstrip('/'), int(sys.argv[4])
LOG = open(sys.argv[5], 'a', buffering=1) if len(sys.argv) > 5 else None
fails = 0


def ts():
    t = time.time()
    return time.strftime('%H:%M:%S', time.localtime(t)) + f'.{int(t * 1000) % 1000:03d}'


def check(name, ok, **kw):
    global fails
    fails += 0 if ok else 1
    line = json.dumps({'t': ts(), 'check': name, 'ok': bool(ok), **kw})
    print(line, flush=True)
    if LOG:
        LOG.write(line + '\n')
    return bool(ok)


def post(path, body):
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(), {'content-type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b'{}')
        except ValueError:
            return e.code, {}


def load_owner(path):
    """The owner that claimed the lock at the door. Refuses a file whose key,
    slot or lock key is not the right shape, before anything is sent."""
    with open(path) as f:
        d = json.load(f)
    priv, lock_pub = bytes.fromhex(d['priv']), bytes.fromhex(d['lock_pub'])
    if len(priv) != 32 or len(lock_pub) != 32:
        raise ValueError('owner file: priv and lock_pub must be 32 bytes each')
    slot = d['slot']
    if not isinstance(slot, int) or isinstance(slot, bool) or not 0 <= slot <= 255:
        raise ValueError('owner file: slot must be 0..255')
    for k in ('device_id', 'bridge_id'):
        if not isinstance(d.get(k), str) or not d[k]:
            raise ValueError(f'owner file: {k} missing')
    o = Owner('owner')
    o.priv = priv
    o.pub = x25519_pub(priv)
    o.cred = ccs(o.pub)
    o.slot = slot
    return o, lock_pub, d['device_id'], d['bridge_id']


class HttpLink:
    """ozapp's link shape (send/recv), over ozlock's relay endpoint."""
    def __init__(self, app_id, lock):
        self.app_id, self.lock, self.last, self.statuses = app_id, lock, None, []

    def send(self, typ, p):
        st, body = post(f'/locks/{self.lock}/ozkie',
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


def one_round(owner, lock_pub, link, r):
    link.statuses.clear()
    t0 = time.time()
    try:
        refused = owner.connect(link, lock_pub)
    except ValueError as e:
        refused = str(e)
    t_ed = time.time() - t0
    if not check(f'round {r}: EDHOC through ozlock', refused is None, refused=refused,
                 statuses=list(link.statuses), s=round(t_ed, 2)):
        return False
    rc, _, res = owner.request(0x01, 'lock/state')
    ok = check(f'round {r}: lock/state read', rc == 0x45 and bool(res) and res[0] == R['OK'], rc=rc,
               statuses=list(link.statuses))
    rc, opts, _ = owner.request(0x02, 'lock/unlock')
    if not check(f'round {r}: unlock → 4.01 + Echo', rc == 0x81 and 252 in opts, rc=rc,
                 statuses=list(link.statuses)):
        return False
    rc, _, res = owner.request(0x02, 'lock/unlock', echo=opts.get(252))
    ok = check(f'round {r}: repeat with Echo → UNLOCK_OK', bool(res) and res[0] == R['UNLOCK_OK'], rc=rc,
               statuses=list(link.statuses), total_s=round(time.time() - t0, 2)) and ok
    return ok


def main():
    owner, lock_pub, lock, bridge = load_owner(OWNER_FILE)
    app_id = owner.pub.hex()
    st, body = post('/pairings', {'app_id': app_id, 'device_id': lock, 'bridge_id': bridge, 'ozkie_version': 2})
    if not check('ozlock: pairing registered as v2', st == 200 and body.get('ozkie_version') == 2,
                 status=st, code=body.get('code')):
        return 1
    link = HttpLink(app_id, lock)
    passes = 0
    for r in range(1, N + 1):
        passes += one_round(owner, lock_pub, link, r)
        time.sleep(3.0)                     # spacing, as relayrun.py
    summary = json.dumps({'t': ts(), 'lock': lock, 'bridge': bridge, 'app_id': app_id,
                          'rounds': N, 'passed': passes, 'fails': fails})
    print(summary, flush=True)
    if LOG:
        LOG.write(summary + '\n')
    return 0 if passes == N and fails == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
