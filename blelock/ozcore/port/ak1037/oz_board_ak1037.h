/* oz_board_ak1037.h — BOARD-PROFILE-1 (PM nexus-1b sign-off 2026-11-20).
 *
 * Every hardware fact of one lock model in ONE record, so one firmware can
 * serve every model (LOCK-MODELS-1 §1). Today: a built-in table with one
 * board, the EVB_MBD_AK1037D bench kit; later a per-model record from a board
 * partition written at the factory burn (after REL-PKG-1 and Anyka Q14),
 * signed with secure boot (Q9).
 *
 * The rule: a record oz_board_valid() rejects never drives the motor; the
 * motor pins are not even configured (a wrong pin map could short an H-bridge
 * or drive a switch input).
 *
 * Not here: battery sensing on/off (OZ_BATT_SENSE) depends on the IMAGE (the
 * bench debug image may run without VOL_DET wired), not on the board.
 */
#ifndef OZ_BOARD_AK1037_H
#define OZ_BOARD_AK1037_H

#include <stdint.h>
#include "hal_gpio.h"
#include "oz_motor.h"
#include "oz_doorio.h"

#define OZ_BOARD_BL_PINS 8      /* backlight pins, indices into the port's bl_pin[] */
#define OZ_BOARD_PADS    15     /* touch pad bits (TOUCH-MAP-1) */

typedef struct {
    int8_t  pin;                /* 0..OZ_BOARD_BL_PINS-1, -1 = no LED */
    uint8_t state;              /* OZ_BL_HIGH / OZ_BL_LOW */
} oz_board_bl;

typedef struct {
    const char   *model;        /* INFO key 5 / dev/info key 2 (REL-VER-1) */
    uint16_t      profile_ver;
    /* actuator: A high + B low one way, A low + B high the other, both high brake */
    T_eGPIO       motor_a, motor_b;
    oz_motor_cfg  motor;        /* timings; drive_ms <= OZ_MOTOR_MAX_DRIVE_MS */
    /* battery: pack = raw x vref_mv x div / 4095 */
    uint32_t      batt_ain;     /* SARADC_SEL_AINn */
    uint8_t       batt_div;
    uint16_t      batt_vref_mv; /* effective full scale (BATT-CAL-1) */
    /* door inputs, pressed = LOW: [OZ_DIO_SW_TAMPER], [OZ_DIO_SW_DOOR], [OZ_DIO_SW_EXIT] */
    T_eGPIO       sw_pin[OZ_DIO_SW_N];
    oz_doorio_cfg doorio;       /* policy defaults */
    oz_board_bl   bl_key[OZ_BOARD_PADS];
} oz_board;

/* Why a record is refused; 0 = valid. */
enum {
    OZ_BOARD_OK = 0, OZ_BOARD_E_NULL, OZ_BOARD_E_MODEL, OZ_BOARD_E_MOTOR_PINS,
    OZ_BOARD_E_MOTOR_TIME, OZ_BOARD_E_BATT, OZ_BOARD_E_SW_PINS, OZ_BOARD_E_BL
};

extern const oz_board OZ_BOARD_EVB_AK1037D;

/* The active board (today always the EVB), and the check. */
const oz_board *oz_ak1037_board(void);
int oz_board_valid(const oz_board *b);
/* Make b the active board (the future partition loader; host tests). */
void oz_ak1037_board_set(const oz_board *b);

#endif
