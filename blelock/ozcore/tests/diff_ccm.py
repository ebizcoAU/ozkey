#!/usr/bin/env python3
"""Differential test: ozcore AES-128-CCM vs Python `cryptography` (OpenSSL).

usage: diff_ccm.py <path-to-ccm_vec> [cases]
Random keys, every legal nonce length (7..13) and tag length (4..16 even),
aad 0..600 bytes (block-packing edges around 16), pt 0..300 bytes.
Exit 0 only if every case matches byte-for-byte.
"""
import os, random, subprocess, sys
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

exe = sys.argv[1]
cases = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
rng = random.Random(int.from_bytes(os.urandom(8), "big"))

lines, expect = [], []
for _ in range(cases):
    key = rng.randbytes(16)
    nl = rng.randint(7, 13)
    tag = rng.choice(range(4, 17, 2))
    al = rng.choice([0, 1, 13, 14, 15, 16, 17, rng.randint(0, 600)])
    pl = rng.choice([0, 1, 15, 16, 17, rng.randint(0, 300)])
    nonce, aad, pt = rng.randbytes(nl), rng.randbytes(al), rng.randbytes(pl)
    ct = AESCCM(key, tag).encrypt(nonce, pt, aad or None)
    h = lambda b: b.hex() if b else "-"
    lines.append(f"{key.hex()} {nonce.hex()} {h(aad)} {h(pt)} {tag}")
    expect.append(ct.hex())

out = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True,
                     text=True, check=True).stdout.split("\n")
bad = 0
for i, (want, got) in enumerate(zip(expect, out)):
    parts = got.split()                  # "<ct‖tag hex> ok|bad", or "err"
    got_ct = parts[0] if len(parts) == 2 else ""
    status = parts[1] if len(parts) == 2 else "missing"
    if got_ct != want or status != "ok":
        bad += 1
        if bad <= 5:
            print(f"MISMATCH case {i}: {lines[i]}\n  want {want}\n  got  {got}")
print(f"diff_ccm: {cases - bad}/{cases} cases match cryptography/OpenSSL")
sys.exit(1 if bad else 0)
