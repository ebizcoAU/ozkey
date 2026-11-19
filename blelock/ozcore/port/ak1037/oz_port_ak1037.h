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

/* The motor board's two inputs (MOTOR-GPIO-1): set both LOW as outputs (call
 * at boot), and drive them: (1,0) / (0,1) turn, (1,1) brake, (0,0) stop. */
void oz_ak1037_motor_pins_init(void);
void oz_ak1037_motor_pins(int a, int b);

/* The keypad backlight (TOUCH-LED-1, BACKLIGHT-2): pins released (all dark),
 * or driven high / low (each lights one LED of every pair). */
enum { OZ_BL_OFF = 0, OZ_BL_HIGH, OZ_BL_LOW };

/* The key beep (BEEP-1): amplifier on once; a beep starts (1, or 0 if one is
 * playing or the DAC refused) and the caller stops it after OZ_BEEP_MS. */
#define OZ_BEEP_MS 80u
void oz_ak1037_speaker_init(void);

/* The door switches (LOCK-IO-1): tamper, door, exit; 1 = pressed. */
#include "oz_doorio.h"
void oz_ak1037_switches_init(void);
void oz_ak1037_switches(uint8_t pressed[OZ_DIO_SW_N]);
int  oz_ak1037_beep_start(void);
void oz_ak1037_beep_stop(void);
void oz_ak1037_backlight_init(void);
void oz_ak1037_backlight(int state);
/* One pin (0..7) only, for mapping keys to pins on the bench: the GPIO number,
 * or -1 for a bad index (LED-MAP-1). */
int  oz_ak1037_backlight_pin(int i, int state);
/* PER-KEY-BL-1: light only the key of this touch pad (its one-bit mask):
 * every other pin released. 1 = lit, 0 = no LED known for it (all dark). */
int  oz_ak1037_backlight_pad(int pad);

/* Battery sensing (BATT-SENSE-1): 0 on the bench build only; the raw ADC and
 * its pin voltage, for measuring the divider (oz.batt). */
int      oz_ak1037_battery_sensing(void);
void     oz_ak1037_battery_note(void);
int      oz_ak1037_battery_raw(void);
uint16_t oz_ak1037_battery_pin_mv(int raw);

#endif
