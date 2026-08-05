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
    ozctl.py enroll <OZINV1:...>      # redeem a BANOI invite, become a member
"""
import argparse, asyncio, json, os, secrets, sys, time

from bleak import BleakScanner, BleakClient
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import hashes, serialization

SVC       = "4f5a4b31-0001-4c4f-434b-000000000001"
CHR_PROV  = "4f5a4b31-0002-4c4f-434b-000000000001"
CHR_STAT  = "4f5a4b31-0003-4c4f-434b-000000000001"
CHR_INFO  = "4f5a4b31-0004-4c4f-434b-000000000001"
CHR_CHAL  = "4f5a4b31-0005-4c4f-434b-000000000001"
CHR_CTL   = "4f5a4b31-0006-4c4f-434b-000000000001"
CHR_MEMB  = "4f5a4b31-0007-4c4f-434b-000000000001"

ENV_VER = 0x02
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ozctl_state.json")


def log(tag, msg):
    print(f"{time.strftime('%H:%M:%S')}  {tag:9} {msg}", flush=True)


# ── identity + counter, persisted so app_id is stable across runs ────────────
def load_state():
    if os.path.exists(STATE):
        with open(STATE) as f:
            return json.load(f)
    priv = X25519PrivateKey.generate()
    raw = priv.private_bytes(serialization.Encoding.Raw,
                             serialization.PrivateFormat.Raw,
                             serialization.NoEncryption())
    st = {"priv": raw.hex(), "counters": {}}
    save_state(st)
    log("KEYS", "minted a fresh bench keypair")
    return st


def save_state(st):
    with open(STATE, "w") as f:
        json.dump(st, f, indent=2)


def our_priv(st):
    return X25519PrivateKey.from_private_bytes(bytes.fromhex(st["priv"]))


def our_app_id(st):
    pub = our_priv(st).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return pub.hex()


def next_counter(st, device_id):
    n = st["counters"].get(device_id, 0) + 1
    st["counters"][device_id] = n
    save_state(st)
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


# ── BLE session ─────────────────────────────────────────────────────────────
class Lock:
    def __init__(self, client):
        self.c = client
        self.status = []

    async def watch_status(self):
        def cb(_, data):
            s = data.decode("utf-8", "replace").strip()
            self.status.append(s)
            log("STATUS<-", s)
        await self.c.start_notify(CHR_STAT, cb)

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
    log("SCAN", f"looking for '{args.name}' — TOUCH THE LOCK'S SCREEN NOW "
                f"(60 s BLE window opens on any tap)")
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


def build_control(st, device_id, lock_pub_hex, challenge, frame):
    """utf8(app_id_hex) ‖ envelope(challenge ‖ frame)."""
    app_id = our_app_id(st)
    secret = our_priv(st).exchange(
        X25519PublicKey.from_public_bytes(bytes.fromhex(lock_pub_hex)))
    key = env_key(secret, device_id, app_id, app_to_lock=True)
    ctr = next_counter(st, device_id)
    env = env_seal(key, device_id, ctr, challenge + frame)
    log("SEAL", f"counter={ctr} plaintext={len(challenge)+len(frame)} B "
                f"envelope={len(env)} B total={64+len(env)} B")
    return app_id.encode() + env


async def run(args):
    st = load_state()
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

        if args.cmd == "enroll":
            payload = json.dumps({"app_id": our_app_id(st),
                                  "invite": args.arg}).encode()
            log("MEMBER->", f"{len(payload)} B enrol payload")
            for i in range(0, len(payload), 180):
                await c.write_gatt_char(CHR_MEMB, payload[i:i + 180], response=True)
            s = await lk.await_status(["MEMBER_"])
            log("RESULT", s or "no MEMBER_* status within timeout")
            return

        # the three control verbs
        if args.cmd == "unlock":
            frame = FRAME_UNLOCK()
        elif args.cmd == "revoke":
            frame = FRAME_REVOKE(bytes.fromhex(args.arg))
        elif args.cmd == "cancel":
            frame = FRAME_CANCEL(bytes.fromhex(args.arg))
        else:
            raise SystemExit(f"unknown command {args.cmd}")

        log("FRAME", frame.hex(" ").upper())
        ch = await lk.challenge()
        log("CHAL", f"issued {ch.hex()}")
        msg = build_control(st, device_id, lock_pub, ch, frame)
        await lk.write_control(msg, chunk=args.chunk)
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
                                    "revoke", "cancel", "enroll"])
    ap.add_argument("arg", nargs="?", default="")
    ap.add_argument("--name", default="OZLOCK")
    ap.add_argument("--scan", type=float, default=45.0)
    ap.add_argument("--chunk", type=int, default=0,
                    help="split the control write into N-byte chunks "
                         "(exercises the lock's reassembly path)")
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
