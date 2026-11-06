/* Invite MAC against ftpos's frozen vectors (member_invite_test.dart,
 * "frozen vector (XF-46 §7.1 / XF-87 v2 — firmware mirrors byte-for-byte)"),
 * plus the CBOR rendering and the v3 guest-key-bound form. */
#include "oz_invite.h"
#include "oz_test.h"

static void base(oz_invite *inv, uint8_t version)
{
    memset(inv, 0, sizeof *inv);
    inv->version = version;
    strcpy(inv->device_id, "ozk-a4cf12879da7");
    memset(inv->issuer, 0xaa, 32);
    inv->role = OZ_INVITE_ROLE_MEMBER;
    strcpy(inv->label, "Ba Ngoai");
    memset(inv->nonce, 0x42, 16);
    inv->expires = 1789000000;
}

int main(void)
{
    uint8_t secret[32], mac[32];
    for (int i = 0; i < 32; i++)
        secret[i] = (uint8_t)(i + 1);
    oz_invite inv, back;

    base(&inv, 1);
    oz_invite_compute_mac(&inv, secret, mac);
    expect("ftpos frozen v1 MAC", mac, 32,
           "e7780baea8feef5674c0ffecd1b83f35dfd9198db50cea6d0735c7a43d268aac");

    base(&inv, 2);
    oz_invite_compute_mac(&inv, secret, mac);
    expect("ftpos frozen v2 MAC (permanent = 0)", mac, 32,
           "9f47eebb937640cbe1f32ae17cca1908cdbba63f3d770c2558e7fa8f32e08ca9");
    memcpy(inv.mac, mac, 32);
    expect_int("v2 verifies", oz_invite_verify(&inv, secret), 0);

    /* Every signed field matters. */
    oz_invite t = inv;
    t.membership_expires = 2000000000;
    expect_int("membership expiry is signed", oz_invite_verify(&t, secret), OZ_E_AUTH);
    t = inv; strcpy(t.label, "Attacker");
    expect_int("label is signed", oz_invite_verify(&t, secret), OZ_E_AUTH);
    t = inv; t.role = OZ_INVITE_ROLE_ADMIN;
    expect_int("role is signed", oz_invite_verify(&t, secret), OZ_E_AUTH);
    uint8_t wrong[32];
    memset(wrong, 9, 32);
    expect_int("other issuer secret", oz_invite_verify(&inv, wrong), OZ_E_AUTH);

    /* CBOR round trip. */
    uint8_t cb[256];
    size_t cl;
    expect_int("to cbor", oz_invite_to_cbor(&inv, cb, sizeof cb, &cl), 0);
    expect_int("from cbor", oz_invite_from_cbor(&back, cb, cl), 0);
    expect_int("round trip verifies", oz_invite_verify(&back, secret), 0);
    printf("v2 invite as CBOR: %zu bytes\n", cl);

    /* v3: bound to one app key and naming the issuing bond. */
    uint8_t guest_pub[32];
    memset(guest_pub, 0x5a, 32);
    base(&inv, 3);
    inv.membership_expires = 1789600000;
    oz_invite_bind_of(guest_pub, inv.bind);
    inv.iss = 4;
    oz_invite_compute_mac(&inv, secret, inv.mac);
    expect_int("v3 verifies", oz_invite_verify(&inv, secret), 0);
    t = inv; t.bind[0] ^= 1;
    expect_int("bind is signed", oz_invite_verify(&t, secret), OZ_E_AUTH);
    t = inv; t.iss = 5;
    expect_int("iss is signed", oz_invite_verify(&t, secret), OZ_E_AUTH);
    oz_invite_to_cbor(&inv, cb, sizeof cb, &cl);
    expect_int("v3 from cbor", oz_invite_from_cbor(&back, cb, cl), 0);
    expect_int("v3 round trip verifies", oz_invite_verify(&back, secret), 0);
    printf("v3 invite as CBOR: %zu bytes; MAC ", cl);
    for (int i = 0; i < 32; i++) printf("%02x", inv.mac[i]);
    printf("  (vector for ftpos)\n");

    /* Malformed. */
    strcpy(inv.label, "a|b");
    oz_invite_to_cbor(&inv, cb, sizeof cb, &cl);
    expect_int("'|' in a label refused", oz_invite_from_cbor(&back, cb, cl), OZ_E_ARG);
    cb[1] = 0x09;   /* version 9 */
    expect_int("unknown version refused", oz_invite_from_cbor(&back, cb, cl), OZ_E_ARG);

    return report("invite");
}
