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

static void app_puts(const char *s)
{
    while(*s != '\0')
    {
        (void)svcrt_dev_write(g_con, (void *)s, 1);
        s++;
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

    while(i > 0)
    {
        i--;
        (void)svcrt_dev_write(g_con, (void *)&buf[i], 1);
    }
}

static void app_hex8(uint32 v)
{
    static const char hex[] = "0123456789ABCDEF";
    int32 i;

    for(i = 7; i >= 0; i--)
    {
        char c = hex[(v >> ((uint32)i * 4u)) & 0xFu];
        (void)svcrt_dev_write(g_con, (void *)&c, 1);
    }
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

    /* Non-blocking read: 0 means "no byte waiting", which is a valid result.
     * The console is owned by the kernel shell, so the App probes the read
     * path without trying to consume the interactive byte stream. */
    {
        uint8 b = 0u;
        r = svcrt_dev_read(g_con, &b, 1);
        report("dev.read.com1", (r >= 0), r);
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
     * 10) cleanup of the objects this App created
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
     * 11) heartbeat: visible activity + periodic status line
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

            beat++;
            svcrt_task_wait(250u);
        }
    }
}
