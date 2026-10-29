/* oz_selftest — every host test suite in one call (ozkey-49 P4 item 4b).
 *
 * Each tests/test_<name>.c is compiled with -Dmain=oz_t_<name>, so its
 * statics stay its own and only oz_t_<name> is exported. The host builds
 * this as one more ctest; the AK1037 self-test image calls it on silicon.
 * The suites' check counters are per boot: run once, then reboot. */
#ifndef OZ_SELFTEST_H
#define OZ_SELFTEST_H

/* Runs every suite, prints one line each. Returns the number that failed. */
int oz_selftest_suites(void);

#endif
