#!/usr/bin/env python3
"""BRIDGE-CMD-AUTH-1 LAB signing helper: the bench stand-in for ozlock's signer.

    bcmd_sign.py keygen <seed_file>                     write a random lab seed (0600), print the pub
    bcmd_sign.py pub <seed_file>                        print the pub (64 hex)
    bcmd_sign.py frame <seed_file> <bridge> <op> <ctr> [json_args|null]  -> frame hex on stdout

A LAB key only (never a site/production key). Frame = sig ‖ CBOR [1, op, bridge,
ctr, args], sig = Ed25519(seed, "ozkie-bcmd-v1" ‖ body): the encoder and the
signature are gen_bcmd_vectors.py's (same file, so the bench uses the code the
shared vectors pin). Byte-string args: pass {"pub": "hex:<64 hex>"}.
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_bcmd_vectors import cbor, pub_of, DOMAIN          # noqa: E402


def load(path):
    seed = bytes.fromhex(open(path).read().strip())
    assert len(seed) == 32
    return pub_of(seed)


def main(a):
    if a[1] == 'keygen':
        fd = os.open(a[2], os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.write(fd, os.urandom(32).hex().encode() + b'\n'); os.close(fd)
        print(load(a[2])[1].hex()); return 0
    if a[1] == 'pub':
        print(load(a[2])[1].hex()); return 0
    if a[1] == 'frame':
        key, _ = load(a[2])
        args = None if len(a) < 7 or a[6] == 'null' else json.loads(a[6])
        if args:
            args = {k: bytes.fromhex(v[4:]) if isinstance(v, str) and v.startswith('hex:') else v for k, v in args.items()}
        body = cbor([1, a[4], a[3], int(a[5]), args])
        sys.stdout.write((key.sign(DOMAIN + body) + body).hex() + '\n'); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
