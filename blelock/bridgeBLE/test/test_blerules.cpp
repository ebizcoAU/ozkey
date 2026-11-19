// Host test for ozblerules.h (BRIDGE-HOST-SUITE-1 step 2).
#include <stdio.h>
#include <string>
#include <vector>
#include "ozblerules.h"

static int fails, checks;
static void expect(const char *what, long long got, long long want)
{
    checks++;
    if (got != want) { fails++; printf("FAIL %s: %lld, want %lld\n", what, got, want); }
}

static const char *P = "ozkie/lab/bridges/ozb-98a316a7e638/ozkie/";

static int dn(const char *topic, std::vector<uint8_t> p, const char **lock = nullptr)
{
    const char *l;
    int r = oz2DownlinkCheck(topic, P, p.data(), p.size(), 1024, 2, &l);
    if (lock) *lock = l;
    return r;
}

static void downlink(void)
{
    const char *lock = "";
    std::vector<uint8_t> ok = { 0, 0xaa };
    expect("downlink: a lock's topic, type 0, 1 B", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-568afbc4ba9b", ok, &lock), OZ2_DN_OK);
    expect("…the lock id from the topic", std::string(lock) == "ozk-568afbc4ba9b", 1);
    expect("another bridge's topic: not ours", dn("ozkie/lab/bridges/ozb-000000000001/ozkie/ozk-568afbc4ba9b", ok), OZ2_DN_NOT_OURS);
    expect("the command topic: not ours", dn("ozkie/lab/bridges/ozb-98a316a7e638/cmd", ok), OZ2_DN_NOT_OURS);
    expect("no lock after the prefix: bad topic", dn(P, ok), OZ2_DN_BAD_TOPIC);
    expect("a deeper topic: bad topic", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-568afbc4ba9b/x", ok), OZ2_DN_BAD_TOPIC);
    expect("1 byte (type only): bad length", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", { 1 }), OZ2_DN_BAD_LEN);
    expect("0 bytes: bad length", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", {}), OZ2_DN_BAD_LEN);
    std::vector<uint8_t> max(1 + 1024, 0), over(1 + 1025, 0);
    expect("1 + 1024 B: the largest accepted", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", max), OZ2_DN_OK);
    expect("1 + 1025 B: bad length", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", over), OZ2_DN_BAD_LEN);
    expect("type 2 (error): accepted", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", { 2, 0 }), OZ2_DN_OK);
    expect("type 3: bad type (closed set)", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", { 3, 0 }), OZ2_DN_BAD_TYPE);
    expect("type 255: bad type", dn("ozkie/lab/bridges/ozb-98a316a7e638/ozkie/ozk-1", { 255, 0 }), OZ2_DN_BAD_TYPE);
}

static void emit(const char *id, size_t n, void *ctx) { ((std::vector<std::string> *)ctx)->push_back(std::string(id, n)); }
static std::vector<std::string> split(const char *blob, unsigned max)
{
    std::vector<std::string> v;
    unsigned n = ozPairBlobSplit(blob, max, emit, &v);
    if (n != v.size()) v.push_back("<count mismatch>");
    return v;
}

static void allowlist(void)
{
    auto v = split("ozk-a\nozk-b\n", 9);
    expect("blob: two ids, trailing newline", v.size() == 2 && v[0] == "ozk-a" && v[1] == "ozk-b", 1);
    v = split("ozk-a\nozk-b", 9);
    expect("blob: no trailing newline", v.size() == 2 && v[1] == "ozk-b", 1);
    v = split("", 9);
    expect("blob: empty = unpaired", v.size(), 0);
    v = split("\n\nozk-a\n\n", 9);
    expect("blob: empty lines skipped", v.size() == 1 && v[0] == "ozk-a", 1);
    v = split("a\nb\nc\nd\n", 3);
    expect("blob: at most max ids", v.size() == 3 && v[2] == "c", 1);
    v = split("ozk-568afbc4ba9b\n", 9);
    expect("blob: a real id", v.size() == 1 && v[0] == "ozk-568afbc4ba9b", 1);
}

static void threadfail(void)
{
    expect("attached, Wi-Fi up: attached", ozThreadAttachOutcome(1, 1), OZ_TH_ATTACHED);
    expect("attached, no Wi-Fi (Thread uplink): attached", ozThreadAttachOutcome(1, 0), OZ_TH_ATTACHED);
    expect("NOT attached, Wi-Fi up: run without Thread (THREADFAIL-1)", ozThreadAttachOutcome(0, 1), OZ_TH_RUN_WITHOUT);
    expect("NOT attached, no Wi-Fi: fail closed (the re-arm path)", ozThreadAttachOutcome(0, 0), OZ_TH_FAIL_CLOSED);
    expect("late attach: operational, watching, attached now", ozThreadLateAttachDue(1, 1, 1), 1);
    expect("late attach: still detached", ozThreadLateAttachDue(1, 1, 0), 0);
    expect("late attach: not watching (attached at boot)", ozThreadLateAttachDue(1, 0, 1), 0);
    expect("late attach: not operational (forming / re-armed)", ozThreadLateAttachDue(0, 1, 1), 0);
}

static void tdetach(void)
{
    expect("tdetach 120", ozTdetachSeconds("120"), 120);
    expect("tdetach 1 (min)", ozTdetachSeconds("1"), 1);
    expect("tdetach 3600 (max)", ozTdetachSeconds("3600"), 3600);
    expect("tdetach 3601: refused", ozTdetachSeconds("3601"), 0);
    expect("tdetach 0: refused", ozTdetachSeconds("0"), 0);
    expect("tdetach empty: refused", ozTdetachSeconds(""), 0);
    expect("tdetach -5: refused", ozTdetachSeconds("-5"), 0);
    expect("tdetach 12x: refused", ozTdetachSeconds("12x"), 0);
    expect("tdetach 99999999999999999999 (overflow): refused", ozTdetachSeconds("99999999999999999999"), 0);
    expect("re-enable: not holding", ozTdetachReenableDue(0, 5000, 0, 1000), 0);
    expect("re-enable: before the hold ends", ozTdetachReenableDue(1, 999, 0, 1000), 0);
    expect("re-enable: at the hold's end", ozTdetachReenableDue(1, 1000, 0, 1000), 1);
    expect("re-enable: across a millis() wrap, not yet", ozTdetachReenableDue(1, 100UL, 0xFFFFFF00UL, 1000), 0);
    expect("re-enable: across a millis() wrap, due", ozTdetachReenableDue(1, 0x2F0UL, 0xFFFFFF00UL, 1000), 1);
}

int main()
{
    tdetach();
    downlink();
    allowlist();
    threadfail();
    printf("blerules: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
