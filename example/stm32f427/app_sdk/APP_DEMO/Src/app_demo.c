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

static void report(const char *name, int32 ok, int32 detail)
{
    app_puts("APP_TEST ");
    app_puts(name);
    app_puts(ok ? " ... OK" : " ... FAIL");
    if(!ok)
    {
        app_puts(" (");
        app_num(detail);
        app_puts(")");
    }
    app_puts("\r\n");

    if(ok)
    {
        g_pass++;
    }
    else
    {
        g_fail++;
    }
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
    (void)svcrt_shell_printf("APP_DEMO t=%d ms cpu=%d%% timer_hits=%d\r\n",
                             (int)svcrt_get_time_ms(),
                             (int)svcrt_get_cpu_usage(),
                             (int)g_timer_hits);

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
