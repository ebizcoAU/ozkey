#!/usr/bin/env python3
"""REL-PKG-1: refuse a release image that is not exactly the build it claims.

usage: relcheck.py <ozkie_app.bin> <sha7>      exit 0 = OK, 1 = REFUSED (reasons printed)
       relcheck.py --selftest

A release image must hold exactly one version string "ozkie-ak1037-<semver>+<sha7>"
for the commit being released (REL-VER-1), no "+nosha" (built without the stamp),
and none of the debug-hook names. The hook list is read from
port/ak1037/link_app_mcu.ps1 (the one list the chip link check also uses).
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LINK_PS1 = os.path.join(HERE, '..', 'port', 'ak1037', 'link_app_mcu.ps1')
VER_RE = re.compile(rb'ozkie-ak1037-(\d+\.\d+\.\d+)\+([0-9a-z]+)')


def hook_names():
    m = re.search(r"\$hooks = @\(([^)]*)\)", open(LINK_PS1).read())
    if not m:
        raise SystemExit('relcheck: no $hooks list in link_app_mcu.ps1')
    return re.findall(r"'([^']+)'", m.group(1))


def check(img, sha7):
    """Reasons to refuse, [] = OK."""
    why = []
    if not re.fullmatch(r'[0-9a-fA-F]{7}', sha7):
        return [f'"{sha7}" is not a 7-hex commit id']
    sha7 = sha7.lower()
    found = VER_RE.findall(img)
    if b'+nosha' in img:
        why.append('built without oz_build_sha.h ("+nosha")')
    if len(found) != 1:
        why.append(f'{len(found)} version strings (want exactly 1): '
                   + ', '.join(f'{v.decode()}+{s.decode()}' for v, s in found))
    elif found[0][1].decode() != sha7:
        why.append(f'image says +{found[0][1].decode()}, releasing {sha7}')
    for h in hook_names():
        if h.encode() in img:
            why.append(f'debug hook {h!r} in a release image')
    return why


def selftest():
    hooks = hook_names()
    good = b'\x00junk ozkie-ak1037-0.1.0+0c24f93\x00 more \xff'
    cases = [
        ('good image', good, '0c24f93', True),
        ('built without the stamp', b'ozkie-ak1037-0.1.0+nosha\x00', '0c24f93', False),
        ('another commit', b'ozkie-ak1037-0.1.0+1234567\x00', '0c24f93', False),
        ('no version string', b'\x00\x01 nothing here', '0c24f93', False),
        ('two versions', good + b'ozkie-ak1037-0.1.1+0c24f93\x00', '0c24f93', False),
        ('the old bring-up name', b'ozkie-ak1037-bringup\x00', '0c24f93', False),
        ('sha given in capitals still matches', good, '0C24F93', True),
        ('a short sha refused', good, '0c24f9', False),
    ]
    cases += [(f'debug hook {h!r}', good + h.encode() + b'\x00', '0c24f93', False) for h in hooks]
    fails = 0
    for name, img, sha, want_ok in cases:
        got_ok = not check(img, sha)
        if got_ok != want_ok:
            fails += 1
            print(f'FAIL {name}: ok={got_ok}, want {want_ok}')
    print(f'relcheck selftest: {len(cases)} cases ({len(hooks)} hook names), {fails} failures')
    return 1 if fails or len(hooks) < 11 else 0


def main(argv):
    if argv[1:] == ['--selftest']:
        return selftest()
    if len(argv) != 3:
        print(__doc__); return 2
    why = check(open(argv[1], 'rb').read(), argv[2])
    for w in why:
        print('REFUSED:', w)
    if not why:
        print('relcheck OK:', argv[1])
    return 1 if why else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
