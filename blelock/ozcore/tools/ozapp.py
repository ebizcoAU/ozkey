#!/usr/bin/env python3
"""ozapp.py — an independent OZKIE v2 app for interop with ozhostlock (ozkey-49 P2).

Written from the RFCs, not from ozcore's C: EDHOC method 3 / suite 0 (RFC 9528),
OSCORE (RFC 8613), CoAP (RFC 7252), CBOR (RFC 8949). The only dependency is
`cryptography`. Wire details beyond the RFCs follow ozkey-48 Appendix A.

  python3 -I ozapp.py rfc                      # self-check against RFC 9529 §3 / RFC 8613 C.4
  python3 -I ozapp.py interop PATH/ozhostlock  # full scenario against a fresh host lock

Importable: OzApp, LockLink, mint_invite() are what a server or the BANOI team
can build on.
"""
import hashlib, hmac, os, socket, struct, subprocess, sys, tempfile, time

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives import serialization

# ── CBOR (the subset OZKIE uses) ─────────────────────────────────────────────

def _head(major, n):
    if n < 24: return bytes([major << 5 | n])
    if n < 0x100: return bytes([major << 5 | 24, n])
    if n < 0x10000: return bytes([major << 5 | 25]) + n.to_bytes(2, 'big')
    if n < 0x100000000: return bytes([major << 5 | 26]) + n.to_bytes(4, 'big')
    return bytes([major << 5 | 27]) + n.to_bytes(8, 'big')

def cbor(v):
    if isinstance(v, Raw): return bytes(v)      # already-encoded CBOR
    if v is True: return b'\xf5'
    if v is False: return b'\xf4'
    if v is None: return b'\xf6'
    if isinstance(v, int): return _head(0, v) if v >= 0 else _head(1, -1 - v)
    if isinstance(v, (bytes, bytearray)): return _head(2, len(v)) + bytes(v)
    if isinstance(v, str): b = v.encode(); return _head(3, len(b)) + b
    if isinstance(v, (list, tuple)): return _head(4, len(v)) + b''.join(cbor(x) for x in v)
    if isinstance(v, dict): return _head(5, len(v)) + b''.join(cbor(k) + cbor(x) for k, x in v.items())
    raise TypeError(type(v))

class Raw(bytes):
    """Already-encoded CBOR, appended as is."""

def seq(*items):
    return b''.join(bytes(i) if isinstance(i, Raw) else cbor(i) for i in items)

def uncbor(b, i=0):
    """Decode one item at offset i → (value, next offset)."""
    ib = b[i]; major, ai = ib >> 5, ib & 31; i += 1
    if ai < 24: n = ai
    elif ai <= 27:
        k = 1 << (ai - 24); n = int.from_bytes(b[i:i + k], 'big'); i += k
    else: raise ValueError('indefinite / reserved')
    if major == 0: return n, i
    if major == 1: return -1 - n, i
    if major == 2: return bytes(b[i:i + n]), i + n
    if major == 3: return b[i:i + n].decode(), i + n
    if major == 4:
        out = []
        for _ in range(n): v, i = uncbor(b, i); out.append(v)
        return out, i
    if major == 5:
        out = {}
        for _ in range(n): k, i = uncbor(b, i); v, i = uncbor(b, i); out[k] = v
        return out, i
    if major == 7 and n in (20, 21, 22): return {20: False, 21: True, 22: None}[n], i
    raise ValueError('unsupported CBOR')

def uncbor_seq(b):
    out, i = [], 0
    while i < len(b): v, i = uncbor(b, i); out.append(v)
    return out

# ── primitives ───────────────────────────────────────────────────────────────

def sha256(b): return hashlib.sha256(b).digest()
def hkdf_extract(salt, ikm): return hmac.new(salt or b'\0' * 32, ikm, hashlib.sha256).digest()
def hkdf_expand(prk, info, n):
    out, t, c = b'', b'', 1
    while len(out) < n:
        t = hmac.new(prk, t + info + bytes([c]), hashlib.sha256).digest(); out += t; c += 1
    return out[:n]

def x25519(priv, pub):
    k = X25519PrivateKey.from_private_bytes(priv)
    return k.exchange(X25519PublicKey.from_public_bytes(pub))

def x25519_pub(priv):
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)

def ccm_seal(key, nonce, aad, pt): return AESCCM(key, tag_length=8).encrypt(nonce, pt, aad)
def ccm_open(key, nonce, aad, ct): return AESCCM(key, tag_length=8).decrypt(nonce, ct, aad)

def ccs(pub):
    """Canonical credential (ozkey-48 App. A): {8: {1: {1: 1, -1: 4, -2: x}}}."""
    return cbor({8: {1: {1: 1, -1: 4, -2: pub}}})

# ── EDHOC initiator, method 3 (RFC 9528) ────────────────────────────────────

def ident(b):
    """§3.3.2: a one-byte id that is a one-byte CBOR int travels as that int."""
    if len(b) == 1 and (b[0] <= 0x17 or 0x20 <= b[0] <= 0x37): return Raw(b)
    return b

def read_ident(buf, i):
    if buf[i] >> 5 in (0, 1):
        return bytes([buf[i]]), i + 1
    v, i = uncbor(buf, i)
    return v, i

class Edhoc:
    def __init__(self, priv, cred, id_cred, suites=(0,), c_i=b'\x05', dh=None, eph=None):
        self.priv, self.cred, self.id_cred = priv, cred, id_cred
        self.suites, self.c_i, self.dh = list(suites), c_i, dh or x25519
        self.x = eph[0] if eph else os.urandom(32)
        self.g_x = eph[1] if eph else x25519_pub(self.x)

    def kdf(self, prk, label, ctx, n):
        return hkdf_expand(prk, cbor(label) + cbor(ctx) + cbor(n), n)

    def compact_id_cred(self):
        m, _ = uncbor(self.id_cred)
        if isinstance(m, dict) and list(m) == [4]: return ident(m[4])
        return Raw(self.id_cred)

    def msg1(self, ead1=b''):
        s = self.suites[0] if len(self.suites) == 1 else self.suites
        m1 = seq(3, s, self.g_x, ident(self.c_i)) + ead1
        self.h1 = sha256(m1)
        return m1

    def msg2(self, m2, get_cred):
        gyc, _ = uncbor(m2)
        self.g_y, ct = gyc[:32], gyc[32:]
        self.th2 = sha256(seq(self.g_y, self.h1))
        g_xy = self.dh(self.x, self.g_y)
        prk2e = hkdf_extract(self.th2, g_xy)
        pt = bytes(a ^ b for a, b in zip(ct, self.kdf(prk2e, 0, self.th2, len(ct))))
        c_r, i = read_ident(pt, 0)
        if pt[i] >> 5 == 5:
            idc, j = uncbor(pt, i); id_cred_r = pt[i:j]; i = j
        else:
            kid, i = read_ident(pt, i); id_cred_r = cbor({4: kid})
        mac2, i = uncbor(pt, i)
        ead2 = pt[i:]
        cred_r, pub_r = get_cred(id_cred_r)
        salt = self.kdf(prk2e, 1, self.th2, 32)
        self.prk3e2m = hkdf_extract(salt, self.dh(self.x, pub_r))
        ctx2 = bytes(Raw(cbor(c_r) if not isinstance(ident(c_r), Raw) else c_r)) + id_cred_r + cbor(self.th2) + cred_r + ead2
        if not hmac.compare_digest(self.kdf(self.prk3e2m, 2, ctx2, 8), mac2):
            raise ValueError('MAC_2 mismatch')
        self.c_r, self.ead2 = c_r, ead2
        self.th3 = sha256(cbor(self.th2) + pt + cred_r)

    def msg3(self, ead3=b''):
        salt = self.kdf(self.prk3e2m, 5, self.th3, 32)
        prk4e3m = hkdf_extract(salt, self.dh(self.priv, self.g_y))
        mac3 = self.kdf(prk4e3m, 6, self.id_cred + cbor(self.th3) + self.cred + ead3, 8)
        pt = seq(self.compact_id_cred(), mac3) + ead3
        k3, iv3 = self.kdf(self.prk3e2m, 3, self.th3, 16), self.kdf(self.prk3e2m, 4, self.th3, 13)
        a3 = cbor(['Encrypt0', b'', self.th3])
        m3 = cbor(ccm_seal(k3, iv3, a3, pt))
        th4 = sha256(cbor(self.th3) + pt + self.cred)
        prk_out = self.kdf(prk4e3m, 7, th4, 32)
        self.prk_exporter = self.kdf(prk_out, 10, b'', 32)
        return m3

    def exporter(self, label, n, ctx=b''):
        return self.kdf(self.prk_exporter, label, ctx, n)

    def pair_code(self):
        return int.from_bytes(self.exporter(0xCF50, 4), 'big') % 1000000

    def oscore(self):
        """(secret, salt, sender_id, recipient_id) for the initiator (RFC 9528 Table 14)."""
        return self.exporter(0, 16), self.exporter(1, 8), self.c_r, self.c_i

# ── CoAP + OSCORE client (RFC 7252, RFC 8613) ───────────────────────────────

def coap_opts(opts):
    out, prev = b'', 0
    for num, val in sorted(opts, key=lambda o: o[0]):
        def nib(v):
            if v < 13: return v, b''
            if v < 269: return 13, bytes([v - 13])
            return 14, (v - 269).to_bytes(2, 'big')
        d, de = nib(num - prev); l, le = nib(len(val))
        out += bytes([d << 4 | l]) + de + le + val; prev = num
    return out

def coap_parse_opts(b):
    opts, i, num = [], 0, 0
    while i < len(b):
        if b[i] == 0xff: return opts, b[i + 1:]
        d, l = b[i] >> 4, b[i] & 15; i += 1
        def ext(v):
            nonlocal i
            if v == 13: v = 13 + b[i]; i += 1
            elif v == 14: v = 269 + int.from_bytes(b[i:i + 2], 'big'); i += 2
            return v
        d = ext(d); l = ext(l); num += d
        opts.append((num, b[i:i + l])); i += l
    return opts, b''

def coap_msg(typ, code, mid, token, opts, payload=b''):
    return bytes([0x40 | typ << 4 | len(token), code]) + mid.to_bytes(2, 'big') + token + \
        coap_opts(opts) + (b'\xff' + payload if payload else b'')

def coap_parse(b):
    tkl = b[0] & 15
    opts, payload = coap_parse_opts(b[4 + tkl:])
    return dict(type=(b[0] >> 4) & 3, code=b[1], mid=int.from_bytes(b[2:4], 'big'),
                token=b[4:4 + tkl], opts=opts, payload=payload)

OUTER = {3, 7, 35, 39}

class Oscore:
    def __init__(self, secret, salt, sid, rid, id_ctx=None):
        prk = hkdf_extract(salt, secret)
        def d(i, t, n): return hkdf_expand(prk, cbor([i, id_ctx, 10, t, n]), n)
        self.sid, self.rid = sid, rid
        self.skey, self.rkey, self.iv = d(sid, 'Key', 16), d(rid, 'Key', 16), d(b'', 'IV', 13)
        self.ssn = 0

    def nonce(self, idp, piv):
        n = bytes([len(idp)]) + idp.rjust(7, b'\0') + piv.rjust(5, b'\0')
        return bytes(a ^ b for a, b in zip(n, self.iv))

    def aad(self, kid, piv):
        return cbor(['Encrypt0', b'', cbor([1, [10], kid, piv, b''])])

    def protect(self, code, path, payload=b'', extra=(), mid=1, token=b'\x01'):
        piv = self.ssn.to_bytes(5, 'big').lstrip(b'\0') or b'\0'; self.ssn += 1
        inner = [(11, seg.encode()) for seg in path.split('/')] + list(extra)
        pt = bytes([code]) + coap_opts(inner) + (b'\xff' + payload if payload else b'')
        nonce = self.nonce(self.sid, piv)
        ct = ccm_seal(self.skey, nonce, self.aad(self.sid, piv), pt)
        optval = bytes([0x08 | len(piv)]) + piv + self.sid
        req = coap_msg(0, 0x02, mid, token, [(9, optval)], ct)
        return req, (self.sid, piv, nonce)

    def unprotect(self, resp, x):
        m = coap_parse(resp)
        ov = dict(m['opts']).get(9, b'')
        kid, piv, nonce = x
        if ov and ov[0] & 7:
            n = ov[0] & 7; rpiv = ov[1:1 + n]
            nonce = self.nonce(self.rid, rpiv)
        pt = ccm_open(self.rkey, nonce, self.aad(kid, piv), m['payload'])
        opts, payload = coap_parse_opts(pt[1:])
        return pt[0], dict(opts), uncbor(payload)[0] if payload else None

# ── the link to ozhostlock ───────────────────────────────────────────────────

class LockLink:
    def __init__(self, port, tries=20):
        # The lock takes one BLE central at a time and drops extra connections
        # at once (CONTRACT.md). If the previous one is still being torn down,
        # wait a moment and try again.
        for _ in range(tries):
            self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
            self.s.setblocking(False)
            time.sleep(0.03)
            try:
                dropped = self.s.recv(1, socket.MSG_PEEK) == b''
            except BlockingIOError:
                dropped = False
            self.s.setblocking(True)
            if not dropped:
                break
            self.s.close()
            time.sleep(0.05)
        self.buf = b''

    def send(self, typ, p): self.s.sendall(bytes([typ]) + len(p).to_bytes(2, 'big') + p)

    def recv(self, timeout=2.0):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 3:
                n = int.from_bytes(self.buf[1:3], 'big')
                if len(self.buf) >= 3 + n:
                    t, p = self.buf[0], self.buf[3:3 + n]; self.buf = self.buf[3 + n:]
                    return t, p
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return None, None
            if not d: return None, None
            self.buf += d

    def close(self): self.s.close()

R = dict(OK=0, UNLOCK_OK=1, LOCK_OK=2, BOND_OK=3, MEMBER_OK=4, REVOKE_OK=5, CRED_OK=6, TIME_OK=7,
         FORBIDDEN=23, STALE_EPOCH=22, REVOKE_DENIED=32)

class OzApp:
    def __init__(self, name):
        self.name, self.priv = name, os.urandom(32)
        self.pub = x25519_pub(self.priv)
        self.cred = ccs(self.pub)
        self.slot = None
        self.osc = None
        self.mid = 0

    def id_cred(self):
        return cbor({4: bytes([self.slot])}) if self.slot is not None else cbor({14: Raw(self.cred)})

    def connect(self, link, lock_pub, ead3=b''):
        """EDHOC over link. Returns None when accepted, else the lock's refusal text."""
        self.link, self.e = link, Edhoc(self.priv, self.cred, self.id_cred())
        lock_cred = ccs(lock_pub)
        def get(idc):
            if idc != cbor({4: b'\x00'}): raise ValueError('unknown lock credential')
            return lock_cred, lock_pub
        link.send(0, self.e.msg1())
        t, m2 = link.recv()
        if t != 0: raise ValueError('no message_2')
        try:
            self.e.msg2(m2, get)
        except ValueError:
            err = uncbor_seq(m2)
            return err[1] if len(err) == 2 and isinstance(err[1], str) else 'refused at message_2'
        link.send(0, self.e.msg3(ead3))
        t, p = link.recv(timeout=0.3)
        if t == 0:
            err = uncbor_seq(p)
            return err[1] if len(err) > 1 else 'refused'
        self.osc = Oscore(*self.e.oscore())
        return None

    def request(self, code, path, body=None, echo=None, wait=2.0):
        self.mid += 1
        extra = [(252, echo)] if echo else []
        req, x = self.osc.protect(code, path, cbor(body) if body is not None else b'', extra,
                                  mid=self.mid, token=bytes([self.mid & 0xff]))
        self.link.send(1, req)
        t, p = self.link.recv(timeout=wait)
        if t != 1: return None, {}, None
        return self.osc.unprotect(p, x)

    def call(self, code, path, body=None):
        """Request; on 4.01 + Echo, repeat once with it (ozkey-48 §4.2)."""
        rc, opts, res = self.request(code, path, body)
        if rc == 0x81 and 252 in opts:
            rc, opts, res = self.request(code, path, body, echo=opts[252])
        return rc, res

def mint_invite(issuer, lock_pub, device_id, nonce, version=2, me=0, bind_to=None, iss=0,
                expires=None, label='Ba Ngoai'):
    """An OZINV invite as the EAD_3 item (ozkey-48 §3.3), MAC per ftpos member_invite.dart."""
    expires = expires or int(time.time()) + 600
    secret = x25519(issuer.priv, lock_pub)
    key = hkdf_expand(hkdf_extract((device_id + issuer.pub.hex()).encode(), secret), b'ozkey/invite-v1', 32)
    fields = [str(version), device_id, issuer.pub.hex(), 'member', label, nonce.hex(), str(expires)]
    if version >= 2: fields.append(str(me))
    bind = sha256(bind_to.pub)[:16] if bind_to else b''
    if version >= 3: fields += [bind.hex(), str(iss)]
    mac = hmac.new(key, '|'.join(fields).encode(), hashlib.sha256).digest()
    arr = [version, device_id, issuer.pub, 'member', label, nonce, expires, me, mac]
    if version >= 3: arr += [bind, iss]
    return cbor(-20297) + cbor(cbor(arr))

# ── self-checks against the RFCs ─────────────────────────────────────────────

def rfc_check():
    h = bytes.fromhex
    T = dict(x=h('368ec1f69aeb659ba37d5a8d45b21bdc0299dceaa8ef235f3ca42ce3530f9525'),
             g_x=h('8af6f430ebe18d34184017a9a11bf511c8dff8f834730b96c1b7c8dbca2fc3b6'),
             sk_i=h('fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b'),
             g_y=h('419701d7f00a26c2dc587a36dd752549f33763c893422c8ea0f955a13a4ff5d5'),
             g_r=h('bbc34960526ea4d32e940cad2a234148ddc21791a12afbcbac93622046dd44f0'))
    shared = {(T['x'], T['g_y']): h('2f0cb7e860ba538fbf5c8bded009f6259b4b628fe1eb7dbe9378e5ecf7a824ba'),
              (T['x'], T['g_r']): h('f2b6eea02220b95eee5a0bc701f074e00a843ea02422f60825fb269b3e161423'),
              (T['sk_i'], T['g_y']): h('080f425085bc6249089eac8f108ea62326857e12ab07d72028ca1b5f36e004b3')}
    cred_r = h('a2026b6578616d706c652e65647508a101a501020241322001215820bbc34960526ea4d32e940cad2a2341'
               '48ddc21791a12afbcbac93622046dd44f02258204519e257236b2a0ce2023f0931f1f386ca7afda64fcde0'
               '108c224c51eabf6072')
    cred_i = h('a2027734322d35302d33312d46462d45462d33372d33322d333908a101a5010202412b2001215820ac75e9'
               'ece3e50bfc8ed60399889522405c47bf16df96660a41298cb4307f7eb62258206e5de611388a4b8a821133'
               '4ac7d37ecb52a387d257e6db3c2a93df21ff3affc8')
    e = Edhoc(T['sk_i'], cred_i, h('a104412b'), suites=(6, 2), c_i=b'\x37',
              dh=lambda p, q: shared[(p, q)], eph=(T['x'], T['g_x']))
    ok = True
    def chk(name, got, want):
        nonlocal ok
        good = got == want; ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {name}")
    chk('message_1', e.msg1(), h('0382060258208af6f430ebe18d34184017a9a11bf511c8dff8f834730b96c1b7c8dbca2fc3b637'))
    e.msg2(h('582b419701d7f00a26c2dc587a36dd752549f33763c893422c8ea0f955a13a4ff5d59862a1eef9e0e7e1886fcd'),
           lambda idc: (cred_r, T['g_r']))
    chk('message_3', e.msg3(), h('52e562097bc417dd5919485ac7891ffd90a9fc'))
    sec, salt, sid, rid = e.oscore()
    chk('OSCORE master secret', sec, h('f9868f6a3aca78a05d1485b35030b162'))
    chk('OSCORE master salt', salt, h('ada24c7dbfc85eeb'))
    # RFC 8613 C.4: request from the C.1 client at sequence number 20.
    o = Oscore(h('0102030405060708090a0b0c0d0e0f10'), h('9e7ca92223786340'), b'', b'\x01')
    o.ssn = 20
    req, _ = o.protect(0x01, 'tv1', mid=0x5d1f, token=h('00003974'))
    # C.4 also carries an outer Uri-Host; compare the protected part (option 9 + payload).
    chk('RFC 8613 C.4 ciphertext', req[-13:], h('612f1092f1776f1c1668b3825e'))
    return ok

# ── interop scenario against ozhostlock ──────────────────────────────────────

def interop(exe):
    tmp = tempfile.mkdtemp(prefix='ozhostlock-')
    state, dev = os.path.join(tmp, 'state'), 'ozk-host0001'
    ble, rel = 17001, 17002
    fails = 0
    def start():
        p = subprocess.Popen([exe, '--state', state, '--device-id', dev, '--ble', str(ble),
                              '--relay', str(rel)], stdout=subprocess.PIPE, text=True)
        p.stdout.readline(); key = p.stdout.readline().split()[-1]
        return p, bytes.fromhex(key)
    def chk(name, cond):
        nonlocal fails
        fails += 0 if cond else 1
        print(f"  {'ok  ' if cond else 'FAIL'} {name}")

    lockp, lock_pub = start()
    try:
        owner, member, svc = OzApp('owner'), OzApp('member'), OzApp('ozlodge')
        info = LockLink(ble); info.send(0x82, b''); _, iv = info.recv(); info.close()
        chk('info: unclaimed, key matches', uncbor(iv)[0][4] == 0 and uncbor(iv)[0][3] == lock_pub)

        # First pairing (§3.4): the app computes the code, the "owner" types it.
        k = LockLink(ble)
        chk('pairing handshake', owner.connect(k, lock_pub) is None)
        owner.mid += 1
        req, x = owner.osc.protect(0x02, 'pair', mid=owner.mid)
        k.send(1, req)
        k.send(0x80, owner.e.pair_code().to_bytes(4, 'big'))
        t, p = k.recv()
        code, _, res = owner.osc.unprotect(p, x)
        chk('BOND_OK after keypad code', code == 0x44 and res[0] == R['BOND_OK'])
        owner.slot = 0
        rc, res = owner.call(0x02, 'lock/unlock')
        chk('owner unlocks', res and res[0] == R['UNLOCK_OK'])
        rc, res = owner.call(0x03, 'time', {1: int(time.time())})
        chk('owner sets the clock', res and res[0] == R['TIME_OK'])
        k.close()

        # Member by invite, redeemed in EAD_3.
        ead = mint_invite(owner, lock_pub, dev, os.urandom(16), version=2)
        k = LockLink(ble)
        chk('member redeems invite', member.connect(k, lock_pub, ead) is None)
        rc, res = member.call(0x02, 'lock/unlock')
        chk('member unlocks at the door', res and res[0] == R['UNLOCK_OK'])
        k.close()
        k = LockLink(ble)
        chk('replayed invite refused', OzApp('thief').connect(k, lock_pub, ead) == 'MEMBER_REPLAY')
        k.close()

        # Credentials with the epoch rule.
        k = LockLink(ble); owner.connect(k, lock_pub)
        rc, res = owner.call(0x01, 'bond')
        epoch = res[1]
        rc, res = owner.call(0x02, 'cred', {0: epoch, 1: 1, 3: b'135790'})
        chk('add PIN', res and res[0] == R['CRED_OK'])
        rc, res = owner.call(0x02, 'cred', {0: epoch, 1: 1, 3: b'135790'})
        chk('stale epoch refused', res and res[0] == R['STALE_EPOCH'])
        epoch = res[1]
        rc, res = owner.call(0x02, 'bond', {0: epoch, 1: 3, 2: svc.pub, 3: 'ozlodge'})
        chk('enrol service bond', res and res[0] == R['BOND_OK'])
        svc.slot = res[1]
        k.close()
        k = LockLink(ble); k.send(0x81, b'135790'); _, p = k.recv(); k.close()
        chk('PIN opens the door', p == b'\x01')

        # Relay: owner unlock needs Echo; service can't actuate.
        r = LockLink(rel)
        chk('owner relay context', owner.connect(r, lock_pub) is None)
        code, opts, _ = owner.request(0x02, 'lock/unlock')
        chk('relay unlock → 4.01 + Echo', code == 0x81 and 252 in opts)
        code, _, res = owner.request(0x02, 'lock/unlock', echo=opts[252])
        chk('repeat with Echo unlocks', res and res[0] == R['UNLOCK_OK'])
        r.close()
        r = LockLink(rel)
        chk('service relay context', svc.connect(r, lock_pub) is None)
        rc, res = svc.call(0x02, 'lock/unlock')
        chk('service cannot unlock', res and res[0] == R['FORBIDDEN'])
        r.close()

        # Reboot: restart the process on the same state file.
        lockp.terminate(); lockp.wait()
        lockp, lock_pub2 = start()
        chk('identity survives reboot', lock_pub2 == lock_pub)
        r = LockLink(rel)
        owner.link = r
        code, opts, _ = owner.request(0x01, 'lock/state')
        chk('first relay request after reboot → Echo', code == 0x81 and 252 in opts)
        code, _, res = owner.request(0x01, 'lock/state', echo=opts[252])
        chk('state read after Echo', code == 0x45 and res[0] == R['OK'])
        r.close()
    finally:
        lockp.terminate(); lockp.wait()
    return fails == 0

if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'rfc':
        print('RFC self-check'); sys.exit(0 if rfc_check() else 1)
    if len(sys.argv) >= 3 and sys.argv[1] == 'interop':
        print('RFC self-check'); ok = rfc_check()
        print('interop vs', sys.argv[2]); ok &= interop(sys.argv[2])
        print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
    print(__doc__); sys.exit(2)
