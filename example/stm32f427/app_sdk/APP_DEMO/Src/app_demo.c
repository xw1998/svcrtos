/**
* @brief SVCrtOS App SDK example - full RTOS feature self test
* @details A user-mode App that walks through the RTOS services exposed by the
*          App SDK and reports the result of every step on COM1:
*            device IO   : three on-board LEDs (LED / LED2 / LED3) + UART
*            task        : wait, wait_period, delay
*            sync        : semaphore, mutex, event
*            ipc         : message queue
*            timer       : one-shot / periodic software timer
*            diagnostics : time base, CPU idle rate, fault record count
*            partition   : App slot status via svcrt_app_status()
*
*          The App carries no address and no jump table: it only includes
*          svcrt.h and reaches the kernel through SVC. Slot placement and stack
*          are decided by the kernel at load time (config/svcrt_partition.h).
*
*          After the self test the App falls into a visible heartbeat loop so
*          that the shell's "app stop <slot>" / "app start <slot>" commands can
*          be observed on the LEDs and the console.
*/

#include "svcrt.h"
/* The compatibility layer is a plain App side library: it needs SVC 0x1B
 * (thread control) plus the services the App SDK already publishes, so this
 * single include is the whole "ported from Linux/Windows" story. */
#include "svcrt_win_compat.h"

/* ---- console helpers ------------------------------------------------- */

static int32 g_con = -1;

/* One write() per string instead of one per byte: every svcrt_dev_write is a
 * full SVC round trip, and the console sink below it may have to wait for room
 * in the transmit pipe.  A single call keeps that wait bounded to one place
 * and lets the sink lock the console once for the whole string. */
static void app_puts(const char *s)
{
    int32 len = 0;

    while(s[len] != '\0')
    {
        len++;
    }
    if(len > 0)
    {
        (void)svcrt_dev_write(g_con, (void *)s, len);
    }
}

static void app_num(int32 v)
{
    char buf[12];
    int32 i = 0;
    uint32 u;

    if(v < 0)
    {
        app_puts("-");
        u = (uint32)(-v);
    }
    else
    {
        u = (uint32)v;
    }

    if(u == 0u)
    {
        app_puts("0");
        return;
    }

    while((u > 0u) && (i < (int32)sizeof(buf)))
    {
        buf[i++] = (char)('0' + (u % 10u));
        u /= 10u;
    }

    /* The digits came out least significant first; reverse them in place and
     * hand the whole number to one write(). */
    {
        int32 a = 0;
        int32 b = i - 1;

        while(a < b)
        {
            char t = buf[a];
            buf[a] = buf[b];
            buf[b] = t;
            a++;
            b--;
        }
    }
    if(i > 0)
    {
        (void)svcrt_dev_write(g_con, (void *)buf, i);
    }
}

static void app_hex8(uint32 v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[8];
    int32 i;

    for(i = 0; i < 8; i++)
    {
        buf[i] = hex[(v >> ((uint32)(7 - i) * 4u)) & 0xFu];
    }
    (void)svcrt_dev_write(g_con, (void *)buf, 8);
}

/* ---- test bookkeeping ------------------------------------------------ */

static int32 g_pass = 0;
static int32 g_fail = 0;

/* ---- atomic diagnostic lines ------------------------------------------ *
 * app_puts() issues one svcrt_dev_write per call, so a line assembled from
 * several app_puts() calls can be cut in half by another thread's output and
 * its value ends up printed under someone else's name (observed as a bogus
 * "unlock.rc = -2"). Every diagnostic line is built in a buffer first and
 * handed to the console in a single write, which the sink serialises. */
static char  g_ob_buf[112];
static int32 g_ob_len;

static void ob_reset(void)
{
    g_ob_len = 0;
}

static void ob_str(const char *str)
{
    while((*str != '\0') && (g_ob_len < (int32)(sizeof(g_ob_buf) - 1)))
    {
        g_ob_buf[g_ob_len++] = *str++;
    }
}

static void ob_num(int32 v)
{
    char tmp[12];
    int32 i = 0;
    uint32 u;

    if(v < 0)
    {
        ob_str("-");
        u = (uint32)(-v);
    }
    else
    {
        u = (uint32)v;
    }
    if(u == 0u)
    {
        ob_str("0");
        return;
    }
    while((u > 0u) && (i < (int32)sizeof(tmp)))
    {
        tmp[i++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    while((i > 0) && (g_ob_len < (int32)(sizeof(g_ob_buf) - 1)))
    {
        g_ob_buf[g_ob_len++] = tmp[--i];
    }
}

static void ob_flush(void)
{
    if((g_ob_len > 0) && (g_con >= 0))
    {
        (void)svcrt_dev_write(g_con, g_ob_buf, g_ob_len);
    }
}

static void report(const char *name, int32 ok, int32 detail)
{
    ob_reset();
    ob_str("APP_TEST ");
    ob_str(name);
    ob_str(ok ? " ... OK" : " ... FAIL");
    if(!ok)
    {
        ob_str(" (");
        ob_num(detail);
        ob_str(")");
    }
    ob_str("\r\n");
    ob_flush();

    if(ok)
    {
        g_pass++;
    }
    else
    {
        g_fail++;
    }
}

/* 诊断专用：无论成败都把读数原样打出来。
 * report() 只在 FAIL 时才印数字，而诊断项的读数大多是"正常值"，
 * 走 report() 就等于什么都没看见 —— 那是自造假证据。 */
/* list_names hands back a back to back NUL terminated name list, and
 * neither the App SDK nor this App pulls in string.h, so the check is
 * done here on those terms: report what is actually there. */
static int32 names_has(const char *names, const char *want)
{
    const char *p;
    const char *q;
    const char *w;

    /* svcrt_fs_list_names() writes one name per line (\n separated,
     * one NUL at the very end), so that is the format to walk. */
    p = names;
    while(*p != '\0')
    {
        q = p;
        w = want;
        while((*q == *w) && (*w != '\0'))
        {
            q++;
            w++;
        }
        if(*w == '\0')
        {
            return 1;
        }
        while((*p != '\0') && (*p != '\n'))
        {
            p++;
        }
        if(*p == '\n')
        {
            p++;
        }
    }
    return 0;
}

static void note(const char *name, int32 v)
{
    ob_reset();
    ob_str("APP_NOTE ");
    ob_str(name);
    ob_str(" = ");
    ob_num(v);
    ob_str("\r\n");
    ob_flush();
}

/* ---- LED helpers ------------------------------------------------------ */

static int32 g_led_r = -1;
static int32 g_led_g = -1;
static int32 g_led_b = -1;

/* The LED driver maps the transfer length to the action:
 * len > 0 -> on, len == 0 -> off, len < 0 -> toggle. */
static int32 led_on(int32 led)
{
    uint8 dummy = 1;
    return svcrt_dev_write(led, &dummy, 1);
}

static int32 led_off(int32 led)
{
    uint8 dummy = 0;
    return svcrt_dev_write(led, &dummy, 0);
}

static int32 led_toggle(int32 led)
{
    uint8 dummy = 0;
    return svcrt_dev_write(led, &dummy, -1);
}

/* ---- timer callback -------------------------------------------------- */

static volatile int32 g_timer_hits = 0;

static void on_timer(void *arg)
{
    (void)arg;
    g_timer_hits++;
}

/* =================================================================== */

/* ---- console services: user log (SVC 0x19) and user command (SVC 0x1A) - */

static int app_cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Console output also goes through SVC 0x1A: the App never owns the UART. */
    svcrt_pth_dbg_t slots[SVCRT_POSIX_THREAD_MAX];
    int32 i;

    (void)svcrt_shell_printf("APP_DEMO t=%d ms cpu=%d%% timer_hits=%d\r\n",
                             (int)svcrt_get_time_ms(),
                             (int)svcrt_get_cpu_usage(),
                             (int)g_timer_hits);

    for(i = 0; i < SVCRT_POSIX_THREAD_MAX; i++)
    {
        slots[i].in_use  = 0u;
        slots[i].task_id = -1;
        slots[i].gate    = -1;
        slots[i].done    = -1;
    }
    (void)svcrt_posix_pth_dbg_slots(slots);

    /* claim / boot / lost / post / exit: the whole story of a join that does
     * not come back.  A "lost" worker is an App side bug, a posted "done"
     * with a join still asleep points at the kernel wait, and the split is
     * not guessable from the outside. */
    (void)svcrt_shell_printf("pthinfo claim=%d boot=%d lost=%d post=%d exit=%d\r\n",
                             (int)svcrt_posix_pth_dbg_claim,
                             (int)svcrt_posix_pth_dbg_boot,
                             (int)svcrt_posix_pth_dbg_lost,
                             (int)svcrt_posix_pth_dbg_post,
                             (int)svcrt_posix_pth_dbg_exit);
    for(i = 0; i < SVCRT_POSIX_THREAD_MAX; i++)
    {
        (void)svcrt_shell_printf("pth slot%d in_use=%u task_id=%d gate=%d done=%d\r\n",
                                 (int)i,
                                 (unsigned int)slots[i].in_use,
                                 (int)slots[i].task_id,
                                 (int)slots[i].gate,
                                 (int)slots[i].done);
    }

    return 0;
}

/* The descriptor itself must sit in the App's own RAM - the kernel reads it
 * through SVC and checks the pointer against the caller's windows. The name
 * and help strings may live in the App's read-only data as well. */
static svcrt_ushell_cmd_t g_app_cmd =
{
    "appstat",
    app_cmd_status,
    "APP_DEMO status, registered by the App via SVC 0x1A",
    1
};


/* ---- POSIX / Windows self test helpers -------------------------------- */

static volatile int32 g_pth_hits = 0;

/* pthread start routine: plain POSIX signature. */
static void *pth_worker(void *arg)
{
    int32 i;

    for(i = 0; i < 3; i++)
    {
        g_pth_hits++;
        svcrt_task_wait(10u);
    }
    return arg;
}

/* Win32 start routine: DWORD(*)(LPVOID). Same argument/return registers on
 * ARM, so the trampoline inside the layer calls it directly. */
static svcrt_DWORD win_worker(svcrt_LPVOID arg)
{
    (void)arg;
    g_pth_hits++;
    return (svcrt_DWORD)0x1234u;
}

/* ---- condition variable self test helpers (section 10b) --------------- */

/* 共享的"代际"计数：主线程持锁时把它 +1 再 signal，等待线程在自己的循环
 * 条件里读它。所有读写都在互斥量保护下，没有裸奔的共享状态。 */
static svcrt_pthread_mutex_t g_cv_mtx = PTHREAD_MUTEX_INITIALIZER;
static svcrt_pthread_cond_t  g_cv     = PTHREAD_COND_INITIALIZER;
static volatile int32 g_cv_gen   = 0;
static volatile int32 g_cv_armed = 0;   /* 已进 cond_wait 的等待者个数（持锁自增） */
static int32 g_cv_gate = -1;            /* 等待者"举手"用的会合信号量（持锁时 post） */
/* 诊断探针：每个等待者按自己的任务号占一格，记录它真实走到哪一步。
 * 用独立格子而不是一个计数器，是为了把"没跑到"和"写了但看不到"分开。 */
/* 每个等待者独占 8 格：0=lock.rc 1=armed 2=cv.rc 3=unlock.rc
 *                    4=loops    5=tid   6=step 7=保留
 * 一个线程只写自己那几格，主线程 join 之后统一读出来 —— 两个等待者共用
 * 一格（旧的 thread_self()&3 做法）会让后写的把先写的盖掉，读数也就分不
 * 清是谁的。 */
static volatile uint32 g_cv_probe[16];
static volatile int32  g_cv_target = 1;   /* 等待者要等的代数（与 arg 分离，arg 改作槽位号） */
static const char *g_cv_probe_name[8] = {
    "lock.rc", "armed", "cv.rc", "unlock.rc", "loops", "tid", "step", "rsv"
};

#define CV_PROBE_BASE(i)  ((i) * 8)

/* 把某个等待者的 8 格原样打出来（只在主线程调用）。 */
static void dump_probe(const char *tag, int32 idx)
{
    int32 b = CV_PROBE_BASE(idx);
    int32 k;

    for(k = 0; k < 8; k++)
    {
        ob_reset();
        ob_str("APP_NOTE ");
        ob_str(tag);
        ob_str(".");
        ob_str(g_cv_probe_name[k]);
        ob_str(" = ");
        ob_num((int32)g_cv_probe[b + k]);
        ob_str("\r\n");
        ob_flush();
    }
}

/* 等 g_cv_gen 涨到 target，然后报告结论。
 *
 * 判据全在返回值里，没有一条靠"看起来动了"：
 *   - 醒来后 pthread_mutex_unlock 必须成功 —— 内核只在调用者是持有者时返回 0，
 *     所以这一步本身就是"返回时重新持有互斥量"的证据；
 *   - 循环正常退出时 rc 应为 0，否则回 0xBAD0 让调用方断言显红。 */
static void *cv_waiter(void *arg)
{
    int32 idx = (int32)(uint32)arg;                     /* 探针槽位号，不再兼作 target */
    int32 base = CV_PROBE_BASE(idx);
    int32 target = g_cv_target;
    int32 rc = -1;
    int32 lr;
    int32 loops = 0;

    g_cv_probe[base + 5] = (uint32)svcrt_thread_self();
    g_cv_probe[base + 6] = 1u;                          /* step 1: entering */
    lr = pthread_mutex_lock(&g_cv_mtx);
    g_cv_probe[base + 0] = (uint32)lr;
    if(lr != 0)
    {
        g_cv_probe[base + 6] = 0xE1u;
        return (void *)0xE1u;                           /* step 0xE1: lock failed */
    }

    g_cv_armed++;
    g_cv_probe[base + 1] = (uint32)g_cv_armed;
    if(g_cv_gate >= 0)
    {
        /* 持锁举手：主线程收到这一票时，等待者一定已经拿到过锁、马上进 cond_wait */
        (void)svcrt_sem_post(g_cv_gate);
    }
    app_puts("APP_TRACE w.gate.post\r\n");
    g_cv_probe[base + 6] = 2u;                          /* step 2: armed, still holding the lock */

    while(g_cv_gen < target)
    {
        loops++;
        g_cv_probe[base + 6] = 3u;                      /* step 3: entering cond_wait */
        rc = pthread_cond_wait(&g_cv, &g_cv_mtx);
        g_cv_probe[base + 2] = (uint32)rc;
        g_cv_probe[base + 4] = (uint32)loops;
        g_cv_probe[base + 6] = 4u;                      /* step 4: back from cond_wait */
        if(rc != 0)
        {
            break;
        }
    }

    g_cv_probe[base + 6] = 5u;                          /* step 5: about to unlock */
    lr = pthread_mutex_unlock(&g_cv_mtx);
    g_cv_probe[base + 3] = (uint32)lr;
    g_cv_probe[base + 6] = 6u;                          /* step 6: done */
    if(lr != 0)
    {
        return (void *)0xE2u;
    }
    return (rc == 0) ? (void *)0x600Du : (void *)0xBAD0u;
}

/* ---- FPU self test helpers (section 8c) ------------------------------ */

/* 直接占用 S16-S19 的两个探针。
 *
 * 为什么不用 C 变量来测：硬件在异常进入时只压 S0-S15+FPSCR，S16-S31
 * 必须由端口层在 svcrt_context.S 里手工保存。而编译器愿不愿意把一组
 * 局部变量留在 S16-S31 里是它自己的分配决定 —— 这版 AC5 把 20 个同时
 * 活跃的 float 全溢出到了栈上（反汇编里只出现 s0/s2/s4），靠 C 代码去
 * "逼"它，测出来的结果取决于编译器心情，不是内核行为。
 *
 * 所以这里用内嵌汇编直接写这几个寄存器：写进去、函数返回时故意不恢复
 * （违反 AAPCS 的 callee-saved 约定，这正是目的），让值停在寄存器里跨过
 * 随后的切换点。两个执行流各写自己的值、互相交错；只要端口层那段保存
 * 缺失，读回来的就是对方的数。
 *
 * 期望和 = seed + (seed+0x100) + (seed+0x200) + (seed+0x300) = 4*seed + 0x600
 */

#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)

/* Arm Compiler 6（armclang）：用 GCC 风格内联汇编。
 * s16-s19 故意不写进 clobber 列表 —— 一来 AA 里也放不进 VFP 寄存器，
 * 二来我们正是要它们"悄悄地"被改动，这才是被测行为。 */
static void fpu_probe_set(uint32 seed)
{
    uint32 k1 = seed + 0x100u;
    uint32 k2 = seed + 0x200u;
    uint32 k3 = seed + 0x300u;

    __asm volatile(
        "vmov   s16, %0  \n\t"
        "vmov   s17, %1  \n\t"
        "vmov   s18, %2  \n\t"
        "vmov   s19, %3  \n\t"
        : : "r"(seed), "r"(k1), "r"(k2), "r"(k3));
}

static uint32 fpu_probe_sum(void)
{
    uint32 t0;
    uint32 t1;
    uint32 t2;
    uint32 t3;

    __asm volatile(
        "vmov   %0, s16  \n\t"
        "vmov   %1, s17  \n\t"
        "vmov   %2, s18  \n\t"
        "vmov   %3, s19  \n\t"
        : "=r"(t0), "=r"(t1), "=r"(t2), "=r"(t3));

    return (t0 + t1 + t2 + t3);
}

#else

/* Arm Compiler 5：内嵌汇编函数（内核工程仍在用 AC5，保留这一支） */
__asm void fpu_probe_set(uint32 seed)
{
    VMOV    S16, R0
    ADD     R1, R0, #0x100
    VMOV    S17, R1
    ADD     R1, R0, #0x200
    VMOV    S18, R1
    ADD     R1, R0, #0x300
    VMOV    S19, R1
    BX      LR
}

__asm uint32 fpu_probe_sum(void)
{
    VMOV    R1, S16
    VMOV    R2, S17
    ADD     R1, R1, R2
    VMOV    R2, S18
    ADD     R1, R1, R2
    VMOV    R2, S19
    ADD     R1, R1, R2
    MOV     R0, R1
    BX      LR
}

#endif

/* 写 -> 让出（切换点）-> 读回 */
static uint32 fpu_probe_once(uint32 seed)
{
    fpu_probe_set(seed);
    svcrt_task_wait(5u);
    return fpu_probe_sum();
}

#define FPU_SEED_MAIN    0x11u
#define FPU_SEED_WORKER  0x22u
#define FPU_SEED_SUM(s)  ((s) * 4u + 0x600u)

static volatile int32 g_fpu_probe_bad = -1;

static void *fpu_probe_worker(void *arg)
{
    int32 i;

    (void)arg;
    g_fpu_probe_bad = 0;

    for(i = 1; i <= 20; i++)
    {
        if(fpu_probe_once(FPU_SEED_WORKER) != FPU_SEED_SUM(FPU_SEED_WORKER))
        {
            g_fpu_probe_bad = i;
            break;
        }
        svcrt_task_wait(2u);
    }
    return 0;
}

void AppMain(void)
{
    uint32 t0;
    uint32 t1;
    int32  sem = -1;
    int32  mtx = -1;
    int32  evt = -1;
    int32  mq  = -1;
    int32  tmr = -1;
    int32  r;
    uint32 mq_tx[2];
    uint32 mq_rx[2];

    /* ---------------------------------------------------------------
     * 1) device IO: three LEDs and the console UART
     * --------------------------------------------------------------- */
    g_led_r = svcrt_dev_open("LED", 0);
    g_led_g = svcrt_dev_open("LED2", 0);
    g_led_b = svcrt_dev_open("LED3", 0);
    g_con   = svcrt_dev_open("COM1", 115200);

    app_puts("\r\n=== SVCrtOS APP_DEMO self test ===\r\n");

    report("dev.open.led_r", (g_led_r >= 0), g_led_r);
    report("dev.open.led_g", (g_led_g >= 0), g_led_g);
    report("dev.open.led_b", (g_led_b >= 0), g_led_b);
    report("dev.open.com1",  (g_con   >= 0), g_con);

    /* Blink each LED once on/off to prove the write path end to end. */
    r = 0;
    if(g_led_r >= 0) { r |= led_on(g_led_r);  r |= led_off(g_led_r); }
    if(g_led_g >= 0) { r |= led_on(g_led_g);  r |= led_off(g_led_g); }
    if(g_led_b >= 0) { r |= led_on(g_led_b);  r |= led_off(g_led_b); }
    report("dev.write.leds", (r == 0), r);

    /* The console belongs to the kernel shell. Opening it again must hand
     * back the same shared instance instead of re-initialising the UART:
     * a second drv_open would orphan the shell's handle and the console
     * would stop receiving input. The read path is deliberately NOT probed
     * here - any read on this FIFO competes with the shell's reader and can
     * swallow a command meant for it. */
    {
        int32 shared = svcrt_dev_open("COM1", 115200);
        report("dev.open.com1_shared", (shared == g_con), shared);
    }

    /* ---------------------------------------------------------------
     * 2) time base and task wait
     * --------------------------------------------------------------- */
    t0 = svcrt_get_time_ms();
    svcrt_task_wait(50u);
    t1 = svcrt_get_time_ms();
    report("time.wait50", ((t1 - t0) >= 40u), (int32)(t1 - t0));

    /* Microsecond busy wait: must not deschedule, so it cannot overshoot. */
    t0 = svcrt_get_time_ms();
    svcrt_task_delay(200u);
    t1 = svcrt_get_time_ms();
    report("task.delay200us", ((t1 - t0) <= 5u), (int32)(t1 - t0));

    /* ---------------------------------------------------------------
     * 3) semaphore
     * --------------------------------------------------------------- */
    sem = svcrt_sem_create("app_sem", 0);
    report("sem.create", (sem >= 0), sem);

    r = svcrt_sem_wait(sem, 0);             /* empty + no wait -> must fail now */
    report("sem.wait_empty", (r <= 0), r);

    r = svcrt_sem_post(sem);
    report("sem.post", (r == 0), r);

    r = svcrt_sem_wait(sem, 100);           /* now it must succeed */
    report("sem.wait_ok", (r == 0), r);

    r = svcrt_sem_wait(sem, 20);            /* empty again -> timeout */
    report("sem.wait_timeout", (r <= 0), r);

    /* ---------------------------------------------------------------
     * 4) mutex
     * --------------------------------------------------------------- */
    mtx = svcrt_mutex_create("app_mtx");
    report("mtx.create", (mtx >= 0), mtx);

    r  = svcrt_mutex_lock(mtx, -1);
    r |= svcrt_mutex_unlock(mtx);
    report("mtx.lock_unlock", (r == 0), r);

    /* ---------------------------------------------------------------
     * 5) event: timeout path, then set path
     * --------------------------------------------------------------- */
    evt = svcrt_event_create("app_evt");
    report("evt.create", (evt >= 0), evt);

    t0 = svcrt_get_time_ms();
    svcrt_event_wait(evt, 50);
    t1 = svcrt_get_time_ms();
    report("evt.wait_timeout", ((t1 - t0) >= 40u), (int32)(t1 - t0));

    svcrt_event_set(evt);
    t0 = svcrt_get_time_ms();
    svcrt_event_wait(evt, 200);
    t1 = svcrt_get_time_ms();
    report("evt.wait_set", ((t1 - t0) < 100u), (int32)(t1 - t0));

    /* ---------------------------------------------------------------
     * 6) message queue
     * --------------------------------------------------------------- */
    mq = svcrt_mq_create("app_mq");
    report("mq.create", (mq >= 0), mq);

    mq_tx[0] = 0x5A5A1234u;
    mq_tx[1] = 0xCAFEBABEu;
    mq_rx[0] = 0u;
    mq_rx[1] = 0u;

    r = svcrt_mq_send(mq, mq_tx, 2, -1);
    report("mq.send", (r == 0), r);

    r = svcrt_mq_recv(mq, mq_rx, 2, 100);
    report("mq.recv", (r == 2), r);

    r = ((mq_rx[0] == mq_tx[0]) && (mq_rx[1] == mq_tx[1])) ? 0 : -1;
    report("mq.payload", (r == 0), r);

    r = svcrt_mq_recv(mq, mq_rx, 2, 20);    /* drained -> timeout */
    report("mq.recv_timeout", (r <= 0), r);

    /* ---------------------------------------------------------------
     * 7) software timer (periodic)
     * --------------------------------------------------------------- */
    tmr = svcrt_timer_create("app_tmr");
    report("tmr.create", (tmr >= 0), tmr);

    r = svcrt_timer_start(tmr, 200u, 1u, on_timer, 0);   /* 1 = periodic */
    report("tmr.start", (r == 0), r);

    svcrt_task_wait(700u);                  /* expect >= 3 callbacks */
    report("tmr.periodic", (g_timer_hits >= 2), g_timer_hits);

    r = svcrt_timer_stop(tmr);
    report("tmr.stop", (r == 0), r);

    {
        int32 hits = g_timer_hits;
        svcrt_task_wait(400u);
        report("tmr.stopped", (g_timer_hits == hits), g_timer_hits - hits);
    }

    /* ---------------------------------------------------------------
     * 8) diagnostics + partition view
     * --------------------------------------------------------------- */
    r = (int32)svcrt_get_cpu_usage();
    report("sys.cpu_usage", ((r >= 0) && (r <= 100)), r);

    r = svcrt_fault_record_count();
    report("sys.fault_count", (r >= 0), r);

    /* slot 0..3: 0=EMPTY, 1=LOADED, 2=RUNNING. Read-only view of the
     * partition table through SVC - no address is known on the App side. */
    r = svcrt_app_status(0u);
    report("app.status(slot0)", ((r >= 0) && (r <= 3)), r);

    /* ---------------------------------------------------------------
     * 8b) console services: user log and user command
     * --------------------------------------------------------------- */
    r = svcrt_log_print(SVCRT_LOG_INFO, "APPDEMO", "user log service online\r\n");
    report("log.print", (r == 0), r);

    r = svcrt_log_printf(SVCRT_LOG_INFO, "APPDEMO",
                         "log.printf smoke: dec=%d hex=0x%08X str=%s\r\n",
                         1234, 0xA5A5u, "ok");
    report("log.printf", (r == 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);
    report("shell.register", (r == 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);       /* duplicate name */
    report("shell.register_dup", (r < 0), r);

    r = svcrt_shell_cmd_unregister("appstat");
    report("shell.unregister", (r == 0), r);

    r = svcrt_shell_cmd_unregister("appstat");      /* already gone */
    report("shell.unregister_miss", (r < 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);       /* keep it for the console */
    report("shell.re_register", (r == 0), r);

    /* ---------------------------------------------------------------
     * 8c) FPU: 用户态可用性 + 切换时的浮点现场
     * --------------------------------------------------------------- */
    app_puts("APP_TEST fpu.begin\r\n");

    {
        /* 非特权代码执行 VFP 运算：算得出来就是能跑。CPACR 的
         * CP10/CP11 若不是 0b11，这一句会直接 UsageFault。 */
        volatile float fa = 1.5f;
        volatile float fb = 2.25f;
        float fc = fa * fb + 0.5f;          /* 1.5*2.25+0.5 = 3.875，精确 */

        report("fpu.usr_vfp_math", (fc == 3.875f), (int32)(fc * 1000.0f));
    }

    {
        /* 单流探针：写 S16-S19 -> 让出 5ms（期间必然切换）-> 读回 */
        uint32 got  = fpu_probe_once(FPU_SEED_MAIN);
        uint32 want = FPU_SEED_SUM(FPU_SEED_MAIN);

        report("fpu.s16_hold", (got == want), (int32)got);
    }

    {
        /* 两个流往同一组寄存器里写不同的值，来回切换 20 轮 */
        pthread_t th   = 0u;
        uint32    want = FPU_SEED_SUM(FPU_SEED_MAIN);
        int32     bad  = 0;
        int32     i;
        int32     cr;

        cr = pthread_create(&th, 0, fpu_probe_worker, (void *)0u);
        report("fpu.thread_create", (cr == 0), cr);

        for(i = 1; i <= 20; i++)
        {
            if(fpu_probe_once(FPU_SEED_MAIN) != want)
            {
                bad = i;
                break;
            }
        }

        if(cr == 0)
        {
            (void)pthread_join(th, 0);
        }

        report("fpu.s16_ctx_main", (bad == 0), bad);
        report("fpu.s16_ctx_worker", (g_fpu_probe_bad == 0), g_fpu_probe_bad);
    }

    {
        /* double 在 M4F 上没有硬件、走软件库，正确性同样要成立。
         * 1..1000 的和 = 500500，乘 0.5 = 250250，两种精度都精确。 */
        float  sf = 0.0f;
        double dd = 0.0;
        int32  i;
        int32  ok;

        for(i = 1; i <= 1000; i++)
        {
            sf += (float)i * 0.5f;
            dd += (double)i * 0.5;
        }

        ok = ((sf == 250250.0f) && (dd == 250250.0)) ? 1 : 0;
        report("fpu.float_vs_double", ok, (int32)sf);
    }

    /* ---------------------------------------------------------------
     * 9) POSIX / Windows compatibility layer
     *    Ordinary POSIX C below: no address, no slot, no kernel header.
     *    Every call ends up in an SVC the App SDK already publishes.
     * --------------------------------------------------------------- */
    {
        uint32 heap_before = svcrt_posix_heap_free_bytes();
        void  *p = svcrt_posix_malloc(64u);
        void  *q = svcrt_posix_calloc(4u, 16u);

        report("posix.malloc", (p != 0), (int32)svcrt_posix_heap_free_bytes());
        report("posix.calloc_zero", ((q != 0) && (((uint8 *)q)[0] == 0u)),
               (int32)svcrt_posix_heap_free_bytes());

        svcrt_posix_free(p);
        p = svcrt_posix_realloc(q, 256u);
        report("posix.realloc", (p != 0), (int32)svcrt_posix_heap_free_bytes());
        svcrt_posix_free(p);
        report("posix.heap_reclaim",
               (svcrt_posix_heap_free_bytes() >= (heap_before - 8u)),
               (int32)svcrt_posix_heap_free_bytes());
    }

    {
        pthread_t th = 0u;
        void     *ret = 0;
        int32     hits0 = g_pth_hits;
        int32     cr = pthread_create(&th, 0, pth_worker, (void *)0x5A5Au);

        report("posix.pthread_create", (cr == 0), cr);
        if(cr == 0)
        {
            int32 jr = pthread_join(th, &ret);

            report("posix.pthread_join", (jr == 0), jr);
            report("posix.pthread_ran", ((g_pth_hits - hits0) == 3),
                   (int32)(g_pth_hits - hits0));
            report("posix.pthread_retval", (ret == (void *)0x5A5Au),
                   (int32)(ret != 0));
        }
    }

    {
        svcrt_pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
        svcrt_sem_t ps;
        int32 r1;
        int32 r2;

        r1 = pthread_mutex_lock(&pm);
        r2 = pthread_mutex_unlock(&pm);
        report("posix.mutex", ((r1 == 0) && (r2 == 0)), (r1 | r2));

        r1 = sem_init(&ps, 0, 1u);
        r2 = sem_trywait(&ps);
        report("posix.sem_trywait", ((r1 == 0) && (r2 == 0)), (r1 | r2));

        r1 = sem_trywait(&ps);              /* empty: must report, not block */
        report("posix.sem_empty", (r1 < 0), r1);

        r1 = sem_post(&ps);
        r2 = sem_wait(&ps);
        report("posix.sem_wait", ((r1 == 0) && (r2 == 0)), (r1 | r2));
        (void)sem_destroy(&ps);
    }

    {
        uint32 ta = (uint32)svcrt_get_time_ms();
        int32  rr = usleep(20000u);
        uint32 tb = (uint32)svcrt_get_time_ms();

        report("posix.usleep", ((rr == 0) && ((tb - ta) >= 15u)),
               (int32)(tb - ta));
    }

    {
        svcrt_HANDLE wh;
        svcrt_DWORD  wr;
        int32        hits0 = g_pth_hits;

        wh = CreateThread(0, 1024u, win_worker, 0, 0u, 0);
        report("win.createthread", (wh != 0), (int32)(wh != 0));
        if(wh != 0)
        {
            wr = WaitForSingleObject(wh, INFINITE);
            report("win.wait_infinite", (wr == WAIT_OBJECT_0), (int32)wr);
            report("win.thread_ran", (g_pth_hits > hits0), g_pth_hits);
        }

        wr = WaitForSingleObject(wh, 0u);   /* 0 ms: must report, not hang */
        report("win.wait_timeout", (wr == WAIT_TIMEOUT), (int32)wr);
    }

    {
        char  dst[8];
        int32 sr;

        ZeroMemory(dst, sizeof(dst));
        sr = strcpy_s(dst, sizeof(dst), "hello");
        report("win.strcpy_s", ((sr == 0) && (dst[0] == 'h') && (dst[5] == 0)),
               sr);

        sr = strcpy_s(dst, sizeof(dst), "toolongforbuffer");
        report("win.strcpy_s_bounds", (sr < 0), sr);

        sr = sprintf_s(dst, sizeof(dst), "%d", 42);
        report("win.sprintf_s", ((sr > 0) && (dst[0] == '4') && (dst[2] == 0)),
               sr);
    }

    /* ---------------------------------------------------------------
     * 10) file system (SVC 0x1C)
     *     The App only knows paths. littlefs, the caches and the NOR all
     *     live in the kernel, and one call is one round trip: the kernel
     *     opens, moves the bytes and closes, so no handle survives and
     *     nothing has to be cleaned up when this App is stopped.
     * --------------------------------------------------------------- */
    {
        const char *fpath = "/app_fs.txt";
        const char *keep  = "/app_keep.txt";
        const char *text  = "app writes this to nor";
        char        back[48];
        uint32      fsize = 0u;
        uint32      isdir = 1u;
        uint32      total = 0u;
        uint32      used  = 0u;
        int32       n     = 0;
        int32       i;
        int32       diff  = 0;

        /* A mount does not survive a reset, so ask for the volume instead of
         * assuming the shell mounted it earlier: without this every reading
         * below would be "not mounted", which says nothing about the file
         * calls themselves. */
        r = svcrt_file_mount();
        report("fs.mount", (r == 0), r);

        for(i = 0; text[i] != '\0'; i++)
        {
            n++;
        }

        for(i = 0; i < (int32)sizeof(back); i++)
        {
            back[i] = 0;
        }

        r = svcrt_file_write(fpath, text, (uint32)n);
        report("fs.write", (r == 0), r);

        r = svcrt_file_stat(fpath, &fsize, &isdir);
        report("fs.stat_size", ((r == 0) && (fsize == (uint32)n) && (isdir == 0u)),
               (int32)fsize);

        r = svcrt_file_read(fpath, back, (uint32)sizeof(back));
        report("fs.read_len", (r == n), r);

        for(i = 0; i < n; i++)
        {
            if(back[i] != text[i])
            {
                diff = i + 1;
                break;
            }
        }
        report("fs.read_match", (diff == 0), diff);

        /* max is a capacity, not a request: a short buffer gets short bytes */
        r = svcrt_file_read(fpath, back, 8u);
        report("fs.read_cap", (r == 8), r);

        r = svcrt_file_info(&total, &used);
        report("fs.info", ((r == 0) && (total > 0u) &&
                           (used >= (uint32)n) && (used <= total)),
               (int32)total);

        /* failure paths must report, not invent a result */
        r = svcrt_file_stat("/no_such_file", &fsize, &isdir);
        report("fs.stat_missing", (r < 0), r);

        r = svcrt_file_write("relative.txt", text, (uint32)n);
        report("fs.write_badpath", (r < 0), r);

        /* an empty file is a legal file */
        r = svcrt_file_write("/app_empty.txt", "", 0u);
        report("fs.write_empty", (r == 0), r);

        r = svcrt_file_stat("/app_empty.txt", &fsize, &isdir);
        report("fs.empty_size", ((r == 0) && (fsize == 0u)), (int32)fsize);

        (void)svcrt_file_remove("/app_empty.txt");

        /* left for the shell: "fs ls" and "fs rd /app_keep.txt" must show
         * the same 23 bytes, which proves both sides share one volume */
        r = svcrt_file_write(keep, text, (uint32)n);
        report("fs.write_keep", (r == 0), r);

        r = svcrt_file_remove(fpath);
        report("fs.remove", (r == 0), r);

        r = svcrt_file_read(fpath, back, (uint32)sizeof(back));
        report("fs.read_removed", (r < 0), r);

        /* ---- random access: a slice of the file, not all of it ---- */
        {
            uint32 got_len = 0u;
            int32  rr;

            for(i = 0; i < 8; i++)
            {
                back[i] = 0;
            }

            rr = svcrt_file_read_at(keep, back, 4u, 4u);
            report("fs.read_at", ((rr == 4) && (back[0] == text[4])), rr);

            /* An offset at end of file is not an error, it is zero bytes */
            rr = svcrt_file_read_at(keep, back, 4u, (uint32)n);
            report("fs.read_at_eof", (rr == 0), rr);

            /* Past end of file behaves the same way */
            rr = svcrt_file_read_at(keep, back, 4u, (uint32)n + 64u);
            report("fs.read_at_past_eof", (rr == 0), rr);

            (void)got_len;
        }

        /* ---- rename: the new name is the file, the old one is gone ---- */
        r = svcrt_file_rename(keep, "/app_renamed.txt");
        report("fs.rename", (r == 0), r);

        r = svcrt_file_stat(keep, &fsize, &isdir);
        report("fs.rename_old_gone", (r < 0), r);

        r = svcrt_file_stat("/app_renamed.txt", &fsize, &isdir);
        report("fs.rename_new_here",
               ((r == 0) && (fsize == (uint32)n) && (isdir == 0u)), (int32)fsize);

        /* Renaming what is not there must report, not invent a file */
        r = svcrt_file_rename("/no_such_file", "/whatever.txt");
        report("fs.rename_missing", (r < 0), r);

        /* ---- listing: what files does the volume actually hold ---- */
        {
            static char names[160];
            uint32      cnt = 0u;

            for(i = 0; i < (int32)sizeof(names); i++)
            {
                names[i] = 0;
            }

            r = svcrt_file_list_names("/", names, (uint32)sizeof(names), &cnt);
            report("fs.list_names", ((r == 0) && (cnt >= 1u)), (int32)cnt);

            /* The renamed file has to show up under its new name */
            report("fs.list_has_new_name", (names_has(names, "app_renamed.txt") != 0),
                   (int32)cnt);
        }

        r = svcrt_file_remove("/app_renamed.txt");
        report("fs.cleanup", (r == 0), r);
    }

    /* ---------------------------------------------------------------
     * 10b) condition variable（内核对象层 + POSIX 层）
     * --------------------------------------------------------------- */
    {
        /* 先确认 SVC 通道本身通：创建 / 没人等时 signal / 删除 */
        int32 hc = svcrt_cond_create("app_cond");

        report("cond.create", (hc >= 0), hc);
        if(hc >= 0)
        {
            int32 cr = 0;

            cr |= svcrt_cond_signal(hc);
            cr |= svcrt_cond_broadcast(hc);
            report("cond.no_waiter_ok", (cr == 0), cr);
            report("cond.delete", (svcrt_cond_delete(hc) == 0), 0);
        }
    }

    {
        svcrt_pthread_t th = 0u;
        void     *ret = 0;
        int32     cr;
        int32     lr;
        int32     gr;
        int32     tr;
        int32     ur;
        int32     pr;
        uint32    t0;
        uint32    t1;

        /* 这两个宏在 POSIX 里都是惯用的 (obj, attr) 双参形态，attr 传 0 即默认属性 */
        report("posix.cond_init", (pthread_cond_init(&g_cv, 0) == 0), g_cv.handle);
        report("posix.cond_mtx_init",
               (pthread_mutex_init(&g_cv_mtx, 0) == 0), g_cv_mtx.handle);

        /* 会合信号量：把"等待者已经进 cond_wait"变成一个可等的确定事实，
         * 而不是靠"轮询几次看看"。 */
        g_cv_gate = svcrt_sem_create("cvgate", 0);
        report("posix.cond_gate_create", (g_cv_gate >= 0), g_cv_gate);

        /* ---- 有限超时探针 ----
         * 门信号量此刻计数必为 0，这一句应当在约 200 ms 后以"超时"返回。
         * 它同时是"内核有限超时路径通不通"的判据：timeout > 0 这条分支
         * 在这份自检里此前从没被走到过。通了，条件变量那两处 3000 ms 等待
         * 才谈得上"卡住就是别的原因"；不通，根因就在这里落定。 */
        app_puts("APP_TRACE c2 to-probe enter\r\n");
        t0 = svcrt_get_time_ms();
        tr = svcrt_sem_wait(g_cv_gate, 200);
        t1 = svcrt_get_time_ms();
        app_puts("APP_TRACE c2 to-probe done\r\n");
        note("sem.to.rc", tr);
        note("sem.to.ms", (int32)(t1 - t0));
        report("posix.sem_wait_timeout",
               ((tr != 0) && ((t1 - t0) >= 150u) && ((t1 - t0) < 1500u)),
               tr);

        /* ---- token conservation: one post buys exactly one wait ----
         * Starting from a count of 0, two waits must both time out.  After a
         * single post, exactly one wait may succeed and the next one must time
         * out again.  Two successes would mean the same post was consumed
         * twice, i.e. a token nobody ever gave away; a failure to consume it
         * would mean the opposite bookkeeping error. */
        {
            int32 cs = svcrt_sem_create("tok", 0);
            int32 w1;
            int32 w2;

            report("sem.tok_create", (cs >= 0), cs);

            if(cs >= 0)
            {
                w1 = svcrt_sem_wait(cs, 60);
                w2 = svcrt_sem_wait(cs, 60);
                report("sem.tok_empty_both_timeout",
                       ((w1 == -2) && (w2 == -2)), (w1 * 256) + (w2 & 0xff));

                (void)svcrt_sem_post(cs);

                w1 = svcrt_sem_wait(cs, 60);
                w2 = svcrt_sem_wait(cs, 60);
                report("sem.tok_post_consumed_once",
                       ((w1 == 0) && (w2 == -2)), (w1 * 256) + (w2 & 0xff));

                /* timeout 0 stays "try once": it must never block and never
                 * invent a token */
                w2 = svcrt_sem_wait(cs, 0);
                report("sem.tok_try_once", (w2 == -2), w2);

                (void)svcrt_sem_delete(cs);
            }
        }

        /* 没持锁就等：必须被拒。要是内核"顺手"替它交出一把不属于它的锁，
         * 这把锁就再也没人还回来了。 */
        report("cond.wait_not_owner",
               (svcrt_cond_wait(g_cv.handle, g_cv_mtx.handle, 50) < 0), 0);

        /* timeout 0 在条件变量上无意义（无法"不等待地等"），必须报错。
         * 若被当成"永久等待"，调用方会无声地睡死在这里。 */
        report("cond.wait_timeout0_rejected",
               (svcrt_cond_wait(g_cv.handle, g_cv_mtx.handle, 0) < 0), 0);

        /* ---- 单等待者：signal 唤醒 + 等待期间锁确实交出去了 ----
         *
         * 光看"主线程能拿到锁"证明不了什么：锁本来就空闲，谁都能立刻拿到。
         * 证据链要两步：① 等待者持锁后 post 会合信号量（说明它拿过锁、马上
         * 要进 cond_wait）；② 主线程随后能拿到锁（说明它确实交出来了）。 */
        g_cv_gen    = 0;
        g_cv_armed  = 0;
        g_cv_target = 1;
        g_cv_probe[0] = 0u; g_cv_probe[1] = 0u;
        g_cv_probe[2] = 0u; g_cv_probe[3] = 0u;
        g_cv_probe[4] = 0u; g_cv_probe[5] = 0u;
        g_cv_probe[6] = 0u; g_cv_probe[7] = 0u;

        cr = pthread_create(&th, 0, cv_waiter, (void *)0u);
        report("posix.cond_waiter_create", (cr == 0), cr);

        /* 第一步：等它举手。收不到就说明它压根没跑到 cond_wait 前面。 */
        gr = (cr == 0) ? svcrt_sem_wait(g_cv_gate, 3000) : -1;
        report("posix.cond_waiter_gate", (gr == 0), gr);

        /* 第二步：它举完手仍持着锁，这一句能拿到锁就说明锁被交出来了 */
        t0 = svcrt_get_time_ms();
        lr = svcrt_mutex_lock(g_cv_mtx.handle, 3000);
        t1 = svcrt_get_time_ms();
        note("c1.lock.raw", lr);
        note("c1.lock.ms", (int32)(t1 - t0));
        report("posix.cond_release_mtx", (lr == 0), lr);

        /* 不管上面成没成，都要把 gen 抬起来并唤醒：否则等待者会永久睡在
         * cond_wait 里，join 会把整段自检挂死。 */
        /* 广播前先看锁在谁手上：-1 = 自己持有（递归被拒） */
        pr = svcrt_mutex_lock(g_cv_mtx.handle, 0);
        note("c1.own.before_bcast", pr);
        if(pr == 0)
        {
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        g_cv_gen = 1;
        (void)pthread_cond_broadcast(&g_cv);

        /* 解锁前再看一次：这一步的读数直接决定"解锁失败"是"锁被转手了"
         * 还是"我们一开始就没拿着"。 */
        pr = svcrt_mutex_lock(g_cv_mtx.handle, 0);
        note("c1.own.before_unlock", pr);
        if(pr == 0)
        {
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        if(lr == 0)
        {
            /* 返回值不再是 (void)：解锁失败会让主线程一直"持有"这把锁，
             * 而后面所有的推理都建立在"没人持有"之上 —— 这种失败必须
             * 变成读数，不能吞掉。 */
            ur = pthread_mutex_unlock(&g_cv_mtx);
            note("c1.unlock.rc", ur);
        }
        pr = svcrt_mutex_lock(g_cv_mtx.handle, 0);
        note("c1.mtx.after", pr);
        if(pr == 0)
        {
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        if(cr == 0)
        {
            app_puts("APP_TRACE c2 join1 enter\r\n");
            (void)pthread_join(th, &ret);
            app_puts("APP_TRACE c2 join1 done\r\n");
            report("posix.cond_signal_wakes",
                   (ret == (void *)0x600Du), (int32)(uint32)ret);
        }
        dump_probe("w0.post", 0);
    }

    {
        svcrt_pthread_t th1 = 0u;
        svcrt_pthread_t th2 = 0u;
        void     *ret1 = 0;
        void     *ret2 = 0;
        int32     cr1;
        int32     cr2;
        int32     lr = -1;
        int32     g1 = -1;
        int32     g2 = -1;
        int32     pr;
        int32     armed_ok = 0;

        g_cv_gen    = 0;
        g_cv_armed  = 0;
        g_cv_target = 1;

        {
            int32 z;
            for(z = 0; z < 16; z++)
            {
                g_cv_probe[z] = 0u;
            }
        }

        /* 先探一次互斥量在不在别人手上（timeout=0 = 只试一次）。
         * 这一段后面所有推理都建立在"没人持有"之上，所以这个前提必须
         * 变成读数：拿到 0 = 空闲；拿到 -2 = 有人持有，推理前提不成立。 */
        pr = svcrt_mutex_lock(g_cv_mtx.handle, 0);
        note("c2.mtx.pre", pr);
        if(pr == 0)
        {
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        cr1 = pthread_create(&th1, 0, cv_waiter, (void *)0u);
        cr2 = pthread_create(&th2, 0, cv_waiter, (void *)1u);
        report("posix.cond_2_waiters", ((cr1 == 0) && (cr2 == 0)), cr1 | cr2);

        /* "两个人都进队列了吗"不靠轮询：App 主任务与新 pthread 同为优先级 10，
         * 锁空闲时 svcrt_mutex_lock 立即返回、不让出 CPU，轮询只会测出"对方还
         * 没被调度"。改成收 2 次会合信号——两人都举过手，才谈得上都进过队列。 */
        app_puts("APP_TRACE c2 g1 enter\r\n");
        g1 = (cr1 == 0) ? svcrt_sem_wait(g_cv_gate, 3000) : -1;
        app_puts("APP_TRACE c2 g1 done\r\n");
        app_puts("APP_TRACE c2 g2 enter\r\n");
        g2 = (cr2 == 0) ? svcrt_sem_wait(g_cv_gate, 3000) : -1;
        app_puts("APP_TRACE c2 g2 done\r\n");
        report("posix.cond_2_gates", ((g1 == 0) && (g2 == 0)), g1 | g2);

        armed_ok = 0;
        if((g1 == 0) && (g2 == 0))
        {
            /* 两人都举过手，此刻锁一定空闲（两人都在 cond_wait 里），能拿到
             * 就说明队列里确实蹲着两个人。
             * 但"举过手"不等于"进了 cond_wait"：举手发生在进 cond_wait 之前，
             * 只凭两张票就把后面的广播当成"唤醒了两个等待者"，会把一个还没
             * 睡下的线程算进结论。所以 armed 必须自己数到 2。 */
            lr = svcrt_mutex_lock(g_cv_mtx.handle, 3000);
            armed_ok = ((lr == 0) && (g_cv_armed == 2));
        }
        report("posix.cond_2_queued", armed_ok, g_cv_armed);

        /* 把每个等待者的真实进度和主线程自己的视角一起打出来：
         * 原始错误码不做布尔化，否则分不出"没跑到"和"看不到"。 */
        note("c2.armed.main", (int32)g_cv_armed);
        note("c2.lock.raw", lr);
        dump_probe("w0.pre", 0);
        dump_probe("w1.pre", 1);

        app_puts("APP_TRACE c2 broadcast enter\r\n");
        g_cv_gen = 1;
        (void)pthread_cond_broadcast(&g_cv);
        app_puts("APP_TRACE c2 broadcast done\r\n");
        if(armed_ok)
        {
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        /* join 必须有前提：若上面任何一步没成立，两个线程很可能正卡在
         * 互斥量上，join 会永久阻塞，整段自检就连 summary 也跑不到 ——
         * 那等于用一次挂死换掉全部读数。 */
        if((cr1 == 0) && armed_ok)
        {
            app_puts("APP_TRACE c2 join2 enter\r\n");
            (void)pthread_join(th1, &ret1);
            app_puts("APP_TRACE c2 join2 done\r\n");
        }
        else
        {
            ret1 = (void *)0xDEADu;
        }
        if((cr2 == 0) && armed_ok)
        {
            app_puts("APP_TRACE c2 join3 enter\r\n");
            (void)pthread_join(th2, &ret2);
            app_puts("APP_TRACE c2 join3 done\r\n");
        }
        else
        {
            ret2 = (void *)0xDEADu;
        }

        /* join 之后再打一次进度格：线程如果确实跑过，这里能看到它自己的
         * 锁/等待/解锁原始返回码；如果一直是 0，那就是压根没跑起来。 */
        dump_probe("w0.post", 0);
        dump_probe("w1.post", 1);
        report("posix.cond_broadcast_wakes",
               ((ret1 == (void *)0x600Du) && (ret2 == (void *)0x600Du)),
               (int32)((uint32)ret1 ^ (uint32)ret2));
    }

    {
        struct timespec ts;
        uint32 t0;
        uint32 t1;
        int32  trc;
        int32  te;
        int32  lr;

        g_cv_gen = 0;               /* 没人会 signal，只能靠超时醒来 */

        report("posix.clock_gettime",
               (svcrt_posix_clock_gettime(SVCRT_CLOCK_MONOTONIC, &ts) == 0), 0);

        ts.tv_nsec += 150000000;
        if(ts.tv_nsec >= 1000000000)
        {
            ts.tv_nsec -= 1000000000;
            ts.tv_sec  += 1;
        }

        lr = pthread_mutex_lock(&g_cv_mtx);
        report("posix.cond_timedwait_lock", (lr == 0), lr);
        if(lr == 0)
        {
            t0  = svcrt_get_time_ms();
            trc = pthread_cond_timedwait(&g_cv, &g_cv_mtx, &ts);
            te  = errno;
            t1  = svcrt_get_time_ms();

            note("tw.to.trc", trc);
            note("tw.to.errno", te);
            note("tw.to.ms", (int32)(t1 - t0));
            report("posix.cond_timedwait_to",
                   ((trc == -1) && (te == ETIMEDOUT) && ((t1 - t0) >= 100u)),
                   (int32)(t1 - t0));
            /* 超时路径同样要持锁返回，否则调用方会在解锁时拿到 -1 */
            report("posix.cond_timedwait_mtx_kept",
                   (pthread_mutex_unlock(&g_cv_mtx) == 0), 0);
        }

        /* 已经过去的时间点：立即 ETIMEDOUT，不能倒回去等一个负数 */
        ts.tv_sec  = 0;
        ts.tv_nsec = 0;
        if(pthread_mutex_lock(&g_cv_mtx) == 0)
        {
            trc = pthread_cond_timedwait(&g_cv, &g_cv_mtx, &ts);
            te  = errno;
            report("posix.cond_timedwait_past",
                   ((trc == -1) && (te == ETIMEDOUT)), te);
            (void)pthread_mutex_unlock(&g_cv_mtx);
        }

        report("posix.cond_destroy", (pthread_cond_destroy(&g_cv) == 0), 0);

        if(g_cv_gate >= 0)
        {
            (void)svcrt_sem_delete(g_cv_gate);
            g_cv_gate = -1;
        }
    }

    /* ---------------------------------------------------------------
     * 11) cleanup of the objects this App created
     * --------------------------------------------------------------- */
    r  = svcrt_sem_delete(sem);
    r |= svcrt_mutex_delete(mtx);
    r |= svcrt_mq_delete(mq);
    r |= svcrt_timer_delete(tmr);
    report("obj.delete", (r == 0), r);

    /* event has no delete API in the SDK: it is owned by the kernel table. */

    app_puts("APP_TEST summary: pass=");
    app_num(g_pass);
    app_puts(" fail=");
    app_num(g_fail);
    app_puts("\r\n");

    /* ---------------------------------------------------------------
     * 12) heartbeat: visible activity + periodic status line
     * --------------------------------------------------------------- */
    app_puts("APP_TEST heartbeat: LED3 toggles 4x/s, line every 5s\r\n");

    {
        uint32 beat = 0u;

        for(;;)
        {
            led_toggle(g_led_b);            /* blue: fast heartbeat */
            if((beat % 4u) == 0u)
            {
                led_toggle(g_led_g);        /* green: 1 Hz */
            }
            if((beat % 20u) == 0u)
            {
                led_toggle(g_led_r);        /* red: 5 s */
                app_puts("APP_ALIVE t=");
                app_num((int32)svcrt_get_time_ms());
                app_puts("ms cpu=");
                app_num((int32)svcrt_get_cpu_usage());
                app_puts("% timer_hits=");
                app_num(g_timer_hits);
                app_puts(" id=0x");
                app_hex8(0x41505031u);   /* "APP1" */
                app_puts("\r\n");
            }

            /* Heartbeat contract with the kernel guard: this slot promises
             * to report at least once every 1000 ms. The call below both
             * declares the period (first time) and reports liveness. */
            (void)svcrt_heartbeat(1000u);

            beat++;
            svcrt_task_wait(250u);
        }
    }
}
