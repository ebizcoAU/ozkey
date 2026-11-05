#!/usr/bin/env python3
"""blerun.py — ozapp's interop scenario against a REAL OZKIE v2 lock over BLE
(ozkey-55 F5 part 2, direct BLE; the bridge-relay half needs F1).

  python3 -I blerun.py <device_id> <lock serial port>

  e.g.  blerun.py ozk-acebe63acab8 /dev/cu.usbmodem1431401

The lock must be UNCLAIMED (fresh, or `oz.wipe CONFIRM` on its shell). The
serial port stands in for a person at the door (pairing code, keypad PIN).
🔴 Opening a C6 lock's USB serial resets it, so the port is opened ONCE, before
the first BLE connection, and kept open for the whole run.

Steps that only the host lock can do (the relay link, a process restart as a
reboot) are not here; the relay is tested through the bridge once F1 exists.
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ozapp import OzApp, R, uncbor, mint_invite      # noqa: E402
from blelink import BleLockLink                       # noqa: E402


def main(dev, port):
    import serial as pyserial
    ser = pyserial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    time.sleep(3.0)                      # the open reset the lock: let it boot
    ser.reset_input_buffer()

    fails = 0

    def chk(name, cond):
        nonlocal fails
        fails += 0 if cond else 1
        print(f"  {'ok  ' if cond else 'FAIL'} {name}", flush=True)

    def link():
        for attempt in range(3):         # the lock re-advertises after a disconnect
            try:
                return BleLockLink(dev, serial=ser, scan_s=15.0)
            except RuntimeError as e:
                if 'wrong lock' in str(e) or attempt == 2:
                    raise
                time.sleep(2)

    owner, member = OzApp('owner'), OzApp('member')

    k = link()
    print(f'connected, ATT MTU {k.mtu}', flush=True)
    k.send(0x82, b''); _, iv = k.recv()
    info = uncbor(iv)[0]
    lock_pub = info[3]
    chk('info: device_id matches', info[1] == dev)
    chk('info: unclaimed', info[4] == 0)

    # First pairing (§3.4): the app computes the code, the "owner" types it.
    chk('pairing handshake', owner.connect(k, lock_pub) is None)
    owner.mid += 1
    req, x = owner.osc.protect(0x02, 'pair', mid=owner.mid)
    k.send(1, req)
    k.send(0x80, owner.e.pair_code().to_bytes(4, 'big'))
    t, p = k.recv(timeout=5.0)
    ok = p is not None
    if ok:
        code, _, res = owner.osc.unprotect(p, x)
        ok = code == 0x44 and res[0] == R['BOND_OK']
    chk('BOND_OK after keypad code', ok)
    owner.slot = 0
    rc, res = owner.call(0x02, 'lock/unlock')
    chk('owner unlocks', res and res[0] == R['UNLOCK_OK'])
    rc, res = owner.call(0x03, 'time', {1: int(time.time())})
    chk('owner sets the clock', res and res[0] == R['TIME_OK'])
    k.close()

    # Member by invite, redeemed in EAD_3.
    ead = mint_invite(owner, lock_pub, dev, os.urandom(16), version=2)
    k = link()
    chk('member redeems invite', member.connect(k, lock_pub, ead) is None)
    rc, res = member.call(0x02, 'lock/unlock')
    chk('member unlocks at the door', res and res[0] == R['UNLOCK_OK'])
    k.close()
    k = link()
    chk('replayed invite refused', OzApp('thief').connect(k, lock_pub, ead) == 'MEMBER_REPLAY')
    k.close()

    # Credentials with the epoch rule, then the PIN at the keypad.
    k = link(); owner.connect(k, lock_pub)
    rc, res = owner.call(0x01, 'bond')
    epoch = res[1]
    rc, res = owner.call(0x02, 'cred', {0: epoch, 1: 1, 3: b'135790'})
    chk('add PIN', res and res[0] == R['CRED_OK'])
    rc, res = owner.call(0x02, 'cred', {0: epoch, 1: 1, 3: b'135790'})
    chk('stale epoch refused', res and res[0] == R['STALE_EPOCH'])
    epoch = res[1]
    rc, res = owner.call(0x02, 'cred', {0: epoch, 1: 1, 3: b'1357'})
    chk('4-digit PIN refused (35 PIN_TOO_SHORT)', res and res[0] == 35)
    k.close()
    k = link(); k.send(0x81, b'135790'); _, p = k.recv(timeout=4.0); k.close()
    chk('PIN opens the door', p == b'\x01')

    ser.close()
    print('PASS' if fails == 0 else f'FAIL ({fails})', flush=True)
    return fails == 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(2)
    sys.exit(0 if main(sys.argv[1], sys.argv[2]) else 1)
