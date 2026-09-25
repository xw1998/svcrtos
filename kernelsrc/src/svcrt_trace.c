/* ============================================================
 * svcrt_trace.c - Kernel event trace facade over the mdk_trace SWD backend
 *
 * See svcrt_trace.h for why the kernel does not own a ring any more.
 *
 * Re-entrancy
 *   Every recording call site is already inside a critical section:
 *     - svcrt_trace_switch() <- svcrt_sched_activate(), which runs in
 *                               the PendSV / fault exception
 *     - svcrt_trace_wait()   <- the blocking helpers, between
 *                               SVCRT_DISABLE_IRQ()/SVCRT_ENABLE_IRQ()
 *   and the backend takes its own critical section besides, so a record
 *   never races the host's read-modify-write of the drain cursor.
 *
 * Cost
 *   One event is a few stores plus a masked ring index; the ring is in
 *   SRAM and nothing blocks. That is what lets this run on every context
 *   switch without the trace changing the thing it measures.
 * ============================================================ */
#include "svcrt_trace.h"
#include "svcrt_task.h"
#include "mdk_trace.h"
#include "mdk_trace_swd.h"

/* ---- 阻塞对象登记（见 svcrt_trace.h 的契约） ----
 * 放在 #if 之外：关掉追踪时这两句也必须在，因为 svcrt_task.c 无条件调用。 */
static volatile uint16 g_tr_wait_obj13;

void svcrt_trace_wait_obj(uint16 obj13)
{
    g_tr_wait_obj13 = obj13;
}

uint16 svcrt_trace_take_wait_obj(void)
{
    uint16 v = g_tr_wait_obj13;

    g_tr_wait_obj13 = 0u;   /* 只消费一次，不留给下一次阻塞 */
    return v;
}

#include "ark_shell.h"
#include "svcrt_config.h"    /* feature gates: SVCRT_USE_MDK_TRACE */
#if SVCRT_USE_MDK_TRACE

/* ---------------- state ---------------- */
static volatile uint8 g_tr_on;
static uint32 g_tr_sw[SVCRT_TRACE_MAX_TASKS];      /* switch-in counter    */

/* ---------------- lifecycle ---------------- */
void svcrt_trace_init(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)SVCRT_TRACE_MAX_TASKS; i++)
    {
        g_tr_sw[i] = 0u;
    }

    /* Enables the cycle counter and, with the SWD backend, starts a new
     * recording sequence (the ring is cleared so stale bytes from a previous
     * run cannot be spliced onto this one). */
    mdk_trace_init();

    /* 「内核钩子装好」这一刻打个标记，时间线可以从这里切开，
     * 而不是从碰巧的第一条事件开始。 */
    mdk_trace_svcrt_init();
    g_tr_on = 1u;
}

void svcrt_trace_reset(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)SVCRT_TRACE_MAX_TASKS; i++)
    {
        g_tr_sw[i] = 0u;
    }
}

void svcrt_trace_enable(uint8 on)
{
    g_tr_on = (uint8)(on ? 1u : 0u);
}

uint8  svcrt_trace_enabled(void)   { return g_tr_on; }

uint32 svcrt_trace_capacity(void)  { return mdk_trace_swd_blob.ctrl.cap; }
uint32 svcrt_trace_total(void)     { return mdk_trace_swd_blob.ctrl.events; }
uint32 svcrt_trace_last_cycles(void) { return mdk_trace_now(); }
uint32 svcrt_trace_overrun(void)   { return mdk_trace_swd_blob.ctrl.lost_events; }

uint32 svcrt_trace_switch_count(uint8 task_id)
{
    return (task_id < (uint8)SVCRT_TRACE_MAX_TASKS) ? g_tr_sw[task_id] : 0u;
}


/* ---------------- 调度开销统计（基准用） ----------------
 * 符号全部是全局的，主机侧 read_variable 直接读，不需要 shell 命令：
 *   svcrt_sched_stat_*   每次真正切换时 sched_activate() 的 DWT 周期数
 *   svcrt_pendsv_stat_*  PendSV 整趟往返（在下面的 isr 钩子里量，与 trace 开关无关）
 * 注意：CYCCNT 由 mdk_trace_init() 打开，统计才有意义。 */
#if (SVCRT_USE_SCHED_STAT == 1)
volatile uint32 svcrt_sched_stat_n   = 0u;
volatile uint32 svcrt_sched_stat_sum = 0u;
volatile uint32 svcrt_sched_stat_min = 0xFFFFFFFFu;
volatile uint32 svcrt_sched_stat_max = 0u;
volatile uint32 svcrt_pendsv_stat_n   = 0u;
volatile uint32 svcrt_pendsv_stat_sum = 0u;
volatile uint32 svcrt_pendsv_stat_min = 0xFFFFFFFFu;
volatile uint32 svcrt_pendsv_stat_max = 0u;
volatile uint32 svcrt_pendsv_span_t0  = 0u;

void svcrt_sched_stat_close(uint32 t0)
{
    uint32 d = svcrt_trace_last_cycles() - t0;

    svcrt_sched_stat_n++;
    svcrt_sched_stat_sum += d;
    if(d < svcrt_sched_stat_min) { svcrt_sched_stat_min = d; }
    if(d > svcrt_sched_stat_max) { svcrt_sched_stat_max = d; }
}
#endif

/* ---------------- recording ---------------- */
void svcrt_trace_record(uint8 ev, uint8 arg)
{
    if(g_tr_on == 0u)
    {
        return;
    }
    mdk_trace_event((uint16_t)ev, MDK_TRACE_KIND_POINT, (uint32_t)arg);
}

void svcrt_trace_switch(uint8 from, uint8 to)
{
    if(g_tr_on == 0u)
    {
        return;
    }
    if(to < (uint8)SVCRT_TRACE_MAX_TASKS)
    {
        g_tr_sw[to]++;
    }
    mdk_trace_sched((uint16_t)from, (uint16_t)to);
}

void svcrt_trace_wait(uint8 task_id)
{
    svcrt_trace_record(SVCRT_TR_EV_WAIT, task_id);
}

void svcrt_trace_isr(uint8 irq, uint8 kind)
{
#if (SVCRT_USE_SCHED_STAT == 1)
    /* PendSV(14) 的进/出钩子正好夹住整个异常处理（不切换时也会成对进出），
     * 用它们量整趟往返。放在 g_tr_on 早退之前，所以 trace 关掉时也能量。 */
    if(irq == 14u)
    {
        uint32 now = svcrt_trace_last_cycles();

        if(kind == 0u)
        {
            svcrt_pendsv_span_t0 = now;
        }
        else
        {
            uint32 d = now - svcrt_pendsv_span_t0;

            svcrt_pendsv_stat_n++;
            svcrt_pendsv_stat_sum += d;
            if(d < svcrt_pendsv_stat_min) { svcrt_pendsv_stat_min = d; }
            if(d > svcrt_pendsv_stat_max) { svcrt_pendsv_stat_max = d; }
        }
    }
#endif
    if(g_tr_on == 0u)
    {
        return;
    }
    mdk_trace_isr((uint16_t)irq, (uint8_t)kind);
}

/* ---------------- console command ----------------
 * The console reports the health of the ring. It does NOT dump the events:
 * the bytes are meant to travel over SWD, where the host can read a slice at
 * a time and the target never has to hold the CPU for a whole buffer. */
static int trc_streq(const char *a, const char *b)
{
    while((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

static void trc_status_line(void)
{
    const mdk_trace_swd_ctrl_t *c = &mdk_trace_swd_blob.ctrl;
    uint32 pending = c->head - c->drained;

    ark_shell_printf("\r\ntrace : %s, seq=%u cap=%u events=%u tokens=%u "
                     "pending=%u lost=%u B\r\n",
                     (g_tr_on != 0u) ? "ARMED" : "idle",
                     (unsigned)c->seq, (unsigned)c->cap,
                     (unsigned)c->events, (unsigned)c->tokens,
                     (unsigned)pending, (unsigned)c->lost_bytes);
    if(c->lost_events != 0u)
    {
        ark_shell_printf("        %u events were dropped (the host drained "
                         "slower than they arrived)\r\n",
                         (unsigned)c->lost_events);
    }
    ark_shell_printf("        read it with the host tools trace_swd_status / "
                     "trace_swd_read / trace_swd_reset\r\n");
}

int svcrt_trace_shell_cmd(int argc, char *argv[])
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    if(trc_streq(sub, "start") != 0)
    {
        svcrt_trace_init();
        ark_shell_printf("\r\ntrace : armed, %u B ring, %u cycles/s\r\n",
                         (unsigned)mdk_trace_swd_blob.ctrl.cap,
                         (unsigned)mdk_trace_swd_blob.ctrl.cpu_hz);
        return 0;
    }
    if(trc_streq(sub, "stop") != 0)
    {
        svcrt_trace_enable(0u);
        ark_shell_printf("\r\ntrace : stopped, %u events, %u lost\r\n",
                         (unsigned)mdk_trace_swd_blob.ctrl.events,
                         (unsigned)mdk_trace_swd_blob.ctrl.lost_events);
        return 0;
    }
    if(trc_streq(sub, "reset") != 0)
    {
        svcrt_trace_reset();
        ark_shell_printf("\r\ntrace : switch counters cleared "
                         "(the ring restarts via the host's trace_swd_reset)\r\n");
        return 0;
    }
    if(trc_streq(sub, "dump") != 0)
    {
        trc_status_line();
        return 0;
    }
    if(trc_streq(sub, "mark") != 0)
    {
        uint32 v = 0u;
        if(argc > 2)
        {
            const char *p = argv[2];
            while((*p >= '0') && (*p <= '9'))
            {
                v = v * 10u + (uint32)(*p - '0');
                p++;
            }
        }
        mdk_trace_mark(v);
        ark_shell_printf("\r\ntrace : mark %u\r\n", (unsigned)v);
        return 0;
    }

    trc_status_line();
    ark_shell_printf("usage : trace [start | stop | reset | dump | mark <n>]\r\n");
    return 0;
}

#else  /* !SVCRT_USE_MDK_TRACE -------------------------------------------- */
/* Instrumentation is compiled out.  The kernel still calls these from the
 * switch path and the board calls the fault capture from every fault handler,
 * so they stay here as no-ops instead of forcing every call site into #if.
 * Behaviour when off: nothing is recorded, every counter reads zero, and the
 * console command refuses. */
#include "svcrt_trace.h"

void   svcrt_trace_init(void) {}
void   svcrt_trace_reset(void) {}
void   svcrt_trace_enable(uint8 on) { (void)on; }
uint8  svcrt_trace_enabled(void) { return 0u; }
uint32 svcrt_trace_capacity(void) { return 0u; }
uint32 svcrt_trace_total(void) { return 0u; }
uint32 svcrt_trace_last_cycles(void) { return 0u; }
uint32 svcrt_trace_switch_count(uint8 task_id) { (void)task_id; return 0u; }
uint32 svcrt_trace_overrun(void) { return 0u; }
void   svcrt_trace_record(uint8 ev, uint8 arg) { (void)ev; (void)arg; }
void   svcrt_trace_switch(uint8 from, uint8 to) { (void)from; (void)to; }
void   svcrt_trace_wait(uint8 task_id) { (void)task_id; }
void   svcrt_trace_isr(uint8 irq, uint8 kind) { (void)irq; (void)kind; }
int    svcrt_trace_shell_cmd(int argc, char *argv[]) { (void)argc; (void)argv; return -1; }

#endif /* SVCRT_USE_MDK_TRACE */
