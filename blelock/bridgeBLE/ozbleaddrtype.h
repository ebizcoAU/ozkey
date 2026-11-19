// ozbleaddrtype.h — BRIDGE-ADDRTYPE-1 (2026-11-20): remember the BLE address
// TYPE each lock advertised with, so connect() uses it. BLEAddress(String)
// defaults to PUBLIC; the AK1037 advertises with a RANDOM address
// (SDK ble_hci.c Own_Address_Type), so a public-type connect never reaches it.
// Plain C++ (no Arduino): host-tested by test/test_addrtype.cpp.
#ifndef OZBLEADDRTYPE_H
#define OZBLEADDRTYPE_H
#include <stdint.h>
#include <string.h>

#define OZ_ADDRTYPE_SLOTS 16

struct OzAddrTypeSlot { char addr[18]; uint8_t type; uint32_t seq; };
static OzAddrTypeSlot g_addrType[OZ_ADDRTYPE_SLOTS];
static uint32_t g_addrTypeSeq;

// "aa:bb:cc:dd:ee:ff", any case. 0 = not an address (never stored or matched).
static int ozAddrTypeNorm(const char *in, char out[18])
{
    size_t n = strlen(in);
    if (n != 17)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'F')
            c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[17] = 0;
    return 1;
}

// Called from the scan callback for every OZ lock it sees.
static void ozAddrTypeNote(const char *addr, uint8_t type)
{
    char a[18];
    if (!ozAddrTypeNorm(addr, a))
        return;
    OzAddrTypeSlot *victim = &g_addrType[0];
    for (int i = 0; i < OZ_ADDRTYPE_SLOTS; i++) {
        OzAddrTypeSlot *s = &g_addrType[i];
        if (s->seq && !strcmp(s->addr, a)) { victim = s; break; }   // re-seen: update in place
        if (s->seq < victim->seq)
            victim = s;                                           // else the oldest (or an empty) slot
    }
    memcpy(victim->addr, a, sizeof a);
    victim->type = type;
    victim->seq = ++g_addrTypeSeq;
}

// The type last seen for addr, or dflt (PUBLIC: the old behaviour) if unknown.
static uint8_t ozAddrTypeOf(const char *addr, uint8_t dflt)
{
    char a[18];
    if (!ozAddrTypeNorm(addr, a))
        return dflt;
    for (int i = 0; i < OZ_ADDRTYPE_SLOTS; i++)
        if (g_addrType[i].seq && !strcmp(g_addrType[i].addr, a))
            return g_addrType[i].type;
    return dflt;
}
#endif
