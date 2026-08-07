#!/usr/bin/env python3
"""ozctl — bench client for the OZLOCK M4 `control` characteristic (…0006).

The lock-side mirror is blelock/doorlock/ozcrypto.h; the app-side reference is
ftpos packages/ozkey_commissioner/lib/src/envelope.dart. This is a third
implementation written against the frozen wire format, which is the point: if
all three agree, the format is real rather than a shared assumption.

Wire (CONTRACT.md "Operational / member profile"):

    control write = utf8(app_id_hex, 64) ‖ envelope
    envelope      = ver(1B=0x02) ‖ counter(8B BE) ‖ nonce(12B) ‖ ct ‖ tag(16B)
    nonce         = prefix(4B random) ‖ counter(8B BE)
    AAD           = ver(1B) ‖ counter(8B BE) ‖ utf8(device_id)
    key           = HKDF-SHA256(ikm  = X25519(our_priv, lock_pub),
                                salt = utf8(device_id) ‖ utf8(app_id_hex),
                                info = "ozkey/app->lock")
    plaintext     = challenge(16) ‖ DPID frame

A commissioned lock only advertises for ~60 s after a keypad touch, so every
command here starts by asking for one. That is not a nicety — it is the physical
presence rule (XF-52 §4), and there is deliberately no remote way around it.

Usage:
    ozctl.py keys                     # show our app_id (the pubkey to enrol)
    ozctl.py info                     # read info + two challenges (freshness)
    ozctl.py probe                    # gate tests that need NO bond
    ozctl.py unlock                   # DP 1  — needs a bond
    ozctl.py revoke <pub_hex64>       # DP 101 — needs a bond
    ozctl.py cancel <nonce_hex32>     # DP 102 — needs bond #0
    ozctl.py invite <label>           # mint an OZINV1 invite — needs bond #0
    ozctl.py enroll <OZINV1:...>      # redeem an invite, become a member
    ozctl.py list_bonds               # DP 103 — needs bond #0 (admin-only)

`invite` and `enroll` are opposite ends of the same ceremony and must run as
TWO DIFFERENT identities (bond #0 mints, a member redeems) — pass `--state
<path>` to point either command at a second identity file, e.g.:

    ozctl.py invite "Ba Ngoai"                              # bond #0 (default state)
    ozctl.py --state ozctl_state_member.json enroll OZINV1:...  # a fresh identity

`invite` is pure local crypto (same pairing secret as `control`, no `member_enroll`
write) — it only connects to read `info` (device_id + lock pub), never mints
against a lock we haven't verified we own. Confirm with `list_bonds` first if
unsure which identity is bond #0 on a given lock.
"""
import argparse, asyncio, base64, json, os, secrets, sys, time

from bleak import BleakScanner, BleakClient
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import hashes, hmac, serialization

SVC       = "4f5a4b31-0001-4c4f-434b-000000000001"
CHR_PROV  = "4f5a4b31-0002-4c4f-434b-000000000001"
CHR_STAT  = "4f5a4b31-0003-4c4f-434b-000000000001"
CHR_INFO  = "4f5a4b31-0004-4c4f-434b-000000000001"
CHR_CHAL  = "4f5a4b31-0005-4c4f-434b-000000000001"
CHR_CTL   = "4f5a4b31-0006-4c4f-434b-000000000001"
CHR_MEMB  = "4f5a4b31-0007-4c4f-434b-000000000001"

ENV_VER = 0x02
INVITE_PREFIX = "OZINV1:"
DEFAULT_STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ozctl_state.json")


def log(tag, msg):
    print(f"{time.strftime('%H:%M:%S')}  {tag:9} {msg}", flush=True)


# ── identity + counter, persisted so app_id is stable across runs ────────────
def load_state(path):
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    priv = X25519PrivateKey.generate()
    raw = priv.private_bytes(serialization.Encoding.Raw,
                             serialization.PrivateFormat.Raw,
                             serialization.NoEncryption())
    st = {"priv": raw.hex(), "counters": {}}
    save_state(st, path)
    log("KEYS", f"minted a fresh bench keypair ({path})")
    return st


def save_state(st, path):
    with open(path, "w") as f:
        json.dump(st, f, indent=2)


def our_priv(st):
    return X25519PrivateKey.from_private_bytes(bytes.fromhex(st["priv"]))


def our_app_id(st):
    pub = our_priv(st).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return pub.hex()


def next_counter(st, device_id, path):
    n = st["counters"].get(device_id, 0) + 1
    st["counters"][device_id] = n
    save_state(st, path)
    return n


# ── the frozen crypto, mirroring ozcrypto.h ─────────────────────────────────
def env_key(pairing_secret, device_id, app_id_hex, app_to_lock=True):
    info = b"ozkey/app->lock" if app_to_lock else b"ozkey/lock->app"
    salt = (device_id + app_id_hex).encode()
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=salt,
                info=info).derive(pairing_secret)


def env_seal(key, device_id, counter, plaintext):
    nonce = secrets.token_bytes(4) + counter.to_bytes(8, "big")
    aad = bytes([ENV_VER]) + counter.to_bytes(8, "big") + device_id.encode()
    ct = AESGCM(key).encrypt(nonce, plaintext, aad)  # ct‖tag, same as mbedtls
    return bytes([ENV_VER]) + counter.to_bytes(8, "big") + nonce + ct


# ── DPID frames (the one builder, as in dpid_frames.dart / ozBuildDpFrame) ──
def dp_frame(dpid, dtype, value: bytes) -> bytes:
    dp = bytes([dpid, dtype, len(value) >> 8, len(value) & 0xFF]) + value
    f = bytes([0x55, 0xAA, 0x00, 0x06, len(dp) >> 8, len(dp) & 0xFF]) + dp
    return f + bytes([sum(f) & 0xFF])


FRAME_UNLOCK = lambda: dp_frame(1, 0x01, bytes([0x01]))
FRAME_REVOKE = lambda pub: dp_frame(101, 0x00, pub)
FRAME_CANCEL = lambda non: dp_frame(102, 0x00, non)
FRAME_LIST_BONDS = lambda: dp_frame(103, 0x00, b"")


# ── BLE session ─────────────────────────────────────────────────────────────
class Lock:
    def __init__(self, client):
        self.c = client
        self.status = []
        self.member_buf = ""
        self.bonds = None  # set once member_buf parses as JSON

    async def watch_status(self):
        def cb(_, data):
            s = data.decode("utf-8", "replace").strip()
            self.status.append(s)
            log("STATUS<-", s)
        await self.c.start_notify(CHR_STAT, cb)

    async def watch_member(self):
        # Mirrors doorlock.ino's ozNotifyChunked()/memberBuf convention in
        # reverse: a chunk starting with '[' begins a fresh buffer, every
        # piece appends, and "it parses as JSON" IS the end marker — no
        # length prefix, no explicit terminator.
        def cb(_, data):
            chunk = data.decode("utf-8", "replace")
            self.member_buf = chunk if chunk.startswith("[") else self.member_buf + chunk
            try:
                self.bonds = json.loads(self.member_buf)
                log("LIST<-", f"parsed {len(self.bonds)} bond(s), {len(self.member_buf)} B total")
            except json.JSONDecodeError:
                pass  # still assembling
        await self.c.start_notify(CHR_MEMB, cb)

    async def await_bonds(self, timeout=12.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.bonds is not None:
                return self.bonds
            await asyncio.sleep(0.1)
        return None

    async def info(self):
        raw = await self.c.read_gatt_char(CHR_INFO)
        return json.loads(raw.decode("utf-8", "replace"))

    async def challenge(self):
        ch = bytes(await self.c.read_gatt_char(CHR_CHAL))
        if len(ch) != 16:
            raise SystemExit(f"challenge returned {len(ch)} bytes, expected 16")
        return ch

    async def write_control(self, payload: bytes, chunk=0):
        if chunk and chunk < len(payload):
            for i in range(0, len(payload), chunk):
                await self.c.write_gatt_char(CHR_CTL, payload[i:i + chunk], response=True)
            log("CTL->", f"{len(payload)} B in {(len(payload)+chunk-1)//chunk} chunks of {chunk}")
        else:
            await self.c.write_gatt_char(CHR_CTL, payload, response=True)
            log("CTL->", f"{len(payload)} B in one write")

    async def await_status(self, prefixes, timeout=12.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for s in self.status:
                if any(s.startswith(p) for p in prefixes):
                    return s
            await asyncio.sleep(0.1)
        return None


async def connect(args):
    if args.addr:
        log("SCAN", f"looking for address {args.addr} — TOUCH THE LOCK'S SCREEN NOW "
                    f"(60 s BLE window opens on any tap)")
        dev = None
        t0 = time.time()
        while time.time() - t0 < args.scan:
            dev = await BleakScanner.find_device_by_address(args.addr, timeout=5.0)
            if dev:
                break
        if not dev:
            raise SystemExit(f"no device at {args.addr} seen in {args.scan}s — was the "
                             f"screen touched?")
        log("SCAN", f"found {dev.address}")
        return dev

    log("SCAN", f"looking for '{args.name}' — TOUCH THE LOCK'S SCREEN NOW "
                f"(60 s BLE window opens on any tap). WARNING: with more than one "
                f"'{args.name}' advertising at once this grabs whichever answers first "
                f"— pass --addr to pin a specific device.")
    dev = None
    t0 = time.time()
    while time.time() - t0 < args.scan:
        dev = await BleakScanner.find_device_by_name(args.name, timeout=5.0)
        if dev:
            break
    if not dev:
        raise SystemExit(f"no '{args.name}' seen in {args.scan}s — was the screen touched? "
                         f"(a commissioned lock is dark outside its window)")
    log("SCAN", f"found {dev.address}")
    return dev


def build_control(st, state_path, device_id, lock_pub_hex, challenge, frame):
    """utf8(app_id_hex) ‖ envelope(challenge ‖ frame)."""
    app_id = our_app_id(st)
    secret = our_priv(st).exchange(
        X25519PublicKey.from_public_bytes(bytes.fromhex(lock_pub_hex)))
    key = env_key(secret, device_id, app_id, app_to_lock=True)
    ctr = next_counter(st, device_id, state_path)
    env = env_seal(key, device_id, ctr, challenge + frame)
    log("SEAL", f"counter={ctr} plaintext={len(challenge)+len(frame)} B "
                f"envelope={len(env)} B total={64+len(env)} B")
    return app_id.encode() + env


# ── member invite (CONTRACT.md "Member-enroll lock-side algorithm") ────────
# Byte-exact-verified against ftpos's frozen vector
# (packages/ozkey_commissioner/tool/gen_invite_vector.dart) before first use:
# pairing_secret=01..20 hex, device_id="ozk-a4cf12879da7",
# issuer="aa"*32, label="Ba Ngoai", nonce="42"*16, expires=1789000000
# -> mac e7780baea8feef5674c0ffecd1b83f35dfd9198db50cea6d0735c7a43d268aac (MATCH).
def invite_mac(pairing_secret, device_id, issuer_app_id, role, label, nonce_hex, expires):
    salt = (device_id + issuer_app_id).encode()
    key = HKDF(algorithm=hashes.SHA256(), length=32, salt=salt,
               info=b"ozkey/invite-v1").derive(pairing_secret)
    canonical = f"1|{device_id}|{issuer_app_id}|{role}|{label}|{nonce_hex}|{expires}"
    h = hmac.HMAC(key, hashes.SHA256())
    h.update(canonical.encode())
    return h.finalize().hex()


def build_invite(st, device_id, lock_pub_hex, label, role="member", ttl=600):
    """Mints an OZINV1 invite AS bond #0 — same pairing secret `control` uses,
    no BLE write. If `st` is not actually bond #0 on this lock, the mac simply
    won't match what the lock recomputes, and enroll will report MEMBER_FAIL."""
    issuer_app_id = our_app_id(st)
    secret = our_priv(st).exchange(
        X25519PublicKey.from_public_bytes(bytes.fromhex(lock_pub_hex)))
    nonce_hex = secrets.token_bytes(16).hex()
    expires = int(time.time()) + ttl
    mac = invite_mac(secret, device_id, issuer_app_id, role, label, nonce_hex, expires)
    obj = {"v": 1, "d": device_id, "i": issuer_app_id, "r": role,
           "l": label, "n": nonce_hex, "e": expires, "m": mac}
    payload = base64.urlsafe_b64encode(json.dumps(obj, separators=(",", ":")).encode()).decode()
    return INVITE_PREFIX + payload


async def run(args):
    state_path = args.state or DEFAULT_STATE
    st = load_state(state_path)
    if args.cmd == "keys":
        print(our_app_id(st))
        return

    dev = await connect(args)
    async with BleakClient(dev) as c:
        lk = Lock(c)
        await lk.watch_status()
        info = await lk.info()
        device_id = info.get("device_id", "")
        lock_pub = info.get("pub", "")
        log("INFO", f"device_id={device_id} fw={info.get('fw')} "
                    f"transport={info.get('transport')}")
        log("INFO", f"lock pub={lock_pub}")
        log("KEYS", f"our app_id={our_app_id(st)}")

        if args.cmd == "info":
            a = await lk.challenge()
            b = await lk.challenge()
            log("CHAL", f"read1={a.hex()}")
            log("CHAL", f"read2={b.hex()}")
            log("CHAL", "FRESH per read — PASS" if a != b else "IDENTICAL — FAIL")
            return

        if args.cmd == "probe":
            await probe(lk, st, device_id, lock_pub)
            return

        if args.cmd == "invite":
            label = args.arg or "member"
            inv = build_invite(st, device_id, lock_pub, label, role=args.role, ttl=args.ttl)
            log("INVITE", f"issuer(us)={our_app_id(st)} label={label!r} role={args.role} "
                          f"ttl={args.ttl}s")
            print(inv)
            return

        if args.cmd == "enroll":
            payload = json.dumps({"app_id": our_app_id(st),
                                  "invite": args.arg}).encode()
            log("MEMBER->", f"{len(payload)} B enrol payload")
            for i in range(0, len(payload), 180):
                await c.write_gatt_char(CHR_MEMB, payload[i:i + 180], response=True)
            s = await lk.await_status(["MEMBER_"])
            log("RESULT", s or "no MEMBER_* status within timeout")
            return

        # the control verbs
        if args.cmd == "unlock":
            frame = FRAME_UNLOCK()
        elif args.cmd == "revoke":
            frame = FRAME_REVOKE(bytes.fromhex(args.arg))
        elif args.cmd == "cancel":
            frame = FRAME_CANCEL(bytes.fromhex(args.arg))
        elif args.cmd == "list_bonds":
            frame = FRAME_LIST_BONDS()
        else:
            raise SystemExit(f"unknown command {args.cmd}")

        log("FRAME", frame.hex(" ").upper())
        if args.cmd == "list_bonds":
            await lk.watch_member()
        ch = await lk.challenge()
        log("CHAL", f"issued {ch.hex()}")
        msg = build_control(st, state_path, device_id, lock_pub, ch, frame)
        await lk.write_control(msg, chunk=args.chunk)

        if args.cmd == "list_bonds":
            bonds = await lk.await_bonds(timeout=6.0)
            if bonds is not None:
                log("RESULT", f"LIST_OK — {len(bonds)} bond(s)")
                for b in bonds:
                    log("BOND", f"slot={b['slot']} label={b['label']!r} "
                                f"floor={b['floor']} pub={b['pub'][:16]}…")
            else:
                s = await lk.await_status(["LIST_"], timeout=1.0)
                log("RESULT", s or "NO ANSWER — neither a bond list nor LIST_DENIED (defect)")
            return

        s = await lk.await_status(["UNLOCK_", "REVOKE_"])
        log("RESULT", s or "NO STATUS — the lock never answered (this is a defect)")


async def probe(lk, st, device_id, lock_pub):
    """Gate tests that need no bond. Every one of these must produce a status —
    'no answer' is itself the failure, because a write with no reply is the
    XF-53 hang wearing new clothes."""
    log("PROBE", "1/3 short write (50 B) — expect UNLOCK_DENIED after the 400 ms backstop")
    lk.status.clear()
    t0 = time.time()
    await lk.write_control(bytes(50))
    s = await lk.await_status(["UNLOCK_"], timeout=5.0)
    log("PROBE", f"-> {s or 'NO ANSWER (FAIL)'}  after {time.time()-t0:.2f}s")

    log("PROBE", "2/3 valid length, non-hex app_id — expect immediate UNLOCK_DENIED")
    lk.status.clear()
    t0 = time.time()
    await lk.write_control(b"Z" * 64 + bytes(37))
    s = await lk.await_status(["UNLOCK_"], timeout=5.0)
    log("PROBE", f"-> {s or 'NO ANSWER (FAIL)'}  after {time.time()-t0:.2f}s")

    log("PROBE", "3/3 well-formed app_id that holds NO bond — expect UNLOCK_DENIED")
    lk.status.clear()
    t0 = time.time()
    stranger = X25519PrivateKey.generate().public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    await lk.write_control(stranger.encode() + bytes(37))
    s = await lk.await_status(["UNLOCK_"], timeout=5.0)
    log("PROBE", f"-> {s or 'NO ANSWER (FAIL)'}  after {time.time()-t0:.2f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["keys", "info", "probe", "unlock",
                                    "revoke", "cancel", "invite", "enroll",
                                    "list_bonds"])
    ap.add_argument("arg", nargs="?", default="",
                    help="revoke/cancel: hex arg. invite: label. enroll: OZINV1:...")
    ap.add_argument("--state", default=None,
                    help="identity/state JSON path (default: ozctl_state.json next "
                         "to this script) — use a second file to act as a second "
                         "identity, e.g. a member redeeming an invite")
    ap.add_argument("--role", default="member", choices=["member", "admin"],
                    help="invite only — role admin is refused by firmware in v1")
    ap.add_argument("--ttl", type=int, default=600,
                    help="invite only — seconds until expiry (parse-and-ignore "
                         "in v1 firmware; the nonce/replay-cache is the real guard)")
    ap.add_argument("--name", default="OZLOCK")
    ap.add_argument("--addr", default=None,
                    help="pin a specific BLE address instead of scanning by --name "
                         "— required once more than one lock advertises at once")
    ap.add_argument("--scan", type=float, default=45.0)
    ap.add_argument("--chunk", type=int, default=0,
                    help="split the control write into N-byte chunks "
                         "(exercises the lock's reassembly path)")
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
