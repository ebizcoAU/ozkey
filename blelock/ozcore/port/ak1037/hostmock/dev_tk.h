/* dev_tk.h (host mock): the on-chip touch-key driver calls oz_gatt_ak1037.c makes (TOUCH-KEYPAD-1). */
#ifndef DEV_TK_H_MOCK
#define DEV_TK_H_MOCK
#include <stdint.h>
typedef struct {
    uint16_t touch_dev_id, io_reset, io_intr, intr_mode;
    int16_t  sen[15];
    int16_t  sen_wk;
    void (*callback)(int fun, int key);
    void (*intr_callback)(void);
} tk_dev_param_t;
typedef struct {
    int32_t (*init)(tk_dev_param_t *param, uint32_t keymark);
    int32_t (*key_read)(void);
    int32_t (*set_sensitivity)(uint8_t ch, int16_t sen);
    uint8_t (*get_version)(uint8_t *ver);
    uint8_t (*scan_ctrl)(uint8_t able);
    int32_t (*set_wk_sen)(uint8_t ch, int16_t sen);
    uint8_t (*set_det_time)(uint16_t det_ms);
    void (*update_base)(void);
} tk_dev_opt_t;
int dev_tk_register_inner(tk_dev_opt_t *opt);
#endif
