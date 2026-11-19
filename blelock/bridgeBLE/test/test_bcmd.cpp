// Host test for ozbcmd.h (BRIDGE-CMD-AUTH-1), against the shared vectors.
// Run: sh test/run_bcmd.sh (from blelock/bridgeBLE)
#include <stdio.h>
#include <string.h>
#include "ozbcmd.h"
#include "bridge_cmd_v1.h"

static int fails, checks;
static void expect(const char *what, long long got, long long want)
{
    checks++;
    if (got != want) { fails++; printf("FAIL %s: %lld, want %lld\n", what, got, want); }
}

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = strlen(h) / 2;
    if (n > cap) return 0;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    return n;
}

static const struct bcv *vec(const char *name)
{
    for (size_t i = 0; i < BCV_N; i++) if (!strcmp(BCV[i].name, name)) return &BCV[i];
    printf("no vector %s\n", name); return &BCV[0];
}

static uint8_t pub[32], pub2[32];
static OzBcmdState st;
static OzBcmd cmd;

static int check(const char *name)
{
    uint8_t f[700];
    size_t n = unhex(vec(name)->frame, f, sizeof f);
    return ozBcmdCheck(&st, BCV_BRIDGE, f, n, &cmd);
}

// ozcore's writer must produce the vector body byte for byte (one encoder rule, two implementations).
static void encoder_agreement(void)
{
    struct { const char *name, *op, *id, *target; int nargs; uint32_t dur; uint64_t utc; int pub2; } e[] = {
        { "pair_lock", "pair_lock", "ozk-568afbc4ba9b", 0, 1, 0, 0, 0 },
        { "unpair_lock", "unpair_lock", "ozk-568afbc4ba9b", 0, 1, 0, 0, 0 },
        { "release_lock", "release_lock", "ozk-568afbc4ba9b", 0, 1, 0, 0, 0 },
        { "scan_both_args", "scan", 0, "ozk-568afbc4ba9b", 2, 7, 0, 0 },
        { "factory_reset", "factory_reset", 0, 0, -1, 0, 0, 0 },
        { "unpair", "unpair", 0, 0, -1, 0, 0, 0 },
        { "time", "time", 0, 0, 1, 0, 1763600123ull, 0 },
        { "set_site_key", "set_site_key", 0, 0, 1, 0, 0, 1 },
        { "ctr_small", "pair_lock", "ozk-568afbc4ba9b", 0, 1, 0, 0, 0 },
    };
    for (auto &x : e) {
        uint8_t buf[600], want[600];
        oz_cbor_w w;
        oz_cbor_w_init(&w, buf, sizeof buf);
        oz_cbor_put_array(&w, 5);
        oz_cbor_put_uint(&w, 1);
        oz_cbor_put_tstr(&w, x.op, strlen(x.op));
        oz_cbor_put_tstr(&w, BCV_BRIDGE, strlen(BCV_BRIDGE));
        oz_cbor_put_uint(&w, vec(x.name)->ctr);
        if (x.nargs < 0) {
            oz_cbor_put_null(&w);
        } else {
            oz_cbor_put_map(&w, (size_t)x.nargs);          // keys in bytewise order of their encodings
            if (x.id) { oz_cbor_put_tstr(&w, "id", 2); oz_cbor_put_tstr(&w, x.id, strlen(x.id)); }
            if (x.utc) { oz_cbor_put_tstr(&w, "utc", 3); oz_cbor_put_uint(&w, x.utc); }
            if (x.pub2) { oz_cbor_put_tstr(&w, "pub", 3); oz_cbor_put_bstr(&w, pub2, 32); }
            if (x.target) {     // bytewise key order: "target" (0x66 head) before "duration_s" (0x6a)
                oz_cbor_put_tstr(&w, "target", 6); oz_cbor_put_tstr(&w, x.target, strlen(x.target));
                oz_cbor_put_tstr(&w, "duration_s", 10); oz_cbor_put_uint(&w, x.dur);
            }
        }
        size_t n = unhex(vec(x.name)->body, want, sizeof want);
        char what[80];
        snprintf(what, sizeof what, "encoder agreement: %s", x.name);
        expect(what, !w.err && w.len == n && !memcmp(buf, want, n), 1);
    }
}

// Every vector signature is valid RFC 8032 Ed25519 (Monocypher's optional file).
static void signatures(void)
{
    for (size_t i = 0; i < BCV_N; i++) {
        uint8_t f[700], m[700];
        size_t n = unhex(BCV[i].frame, f, sizeof f);
        memcpy(m, OZB_DOMAIN, OZB_DOMAIN_LEN);
        memcpy(m + OZB_DOMAIN_LEN, f + 64, n - 64);
        const uint8_t *key = strcmp(BCV[i].name, "foreign_signer") ? pub : pub2;
        char what[80];
        snprintf(what, sizeof what, "RFC 8032 signature verifies: %s", BCV[i].name);
        expect(what, crypto_ed25519_check(f, key, m, OZB_DOMAIN_LEN + n - 64), 0);
    }
}

static void rules(void)
{
    const uint64_t base = 1763600000000ull;
    memset(&st, 0, sizeof st);
    expect("no site key: refused", check("pair_lock"), OZB_E_NOKEY);

    ozBcmdProvision(&st, pub, 0);
    expect("pair_lock accepted", check("pair_lock"), OZB_OK);
    expect("…op", cmd.op, OZB_OP_PAIR_LOCK);
    expect("…lock id", !strcmp(cmd.id, "ozk-568afbc4ba9b"), 1);
    expect("…ctr", (long long)cmd.ctr, (long long)(base + 1));
    expect("…persist before acting", cmd.persist, 1);
    ozBcmdSaved(&st, cmd.ctr);
    expect("the same bytes again: a silent duplicate", check("pair_lock"), OZB_DUP);
    expect("another frame at the same ctr: refused (not a duplicate)", check("same_ctr_other"), OZB_E_REPLAY);
    expect("unpair_lock (newer) accepted", check("unpair_lock"), OZB_OK);
    expect("…op", cmd.op, OZB_OP_UNPAIR_LOCK);
    ozBcmdSaved(&st, cmd.ctr);
    expect("the older pair_lock replayed: refused", check("pair_lock"), OZB_E_REPLAY);
    expect("a far older ctr: refused", check("ctr_small"), OZB_E_REPLAY);
    expect("release_lock accepted", check("release_lock"), OZB_OK);
    ozBcmdSaved(&st, cmd.ctr);
    expect("scan accepted", check("scan_both_args"), OZB_OK);
    expect("…target", !strcmp(cmd.id, "ozk-568afbc4ba9b"), 1);
    expect("…duration", cmd.duration_s, 7);
    ozBcmdSaved(&st, cmd.ctr);
    expect("factory_reset (args null) accepted", check("factory_reset"), OZB_OK);
    expect("…op", cmd.op, OZB_OP_FACTORY_RESET);
    ozBcmdSaved(&st, cmd.ctr);
    expect("unpair accepted", check("unpair"), OZB_OK);
    ozBcmdSaved(&st, cmd.ctr);
    expect("time accepted", check("time"), OZB_OK);
    expect("…utc", (long long)cmd.utc, 1763600123ll);
    expect("…ctr in RAM only (< 24 h above NVS)", cmd.persist, 0);
    expect("time replayed: a duplicate, never re-applied", check("time"), OZB_DUP);
    expect("a command for another bridge: refused", check("other_bridge"), OZB_E_BRIDGE);
    expect("signed by another key: refused", check("foreign_signer"), OZB_E_SIG);
    expect("an unknown op: refused", check("unknown_op"), OZB_E_OP);
    expect("version 2: refused", check("version_2"), OZB_E_PARSE);

    // The time ctr is saved once it runs 24 h ahead of NVS.
    OzBcmdState t = st;
    st.nvs_last = 0;
    expect("time ctr > 24 h above NVS: persisted", (ozBcmdProvision(&st, pub, 0), check("time")), OZB_OK);
    expect("…persist", cmd.persist, 1);
    st = t;

    // Tampered / truncated / oversized frames.
    uint8_t f[700];
    size_t n = unhex(vec("set_site_key")->frame, f, sizeof f);
    f[n - 1] ^= 1;
    expect("one body bit flipped: refused (signature)", ozBcmdCheck(&st, BCV_BRIDGE, f, n, &cmd), OZB_E_SIG);
    f[n - 1] ^= 1;
    f[3] ^= 0x80;
    expect("one signature bit flipped: refused", ozBcmdCheck(&st, BCV_BRIDGE, f, n, &cmd), OZB_E_SIG);
    f[3] ^= 0x80;
    expect("a bare signature (no body): refused", ozBcmdCheck(&st, BCV_BRIDGE, f, 64, &cmd), OZB_E_PARSE);
    static uint8_t big[64 + OZB_BODY_MAX + 1];
    expect("a body over 512 B: refused", ozBcmdCheck(&st, BCV_BRIDGE, big, sizeof big, &cmd), OZB_E_PARSE);

    // Rotation: set_site_key, then the old key no longer works, the new one does.
    expect("set_site_key accepted", ozBcmdCheck(&st, BCV_BRIDGE, f, n, &cmd), OZB_OK);
    expect("…the new key", !memcmp(cmd.new_pub, pub2, 32), 1);
    expect("…persisted before acting", cmd.persist, 1);
    ozBcmdSaved(&st, cmd.ctr);
    ozBcmdRotate(&st, cmd.new_pub);
    expect("after rotation: the new key's frame accepted", check("foreign_signer"), OZB_OK);
    ozBcmdSaved(&st, cmd.ctr);
    ozBcmdProvision(&st, pub2, 0);
    expect("after rotation: an old-key frame refused", (ozBcmdRotate(&st, pub2), check("pair_lock")), OZB_E_SIG);

    // tz (PM 2026-11-20): a signed op with time's counter rules.
    ozBcmdProvision(&st, pub, base + 12);
    st.nvs_last = base + 12;
    expect("tz (east) accepted", check("tz_east"), OZB_OK);
    expect("…minutes", cmd.tz_min, 420);
    expect("…RAM only, like time", cmd.persist, 0);
    expect("tz (west, a negative int) accepted", check("tz_west"), OZB_OK);
    expect("…minutes", cmd.tz_min, -300);
    expect("tz out of range (900): refused", check("tz_out_of_range"), OZB_E_PARSE);

    // The commissioning floor: commands from before it never verify again.
    ozBcmdProvision(&st, pub, base + 100);
    expect("below the commissioning floor: refused", check("unpair_lock"), OZB_E_REPLAY);
}

int main()
{
    unhex(BCV_PUB, pub, 32);
    unhex(BCV_PUB2, pub2, 32);
    encoder_agreement();
    signatures();
    rules();
    printf("bcmd: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
