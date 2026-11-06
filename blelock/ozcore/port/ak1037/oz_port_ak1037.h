/* oz_port_ak1037 — AK1037 port internals that the self-test image checks
 * directly (ozkey-49 P4 item 4b). Not for the lock's own code paths. */
#ifndef OZ_PORT_AK1037_H
#define OZ_PORT_AK1037_H

#include <stdint.h>

/* 1 = the SKE engine passed FIPS-197 C.1 and is in use, -1 = software AES.
 * Runs the known-answer test first if nothing has used AES yet. */
int oz_ak1037_ske_state(void);
/* One AES-128 block through the engine, never the software fallback. */
int oz_ak1037_ske_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* One raw TRNG word, before conditioning. */
int oz_ak1037_trng_word(uint32_t *w);
/* Mount the OZKV partition now (oz_port_kv_* otherwise mount on first use). */
int oz_ak1037_kv_mount(void);

/* Who uses kv this boot. The self-test wipes its namespace between cases and
 * the lock owns "oz", so the bring-up commands allow one owner per boot.
 * OZ_OK if kv was free or already this owner's, OZ_E_ARG otherwise. */
enum { OZ_AK1037_KV_SELFTEST = 1, OZ_AK1037_KV_LOCK = 2 };
int oz_ak1037_kv_claim(int owner);
/* The current owner, 0 = none yet. */
int oz_ak1037_kv_owner(void);

/* The battery in mV from the SAR ADC (P5-F). Channel, divider and reference
 * are placeholders until OZ-7 5a: OZ_BATT_AIN, OZ_BATT_DIV, OZ_BATT_VREF_MV.
 * 0 when the ADC reports an error (oz_power ignores it). */
uint16_t oz_ak1037_battery_mv(void);

/* Self-test only: move oz_port_monotonic_ms() forward (the suites step past
 * keypad lockouts). The kv owner rule keeps the lock from starting in a boot
 * that ran the self-test, so the lock never runs on a moved clock. */
void oz_ak1037_clock_advance(uint64_t ms);

#endif
