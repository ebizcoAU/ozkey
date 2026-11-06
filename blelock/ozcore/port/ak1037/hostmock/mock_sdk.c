/* mock_sdk — see mock_sdk.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtthread.h"
#include "rthw.h"
#include "com_ble.h"
#include "com_msh.h"
#include "hal_ske.h"
#include "hal_trng.h"
#include "hal_saradc.h"
#include "hal_sys.h"
#include "hal_sfc_norflash.h"
#include "hal_partition.h"
#include "oz_sw.h"
#include "mock_sdk.h"

/* ── time, interrupts ──────────────────────────────────────────────────── */

uint32_t mock_ms;
long mock_irq_violations;
static int irq_depth;

unsigned int get_ms_tick_func(void) { return mock_ms; }
rt_tick_t rt_tick_get(void) { return mock_ms / (1000u / RT_TICK_PER_SECOND); }

/* RT-Thread 3.1's rounding: up to the next tick. */
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
    if (ms < 0)
        return (rt_tick_t)RT_WAITING_FOREVER;
    return (rt_tick_t)(RT_TICK_PER_SECOND * (ms / 1000) + (RT_TICK_PER_SECOND * (ms % 1000) + 999) / 1000);
}

rt_err_t rt_thread_mdelay(rt_int32_t ms)
{
    mock_ms += (uint32_t)ms;
    return RT_EOK;
}

rt_base_t rt_hw_interrupt_disable(void) { return irq_depth++; }
void rt_hw_interrupt_enable(rt_base_t level) { irq_depth = (int)level; }

/* ── SKE: AES-128 ECB in software, or a broken engine ──────────────────── */

int mock_ske_mode;
long mock_ske_blocks;
uint8_t mock_ske_key_reg[16];

uint32_t hal_ske_init(SKE_ALG alg, SKE_MODE mode, SKE_CRYPTO crypto, uint8_t *key, uint8_t sp_key_idx, uint8_t *iv)
{
    (void)iv;
    if (irq_depth == 0)
        mock_irq_violations++;
    if (alg != SKE_ALG_AES_128 || mode != SKE_MODE_ECB || crypto != SKE_CRYPTO_ENCRYPT || sp_key_idx || !key)
        return 1;
    memcpy(mock_ske_key_reg, key, 16);
    return SKE_SUCCESS;
}

uint32_t hal_ske_update_blocks(uint8_t *in, uint8_t *out, uint32_t byteLen)
{
    if (irq_depth == 0)
        mock_irq_violations++;
    if (mock_ske_mode == MOCK_SKE_ERROR || byteLen % 16)
        return 1;
    for (uint32_t i = 0; i < byteLen; i += 16) {
        uint8_t b[16];
        oz_sw_aes128_encrypt_block(mock_ske_key_reg, in + i, b);
        if (mock_ske_mode == MOCK_SKE_WRONG)
            b[7] ^= 0x10;
        memcpy(out + i, b, 16);
        mock_ske_blocks++;
    }
    return SKE_SUCCESS;
}

/* Like the engine: the key stays in the registers. */
uint32_t hal_ske_final(void)
{
    if (irq_depth == 0)
        mock_irq_violations++;
    return SKE_SUCCESS;
}

/* ── TRNG ──────────────────────────────────────────────────────────────── */

/* ── SAR ADC ─────────────────────────────────────────────────────────── */

int mock_adc_raw = 0xffff;              /* the SDK's error value until a test sets one */
int mock_adc_channel;
long mock_adc_reads;

int32_t hal_saradc_init(int mark) { mock_adc_channel = mark; return 1; }

int hal_saradc_get_data(T_eSARADC_SEL sel)
{
    mock_adc_reads++;
    return (int)sel == mock_adc_channel ? mock_adc_raw : 0xffff;
}

int mock_trng_mode;
int mock_trng_inits;
long mock_trng_words;

void hal_trng_init(void) { mock_trng_inits++; }

int32_t hal_trng_get_randbuff_bytes(uint8_t *da, int32_t len)
{
    static uint64_t s = 0x6f7a6b6965ull;
    if (mock_trng_mode == MOCK_TRNG_TIMEOUT)
        return -1;
    for (int32_t i = 0; i < len; i += 4) {
        uint32_t w = 0x5a5a5a5au;
        if (mock_trng_mode == MOCK_TRNG_GOOD) {   /* splitmix64 */
            uint64_t z = (s += 0x9e3779b97f4a7c15ull);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            w = (uint32_t)(z ^ (z >> 31));
        }
        memcpy(da + i, &w, (size_t)(len - i < 4 ? len - i : 4));
        mock_trng_words++;
    }
    return 0;
}

/* ── NOR flash ─────────────────────────────────────────────────────────── */

uint8_t mock_flash[MOCK_FLASH_SIZE];
int mock_part_present = 1;
uint32_t mock_part_start_page = (1024u * 1024u) / SPIFLASH_PAGE_SIZE;
uint32_t mock_part_size = 64u * 1024u;
long mock_flash_progs, mock_flash_erases, mock_nor_violations, mock_flash_out_of_part;
long mock_flash_fail_prog_at;
int mock_flash_dead;
long mock_part_lookups;

void mock_flash_blank(void) { memset(mock_flash, 0xff, sizeof mock_flash); }

/* The port must stay inside OZKV: anything else is code or another partition. */
static int in_part(uint32_t page, uint32_t pages)
{
    uint32_t end = mock_part_start_page + mock_part_size / SPIFLASH_PAGE_SIZE;
    if (page < mock_part_start_page || page + pages > end) {
        mock_flash_out_of_part++;
        return 0;
    }
    return 1;
}

T_BOOL spi_flash_read(T_U32 page, T_U8 *buf, T_U32 page_cnt)
{
    if (mock_flash_dead)
        return AK_FALSE;
    if (!page_cnt || !in_part(page, page_cnt))
        return AK_FALSE;
    memcpy(buf, mock_flash + (size_t)page * SPIFLASH_PAGE_SIZE, (size_t)page_cnt * SPIFLASH_PAGE_SIZE);
    return AK_TRUE;
}

T_BOOL spi_flash_write(T_U32 page, T_U8 *buf, T_U32 page_cnt)
{
    if (mock_flash_dead)
        return AK_FALSE;
    if (!page_cnt || !in_part(page, page_cnt))
        return AK_FALSE;
    if (mock_flash_fail_prog_at > 0 && --mock_flash_fail_prog_at == 0)
        return AK_FALSE;
    mock_flash_progs++;
    uint8_t *p = mock_flash + (size_t)page * SPIFLASH_PAGE_SIZE;
    for (size_t i = 0; i < (size_t)page_cnt * SPIFLASH_PAGE_SIZE; i++) {
        if ((p[i] & buf[i]) != buf[i])  /* NOR: programming only clears bits */
            mock_nor_violations++;
        p[i] &= buf[i];
    }
    return AK_TRUE;
}

T_BOOL spi_flash_sector_erase(T_U32 sector)
{
    if (mock_flash_dead)
        return AK_FALSE;
    if (!in_part(sector * SPIFLASH_PAGE_PER_SECTOR, SPIFLASH_PAGE_PER_SECTOR))
        return AK_FALSE;
    mock_flash_erases++;
    memset(mock_flash + (size_t)sector * SPIFLASH_SECTOR_SIZE, 0xff, SPIFLASH_SECTOR_SIZE);
    return AK_TRUE;
}

uint8_t partition_get_bin_info(T_SPI_FILE_INFO *info)
{
    mock_part_lookups++;
    if (!mock_part_present || strcmp((const char *)info->file_name, "OZKV"))
        return AK_FALSE;
    info->file_len = 0;
    info->file_start_page = mock_part_start_page;
    info->file_partition_size = mock_part_size;
    info->xip_start_addr = NULL;
    return AK_TRUE;
}

/* ── threads, message queues, mutexes ──────────────────────────────────── */

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int parked, go;
static long passes;

#if defined(__SANITIZE_ADDRESS__)
#define STACK_MEASURE 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STACK_MEASURE 0
#endif
#endif
#ifndef STACK_MEASURE
#define STACK_MEASURE 1
#endif

#define PAINT 0xa5

/* The pthread's own stack: [low, high). */
static void stack_bounds(uintptr_t *low, uintptr_t *high)
{
#ifdef __APPLE__
    *high = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    *low = *high - pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t a;
    void *addr;
    size_t size;
    pthread_getattr_np(pthread_self(), &a);
    pthread_attr_getstack(&a, &addr, &size);
    pthread_attr_destroy(&a);
    *low = (uintptr_t)addr;
    *high = (uintptr_t)addr + size;
#endif
}

/* Paint from just above the guard area up to a little below the caller's
 * frame; the painter's own small frame sits in the gap. */
__attribute__((noinline)) static void paint_stack(struct rt_thread *t, uintptr_t sp)
{
    uintptr_t low, high;
    stack_bounds(&low, &high);
    volatile uint8_t *p = (volatile uint8_t *)(low + 4096);
    volatile uint8_t *end = (volatile uint8_t *)(sp - 512);
    t->paint_lo = (uint8_t *)p;
    t->paint_top = sp;
    while (p < end)
        *p++ = PAINT;
}

static size_t scan_peak(const struct rt_thread *t)
{
    const volatile uint8_t *p = t->paint_lo;
    while ((uintptr_t)p < t->paint_top - 512 && *p == PAINT)
        p++;
    return (size_t)(t->paint_top - (uintptr_t)p);
}

static void *trampoline(void *arg)
{
    struct rt_thread *t = arg;
    volatile char here;
    if (STACK_MEASURE)
        paint_stack(t, (uintptr_t)&here);
    t->entry(t->param);
    if (STACK_MEASURE)
        t->peak_at_exit = scan_peak(t);
    pthread_mutex_lock(&big);
    t->finished = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&big);
    return NULL;
}

static struct rt_thread *threads[8];

rt_err_t rt_thread_init(struct rt_thread *thread, const char *name, void (*entry)(void *parameter),
                        void *parameter, void *stack_start, rt_uint32_t stack_size,
                        rt_uint8_t priority, rt_uint32_t tick)
{
    (void)tick;
    if (!thread || !entry || !stack_start || priority >= RT_THREAD_PRIORITY_MAX)
        return -RT_ERROR;
    memset(thread, 0, sizeof *thread);
    snprintf(thread->name, sizeof thread->name, "%s", name);
    thread->entry = entry;
    thread->param = parameter;
    thread->stack = stack_start;
    thread->stack_size = stack_size;
    memset(stack_start, '#', stack_size);       /* as RT-Thread does */
    for (unsigned i = 0; i < sizeof threads / sizeof threads[0]; i++)
        if (!threads[i]) {
            threads[i] = thread;
            return RT_EOK;
        }
    return -RT_ERROR;
}

/* The code runs on a pthread stack, not the one it gave rt_thread_init:
 * host frames (and ASan's) are bigger, so stack sizes are measured elsewhere
 * (ozkey-49 P5-D). */
rt_err_t rt_thread_startup(rt_thread_t thread)
{
    pthread_t p;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u * 1024u * 1024u);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    thread->started = 1;
    int rc = pthread_create(&p, &a, trampoline, thread);
    pthread_attr_destroy(&a);
    return rc ? -RT_ERROR : RT_EOK;
}

static struct rt_thread *find_thread(const char *name)
{
    struct rt_thread *t = NULL;
    for (unsigned i = 0; i < sizeof threads / sizeof threads[0]; i++)
        if (threads[i] && !strcmp(threads[i]->name, name))
            t = threads[i];
    return t;
}

/* Called only while the thread is parked or finished, so nothing moves. A
 * finished thread's stack is unmapped: it measured itself on the way out. */
size_t mock_stack_peak(const char *name)
{
    struct rt_thread *t = find_thread(name);
    if (!STACK_MEASURE || !t || !t->paint_lo)
        return 0;
    return t->finished ? t->peak_at_exit : scan_peak(t);
}

void mock_join(const char *name)
{
    struct rt_thread *t = NULL;
    for (unsigned i = 0; i < sizeof threads / sizeof threads[0]; i++)
        if (threads[i] && !strcmp(threads[i]->name, name))
            t = threads[i];
    if (!t || !t->started) {
        fprintf(stderr, "mock_join: no started thread %s\n", name);
        abort();
    }
    pthread_mutex_lock(&big);
    while (!t->finished)
        pthread_cond_wait(&cv, &big);
    pthread_mutex_unlock(&big);
}

/* RT-Thread keeps a pointer-sized header in front of each message: 4 bytes on
 * the RV32 target. The capacity sum mirrors that, not the host's 8. */
#define MQ_HEADER 4u

rt_err_t rt_mq_init(rt_mq_t mq, const char *name, void *msgpool, rt_size_t msg_size,
                    rt_size_t pool_size, rt_uint8_t flag)
{
    (void)flag;
    memset(mq, 0, sizeof *mq);
    snprintf(mq->name, sizeof mq->name, "%s", name);
    mq->pool = msgpool;
    mq->msg_size = (msg_size + 3u) & ~(rt_size_t)3u;
    mq->max = pool_size / (mq->msg_size + MQ_HEADER);
    mq->lens = calloc(mq->max ? mq->max : 1, sizeof *mq->lens);
    return mq->max && mq->lens ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_mq_send(rt_mq_t mq, void *buffer, rt_size_t size)
{
    rt_err_t rc = RT_EOK;
    if (size > mq->msg_size)
        return -RT_ERROR;
    pthread_mutex_lock(&big);
    if (mq->count == mq->max) {
        rc = -RT_EFULL;
    } else {
        rt_size_t slot = (mq->head + mq->count) % mq->max;
        memcpy(mq->pool + slot * mq->msg_size, buffer, size);
        mq->lens[slot] = size;
        mq->count++;
    }
    pthread_mutex_unlock(&big);
    return rc;
}

/* The lockstep point: see mock_sdk.h. Like RT-Thread, a receive copies the
 * whole slot (up to `size`), not just what was sent. */
rt_err_t rt_mq_recv(rt_mq_t mq, void *buffer, rt_size_t size, rt_int32_t timeout)
{
    rt_err_t rc;
    pthread_mutex_lock(&big);
    parked = 1;
    pthread_cond_broadcast(&cv);
    while (!go)
        pthread_cond_wait(&cv, &big);
    go = 0;
    parked = 0;
    passes++;
    if (mq->count) {
        memcpy(buffer, mq->pool + mq->head * mq->msg_size, size < mq->msg_size ? size : mq->msg_size);
        mq->head = (mq->head + 1) % mq->max;
        mq->count--;
        rc = RT_EOK;
    } else if (timeout == RT_WAITING_FOREVER) {
        fprintf(stderr, "rt_mq_recv: would block forever\n");
        abort();
    } else {
        mock_ms += (uint32_t)timeout * (1000u / RT_TICK_PER_SECOND);
        rc = -RT_ETIMEOUT;
    }
    pthread_mutex_unlock(&big);
    return rc;
}

void mock_wait_parked(void)
{
    pthread_mutex_lock(&big);
    while (!parked)
        pthread_cond_wait(&cv, &big);
    pthread_mutex_unlock(&big);
}

void mock_pass(void)
{
    pthread_mutex_lock(&big);
    while (!parked)
        pthread_cond_wait(&cv, &big);
    long n = passes;
    go = 1;
    pthread_cond_broadcast(&cv);
    while (passes == n || !parked)
        pthread_cond_wait(&cv, &big);
    pthread_mutex_unlock(&big);
}

rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag)
{
    (void)name;
    (void)flag;
    pthread_mutex_t *m = malloc(sizeof *m);
    if (!m || pthread_mutex_init(m, NULL))
        return -RT_ERROR;
    mutex->impl = m;
    mutex->init = 1;
    return RT_EOK;
}

rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    (void)time;
    return mutex->init && !pthread_mutex_lock(mutex->impl) ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    return mutex->init && !pthread_mutex_unlock(mutex->impl) ? RT_EOK : -RT_ERROR;
}

/* ── BLE ───────────────────────────────────────────────────────────────── */

T_BLE_INIT_PARAM mock_ble;
int mock_ble_inits, mock_ble_init_fail, mock_gatt_fail;
const T_BT_ATTREC *mock_gatt_table;
uint32_t mock_gatt_len;
uint16_t mock_mtu_requested;
uint8_t mock_dev_addr[6] = { 0xb8, 0xca, 0x3a, 0xe6, 0xbe, 0xac };
int mock_no_dev_addr;

int mock_adv_enabled;
uint8_t mock_adv_type;
uint16_t mock_adv_min, mock_adv_max;
uint8_t mock_adv_data[31], mock_scan_rsp[31];
size_t mock_adv_data_len, mock_scan_rsp_len;
int mock_adv_data_fail;
long mock_adv_applies;

static T_eBLE_RESULT accept_notify(void *link, const uint8_t *buf, uint16_t len, uint16_t idx)
{
    (void)link;
    (void)buf;
    (void)len;
    (void)idx;
    return eBLE_RESULT_SUCCESS;
}
T_eBLE_RESULT (*mock_on_notify)(void *link, const uint8_t *buf, uint16_t len, uint16_t idx) = accept_notify;

T_eBLE_RESULT com_ble_init(T_BLE_INIT_PARAM *p)
{
    mock_ble_inits++;
    if (mock_ble_init_fail)
        return eBLE_RESULT_INIT_ERR;
    mock_ble = *p;
    return eBLE_RESULT_SUCCESS;
}

T_eBLE_RESULT com_ble_set_gatt_service(T_BT_ATTREC *table, uint32_t len, uint8_t role)
{
    if (mock_gatt_fail || role != eBLE_ROLE_SERVER)
        return eBLE_RESULT_FAIL;
    mock_gatt_table = table;
    mock_gatt_len = len;
    return eBLE_RESULT_SUCCESS;
}

T_eBLE_RESULT com_ble_server_notify_ext(T_pVOID link, uint8_t *buf, uint16_t len, uint16_t idx)
{
    return mock_on_notify(link, buf, len, idx);
}

T_eBLE_RESULT com_ble_set_adv_enable(T_eBLE_ENABLE_TYPE on)
{
    mock_adv_enabled = on == eBLE_ENABLE;
    if (mock_adv_enabled)
        mock_adv_applies++;
    return eBLE_RESULT_SUCCESS;
}

T_eBLE_RESULT com_ble_set_adv_interval(uint8_t type, uint16_t min, uint16_t max, uint8_t peer_type, T_BLE_ADDR peer)
{
    (void)peer_type;
    (void)peer;
    if (mock_adv_enabled)                       /* HCI: only while stopped */
        return eBLE_RESULT_FAIL;
    mock_adv_type = type;
    mock_adv_min = min;
    mock_adv_max = max;
    return eBLE_RESULT_SUCCESS;
}

T_eBLE_RESULT com_ble_set_adv_data(T_BT_ADV_DATA *adv, T_BT_ADV_DATA *rsp)
{
    if (mock_adv_data_fail || mock_adv_enabled)
        return eBLE_RESULT_FAIL;
    if (adv->custom_data.data_len > 31 || rsp->custom_data.data_len > 31)
        return eBLE_RESULT_PARAM_ERR;
    mock_adv_data_len = adv->custom_data.data_len;
    memcpy(mock_adv_data, adv->custom_data.p_data, mock_adv_data_len);
    mock_scan_rsp_len = rsp->custom_data.data_len;
    memcpy(mock_scan_rsp, rsp->custom_data.p_data, mock_scan_rsp_len);
    return eBLE_RESULT_SUCCESS;
}

uint8_t com_ble_set_mtu_size(T_pVOID handle, uint16_t mtu)
{
    (void)handle;
    mock_mtu_requested = mtu;
    return 0;
}

uint8_t *com_ble_get_dev_addr(void) { return mock_no_dev_addr ? NULL : mock_dev_addr; }

void mock_ble_event(void *link, T_eBT_MSG msg, int32_t param1)
{
    (void)mock_ble.evt_handle_cb(link, msg, param1, 0);
}

void mock_ble_write(void *link, const void *att_rec, const uint8_t *data, uint16_t len)
{
    uint8_t buf[BLE_MTU_MAX_SIZE];
    T_BT_ATTRECV_CB info;
    memset(&info, 0, sizeof info);
    memcpy(buf, data, len);
    info.p_buf = buf;
    info.buf_size = len;
    info.p_att_rec = (void *)att_rec;
    mock_ble.recv_cb(link, &info, sizeof info);
}

uint16_t mock_ble_read(void *link, const void *att_rec, uint8_t *buf, uint16_t cap)
{
    T_BT_ATTRECV_CB info;
    memset(&info, 0, sizeof info);
    info.p_buf = buf;
    info.buf_size = cap;
    info.buf_used = 0xffff;                     /* the callback must set it */
    info.p_att_rec = (void *)att_rec;
    mock_ble.read_cb(link, &info, sizeof info);
    return info.buf_used;
}

/* ── the shell ─────────────────────────────────────────────────────────── */

static struct { const char *name; mock_msh_fn fn; } cmds[16];

void mock_msh_register(const char *name, mock_msh_fn fn)
{
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        if (!cmds[i].name) {
            cmds[i].name = name;
            cmds[i].fn = fn;
            return;
        }
    abort();
}

int mock_msh_run(const char *line)
{
    char copy[128], *argv[8];
    int argc = 0;
    snprintf(copy, sizeof copy, "%s", line);
    for (char *t = strtok(copy, " "); t && argc < 8; t = strtok(NULL, " "))
        argv[argc++] = t;
    for (unsigned i = 0; argc && i < sizeof cmds / sizeof cmds[0] && cmds[i].name; i++)
        if (!strcmp(cmds[i].name, argv[0])) {
            cmds[i].fn((char)argc, argv);
            return 0;
        }
    return -1;
}

/* sdk-1.02 com_ble.h:289. The host mock has no link; it counts the call so a
 * test can see the squat timeout fire. */
int mock_disconnects;
T_eBLE_RESULT com_ble_disconnect(uint8_t idx)
{
    (void)idx;
    mock_disconnects++;
    return eBLE_RESULT_SUCCESS;
}

/* ── watchdog, reset status, boot state (F-AK-WDT-1) ───────────────────── */
#include "hal_watchdog.h"
#include "hal_sysclock.h"
#include "hal_lpc.h"
int mock_wdt_enabled, mock_wdt_period = -1;
long mock_wdt_feeds;
int mock_reset_status = HAL_AO_POR_RST;
int mock_boot_state;
void hal_wdt_start(T_eWDT_TIME feed_time) { mock_wdt_enabled = 1; mock_wdt_period = (int)feed_time; }
void hal_wdt_feed(void) { if (mock_wdt_enabled) mock_wdt_feeds++; }
void hal_wdt_stop(void) { mock_wdt_enabled = 0; }
T_eRST_STATUS hal_clock_get_reset_status(void) { return (T_eRST_STATUS)mock_reset_status; }
int32_t hal_lpc_get_boot_state(void) { return mock_boot_state; }
