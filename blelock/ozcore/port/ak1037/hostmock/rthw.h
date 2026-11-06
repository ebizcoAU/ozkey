/* rthw.h (host mock). "Interrupts off" is a nesting counter the mock checks. */
#ifndef RTHW_H_MOCK
#define RTHW_H_MOCK
#include "rtthread.h"
rt_base_t rt_hw_interrupt_disable(void);
void      rt_hw_interrupt_enable(rt_base_t level);
#endif
