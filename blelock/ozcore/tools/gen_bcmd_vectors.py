#!/usr/bin/env python3
"""BRIDGE-CMD-AUTH-1: shared test vectors for signed bridge commands (CONTRACT-BRIDGE.md).

    python3 -I gen_bcmd_vectors.py <out_dir>

Writes <out_dir>/bridge_cmd_v1.json (for ozlock's encoder/signer tests) and
<out_dir>/bridge_cmd_v1.h (for the bridge's host test). Deterministic: a fixed
seed, fixed commands. Independent of the bridge's code on purpose: its own
minimal deterministic CBOR encoder, and Ed25519 (RFC 8032) from Python's
`cryptography`. The bridge test re-encodes each body with ozcore's CBOR writer
(must be byte-identical) and verifies each signature with Monocypher's
crypto_ed25519_check.
"""
import json, os, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

DOMAIN = b'ozkie-bcmd-v1'
SEED = bytes(range(1, 33))          # 01 02 .. 20: a TEST key, public on purpose
SEED2 = bytes(range(33, 65))        # a second key, for rotation / a foreign signer


def head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if n < (1 << (8 * size)):
            return bytes([major << 5 | ai]) + n.to_bytes(size, 'big')
    raise ValueError(n)


def cbor(v):
    """Deterministic CBOR: shortest heads, definite lengths, map keys sorted bytewise."""
    if v is None:
        return b'\xf6'
    if isinstance(v, bool):
        return b'\xf5' if v else b'\xf4'
    if isinstance(v, int):
        return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, str):
        b = v.encode()
        return head(3, len(b)) + b
    if isinstance(v, list):
        return head(4, len(v)) + b''.join(cbor(x) for x in v)
    if isinstance(v, dict):
        items = sorted((cbor(k), cbor(x)) for k, x in v.items())
        return head(5, len(items)) + b''.join(k + x for k, x in items)
    raise TypeError(v)


def pub_of(seed):
    k = Ed25519PrivateKey.from_private_bytes(seed)
    return k, k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def frame(key, op, bridge, ctr, args):
    body = cbor([1, op, bridge, ctr, args])
    return key.sign(DOMAIN + body) + body, body


def main(out):
    k, pub = pub_of(SEED)
    k2, pub2 = pub_of(SEED2)
    me = 'ozb-98a316a7e638'
    base = 1763600000000                # ~ unix ms, 2025-11
    cases = [
        # name, signer, op, bridge, ctr, args
        ('pair_lock', k, 'pair_lock', me, base + 1, {'id': 'ozk-568afbc4ba9b'}),
        ('unpair_lock', k, 'unpair_lock', me, base + 2, {'id': 'ozk-568afbc4ba9b'}),
        ('release_lock', k, 'release_lock', me, base + 3, {'id': 'ozk-568afbc4ba9b'}),
        ('scan_both_args', k, 'scan', me, base + 4, {'target': 'ozk-568afbc4ba9b', 'duration_s': 7}),
        ('factory_reset', k, 'factory_reset', me, base + 5, None),
        ('unpair', k, 'unpair', me, base + 6, None),
        ('time', k, 'time', me, base + 7, {'utc': 1763600123}),
        ('set_site_key', k, 'set_site_key', me, base + 8, {'pub': pub2}),
        ('ctr_small', k, 'pair_lock', me, 70000, {'id': 'ozk-568afbc4ba9b'}),   # 3-byte..4-byte heads
        ('same_ctr_other', k, 'unpair_lock', me, base + 1, {'id': 'ozk-568afbc4ba9b'}),  # pair_lock's ctr, other bytes
        ('other_bridge', k, 'pair_lock', 'ozb-000000000001', base + 9, {'id': 'ozk-568afbc4ba9b'}),
        ('foreign_signer', k2, 'pair_lock', me, base + 10, {'id': 'ozk-568afbc4ba9b'}),
        ('unknown_op', k, 'launch_rockets', me, base + 11, None),
        ('version_2', k, 'pair_lock', me, base + 12, {'id': 'ozk-568afbc4ba9b'}),
    ]
    vec = {'expect_note': 'expect = the verdict on a FRESH bridge state (site key = pub, ctr floor 0, bridge = "bridge"); '
                          'order-dependent verdicts (dup, replay, rotation) are in the bridge test',
           'domain': DOMAIN.decode(), 'seed': SEED.hex(), 'pub': pub.hex(), 'seed2': SEED2.hex(),
           'pub2': pub2.hex(), 'bridge': me, 'frames': []}
    for name, key, op, bridge, ctr, args in cases:
        if name == 'version_2':
            body = cbor([2, op, bridge, ctr, args])
            pkt = key.sign(DOMAIN + body) + body
        else:
            pkt, body = frame(key, op, bridge, ctr, args)
        assert len(body) <= 512
        verdict = {'other_bridge': 'bridge', 'foreign_signer': 'sig', 'unknown_op': 'op',
                   'version_2': 'parse'}.get(name, 'accept')
        vec['frames'].append({'name': name, 'v': 2 if name == 'version_2' else 1, 'op': op, 'bridge': bridge, 'ctr': ctr,
                              'args_bstr': sorted(kk for kk, vv in (args or {}).items() if isinstance(vv, bytes)),
                              'expect': verdict,
                              'args': {kk: (vv.hex() if isinstance(vv, bytes) else vv) for kk, vv in (args or {}).items()},
                              'args_null': args is None, 'body': body.hex(), 'frame': pkt.hex()})
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'bridge_cmd_v1.json'), 'w') as f:
        json.dump(vec, f, indent=1)
        f.write('\n')
    with open(os.path.join(out, 'bridge_cmd_v1.h'), 'w') as f:
        f.write('/* GENERATED by blelock/ozcore/tools/gen_bcmd_vectors.py: do not edit. */\n')
        f.write('#ifndef BRIDGE_CMD_V1_H\n#define BRIDGE_CMD_V1_H\n#include <stdint.h>\n')
        f.write('static const char BCV_PUB[] = "%s";\nstatic const char BCV_PUB2[] = "%s";\n' % (pub.hex(), pub2.hex()))
        f.write('static const char BCV_BRIDGE[] = "%s";\n' % me)
        f.write('struct bcv { const char *name; const char *frame; const char *body; uint64_t ctr; };\n')
        f.write('static const struct bcv BCV[] = {\n')
        for fr in vec['frames']:
            f.write('    { "%s", "%s", "%s", %duLL },\n' % (fr['name'], fr['frame'], fr['body'], fr['ctr']))
        f.write('};\n#define BCV_N (sizeof BCV / sizeof BCV[0])\n#endif\n')
    print(f"{len(vec['frames'])} frames, pub {pub.hex()}")


if __name__ == '__main__':
    main(sys.argv[1])
