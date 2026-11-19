// Host test for ozbleaddrtype.h (BRIDGE-ADDRTYPE-1). Build + run:
//   c++ -std=c++11 -Wall -Wextra -Werror -I.. test_addrtype.cpp -o /tmp/t && /tmp/t
#include <stdio.h>
#include "ozbleaddrtype.h"
static int fails, checks;
static void expect(const char *what, int got, int want)
{
    checks++;
    if (got != want) { fails++; printf("FAIL %s: %d, want %d\n", what, got, want); }
}
int main()
{
    const uint8_t PUB = 0, RND = 1;
    expect("unknown address: the default (public)", ozAddrTypeOf("aa:bb:cc:dd:ee:ff", PUB), PUB);
    ozAddrTypeNote("56:8a:fb:c4:ba:9b", RND);           /* the AK1037 */
    expect("the AK1037 connects RANDOM", ozAddrTypeOf("56:8a:fb:c4:ba:9b", PUB), RND);
    ozAddrTypeNote("ac:eb:e6:39:f8:c4", PUB);           /* an old public-address lock */
    expect("a public lock stays public", ozAddrTypeOf("ac:eb:e6:39:f8:c4", RND), PUB);
    ozAddrTypeNote("56:8a:fb:c4:ba:9b", PUB);           /* the same address re-seen: latest wins */
    expect("re-seen: the latest type", ozAddrTypeOf("56:8a:fb:c4:ba:9b", RND), PUB);
    ozAddrTypeNote("56:8a:fb:c4:ba:9b", RND);
    char a[18];
    for (int i = 0; i < OZ_ADDRTYPE_SLOTS + 4; i++) {   /* more addresses than slots */
        snprintf(a, sizeof a, "00:00:00:00:00:%02x", i);
        ozAddrTypeNote(a, RND);
    }
    expect("full: the newest kept", ozAddrTypeOf("00:00:00:00:00:13", PUB), RND);
    expect("full: the oldest evicted -> default", ozAddrTypeOf("00:00:00:00:00:00", PUB), PUB);
    expect("case-insensitive (scan vs paired strings)", (ozAddrTypeNote("AB:CD:EF:00:11:22", RND),
           ozAddrTypeOf("ab:cd:ef:00:11:22", PUB)), RND);
    expect("a too-long string is ignored, not truncated into a false match",
           (ozAddrTypeNote("11:22:33:44:55:66:77", RND), ozAddrTypeOf("11:22:33:44:55:66", PUB)), PUB);
    printf("addrtype: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
