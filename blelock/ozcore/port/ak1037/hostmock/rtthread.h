/* rtthread.h (host mock) — the RT-Thread 3.1 subset port/ak1037 uses. Threads
 * are pthreads run in lockstep with the test: see mock_sdk.h. */
#ifndef RTTHREAD_H_MOCK
#define RTTHREAD_H_MOCK

#include "ak_mock_types.h"

typedef long          rt_base_t;
typedef long          rt_err_t;
typedef int32_t       rt_int32_t;
typedef uint8_t       rt_uint8_t;
typedef uint32_t      rt_uint32_t;
typedef uint32_t      rt_tick_t;
typedef unsigned long rt_size_t;

#define RT_EOK      0
#define RT_ERROR    1
#define RT_ETIMEOUT 2
#define RT_EFULL    3

#define RT_WAITING_FOREVER     (-1)
#define RT_IPC_FLAG_FIFO       0x00
#define RT_TICK_PER_SECOND     100      /* rtconfig.h */
#define RT_THREAD_PRIORITY_MAX 8        /* rtconfig.h */

struct rt_thread {
    char   name[8];
    void (*entry)(void *);
    void  *param;
    void  *stack;
    size_t stack_size;
    int    started;
    int    finished;
};
typedef struct rt_thread *rt_thread_t;

struct rt_messagequeue {
    char       name[8];
    uint8_t   *pool;
    rt_size_t  msg_size;                /* aligned, as RT-Thread stores it */
    rt_size_t  max;
    rt_size_t  head, count;
    rt_size_t *lens;
};
typedef struct rt_messagequeue *rt_mq_t;

struct rt_mutex {
    int   init;
    void *impl;
};
typedef struct rt_mutex *rt_mutex_t;

rt_err_t  rt_thread_init(struct rt_thread *thread, const char *name, void (*entry)(void *parameter),
                         void *parameter, void *stack_start, rt_uint32_t stack_size,
                         rt_uint8_t priority, rt_uint32_t tick);
rt_err_t  rt_thread_startup(rt_thread_t thread);
rt_err_t  rt_thread_mdelay(rt_int32_t ms);
rt_tick_t rt_tick_get(void);
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms);

rt_err_t rt_mq_init(rt_mq_t mq, const char *name, void *msgpool, rt_size_t msg_size,
                    rt_size_t pool_size, rt_uint8_t flag);
rt_err_t rt_mq_send(rt_mq_t mq, void *buffer, rt_size_t size);
rt_err_t rt_mq_recv(rt_mq_t mq, void *buffer, rt_size_t size, rt_int32_t timeout);

rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag);
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time);
rt_err_t rt_mutex_release(rt_mutex_t mutex);

#endif
