/**
* @file seamless_v1.c
* @brief Seamless upgrade demo, version 1.0.0: the control loop that hands over.
* @details Long version of the story is in the seamless upgrade demo doc under docs/;
*
*          A control loop lives in RAM as a MiniApp and is loaded from the
*          volume, so writing a new version into that volume never touches the
*          on-chip flash and never freezes the CPU. This version runs the loop
*          at 10 ms and, while it runs, it polls one small file for a handover
*          request. When a newer version appears and asks for control, this one
*          writes its live state once, says so on the console, and returns -
*          the kernel then hands back both of its RAM blocks.
*
*          The loop never writes to the volume while it is in control. The
*          release is a single write, which is what keeps the handover gap in
*          the milliseconds instead of the hundreds.
*
* @note Part of the seamless upgrade example; not part of the kernel.
*/

#include "svcrt.h"
#include "seamless_proto.h"

/* ---------------- build-time identity ---------------- */

#define SXP_VERSION_STR   "1.0.0"
#define SXP_VERSION_NUM   SXP_VER_V1
#define SXP_OWNER         SXP_OWNER_V1
#define SXP_TAG           "[v1]"

/* ---------------- timing ---------------- */

/* Control period. V2 runs at 5 ms with retuned gains, and that difference is
 * part of what makes the handover visible from the console. */
#define SXP_PERIOD_MS     (10u)

/* How often the loop looks for a handover request. This interval is the
 * handover latency, measured from the moment the request is on the volume.
 *
 * It is deliberately ten times the control period. A poll is not free: it
 * opens, reads and closes one record through littlefs and the SPI NOR, and the
 * measured cost of that round trip is about 13 ms (reported as "pr=" on the
 * status line). Polling at the loop rate therefore does not fit in a 10 ms
 * period - the loop would just run at the speed of the file system. One poll
 * per ten periods keeps the loop at its own rate and the handover well inside
 * a tenth of a second. */
#define SXP_POLL_MS       (100u)

#define SXP_PRINT_MS      (1000u)
#define SXP_BEAT_MS       (100u)

/* The declared heartbeat window. It has to cover the longest this loop can be
 * kept off the CPU, which is the scheduler lock held while another MiniApp is
 * loaded from the volume. A window shorter than that pause makes the guard
 * report a broken promise (correctly) and can starve the watchdog, so this is
 * not a number to shrink casually. The guard refuses anything under 100 ms. */
#define SXP_HEARTBEAT_MS  (500u)

/* ---------------- control law, all in thousandths ---------------- */

#define SXP_KP_MILLI      (800)
#define SXP_KI_MILLI      (150)
#define SXP_TAU_MS        (1000u)
#define SXP_U_LIMIT_MILLI (200000)
#define SXP_I_LIMIT_MILLI (200000)

#define SXP_LINE_MAX      (160u)

/* ---------------- output line buffer ----------------
 * One svcrt_dev_write is one complete SVC round trip, and two MiniApps share
 * the console. Building a whole line and writing it once keeps another task
 * from splicing its own text into the middle of this one. */

static int32 g_con = -1;
static char  g_line[SXP_LINE_MAX];
static uint32 g_len;

static void lb_reset(void)
{
    g_len = 0u;
}

static void lb_ch(char c)
{
    if(g_len < (SXP_LINE_MAX - 1u))
    {
        g_line[g_len] = c;
        g_len++;
    }
}

static void lb_str(const char *s)
{
    while(*s != '\0')
    {
        lb_ch(*s);
        s++;
    }
}

static void lb_u32(uint32 v)
{
    char t[12];
    int32 i = 12;

    t[--i] = '\0';
    do
    {
        t[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while(v != 0u);

    lb_str(&t[i]);
}

/* Print a thousandths value as "123.456". */
static void lb_milli(int32 v)
{
    uint32 f;

    if(v < 0)
    {
        lb_ch('-');
        v = -v;
    }
    lb_u32((uint32)(v / SXP_MILLI));
    lb_ch('.');
    f = (uint32)(v % SXP_MILLI);
    lb_ch((char)('0' + (int32)((f / 100u) % 10u)));
    lb_ch((char)('0' + (int32)((f / 10u) % 10u)));
    lb_ch((char)('0' + (int32)(f % 10u)));
}

static void lb_flush(void)
{
    if(g_len != 0u)
    {
        (void)svcrt_dev_write(g_con, g_line, (int32)g_len);
    }
    g_len = 0u;
}

/* ---------------- live state ---------------- */

static sxp_state_t g_st;

static uint32 g_adopted;

/* Longest handover-request poll seen so far, in ms. Measured on the device:
 * the number that explains the loop's real period while it watches the volume. */
static uint32 g_poll_max_ms;

/* How long the last state publication took, in ms. A publication is a littlefs
 * create/truncate plus a metadata commit on the SPI NOR, which is two orders of
 * magnitude more expensive than reading the same record back. Having the device
 * say so is the difference between measuring the handover and guessing at it. */
static uint32 g_pub_ms;

/* A step profile that is a pure function of the wall clock. Resuming with a
 * wrong time base, or with state that never arrived, shows up immediately as a
 * jump in the reference instead of hiding inside the numbers. */
static int32 setpoint_at(uint32 t_ms)
{
    uint32 idx = (t_ms / 5000u) % 4u;

    return 60000 + (int32)(idx * 20000u);
}

static int due(uint32 now, uint32 when)
{
    return ((int32)(now - when) >= 0) ? 1 : 0;
}

/* One control iteration: PI controller plus a first-order plant, both in
 * thousandths. dt comes from the actual elapsed time rather than the nominal
 * period, so a late iteration is accounted for instead of being lost. */
static void step(uint32 dt_ms)
{
    int32 err;
    int32 u;

    if(dt_ms == 0u)
    {
        dt_ms = SXP_PERIOD_MS;
    }

    err = g_st.setpoint - g_st.pv;

    g_st.integ += (err * (int32)dt_ms) / SXP_MILLI;
    if(g_st.integ >  SXP_I_LIMIT_MILLI)
    {
        g_st.integ =  SXP_I_LIMIT_MILLI;
    }
    if(g_st.integ < -SXP_I_LIMIT_MILLI)
    {
        g_st.integ = -SXP_I_LIMIT_MILLI;
    }

    u = ((SXP_KP_MILLI * err) / SXP_MILLI)
      + ((SXP_KI_MILLI * g_st.integ) / SXP_MILLI);
    if(u >  SXP_U_LIMIT_MILLI)
    {
        u =  SXP_U_LIMIT_MILLI;
    }
    if(u < -SXP_U_LIMIT_MILLI)
    {
        u = -SXP_U_LIMIT_MILLI;
    }
    g_st.out = u;

    /* Plant: dy = (K*u - y) * dt / tau, K = 1. */
    g_st.pv += ((u - g_st.pv) * (int32)dt_ms) / (int32)SXP_TAU_MS;
}

/* ---------------- volume side ---------------- */

static void write_state(void)
{
    uint8 buf[SXP_STATE_SIZE];
    uint32 t0 = svcrt_get_time_ms();

    g_st.magic = SXP_STATE_MAGIC;
    g_st.seq++;
    sxp_state_encode(&g_st, buf);

    /* A failed write is reported, not swallowed: a handover that silently did
     * not publish its state is the one outcome the new version must not act
     * on. */
    if(svcrt_file_write(SXP_STATE_PATH, buf, SXP_STATE_SIZE) != 0)
    {
        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" WARNING: could not write ");
        lb_str(SXP_STATE_PATH);
        lb_str("\r\n");
        lb_flush();
    }

    g_pub_ms = svcrt_get_time_ms() - t0;
}

/* 1 when a valid handover request is on the volume, 0 otherwise.
 * "Absent", "half written" and "the file cache is busy because the host is
 * uploading" all mean the same thing here - no request yet - so none of them is
 * treated as an error. */
static int request_pending(void)
{
    uint8 buf[SXP_REQ_SIZE];
    sxp_req_t rq;
    int32 n;

    n = svcrt_file_read(SXP_REQ_PATH, buf, SXP_REQ_SIZE);
    if(n != (int32)SXP_REQ_SIZE)
    {
        return 0;
    }
    if(sxp_req_decode(buf, SXP_REQ_SIZE, &rq) == 0)
    {
        return 0;
    }

    /* Only a request aimed at something newer than this version is this
     * version's to honour. A request left behind by a takeover that gave up,
     * or one addressed at an even newer version, is not ours: acting on it
     * would take the loop out of the only writer there is. */
    return ((rq.cmd == SXP_CMD_HANDOVER) &&
            (rq.to_version > SXP_VERSION_NUM)) ? 1 : 0;
}

/* ---------------- console ---------------- */

static void announce(void)
{
    lb_reset();
    lb_str("\r\n");
    lb_str(SXP_TAG);
    lb_str(" seamless control loop v");
    lb_str(SXP_VERSION_STR);
    lb_str(", period ");
    lb_u32(SXP_PERIOD_MS);
    lb_str(" ms, heartbeat ");
    lb_u32(SXP_HEARTBEAT_MS);
    lb_str(" ms, owner ");
    lb_u32(SXP_OWNER);
    lb_str("\r\n");
    lb_flush();
}

static void print_status(uint32 now)
{
    lb_reset();
    lb_str(SXP_TAG);
    lb_str(" v=");
    lb_str(SXP_VERSION_STR);
    lb_str(" t=");
    lb_u32(now);
    lb_str(" it=");
    lb_u32(g_st.iterations);
    lb_str(" sp=");
    lb_milli(g_st.setpoint);
    lb_str(" pv=");
    lb_milli(g_st.pv);
    lb_str(" u=");
    lb_milli(g_st.out);
    lb_str(" gap=");
    lb_u32(g_st.gap_max_ms);
    lb_str(" over=");
    lb_u32(g_st.overruns);
    lb_str(" pr=");
    lb_u32(g_poll_max_ms);
    lb_str("\r\n");
    lb_flush();
}

/* Publish the live state once and stop controlling. Everything after this
 * write belongs to the new version; this instance must not touch the
 * controlled variable again, which is what makes a single writer hold. */
static void release(uint32 now)
{
    lb_reset();
    lb_str(SXP_TAG);
    lb_str(" handover requested at t=");
    lb_u32(now);
    lb_str(" ms, it=");
    lb_u32(g_st.iterations);
    lb_str(", gap_max=");
    lb_u32(g_st.gap_max_ms);
    lb_str(" ms\r\n");
    lb_flush();

    g_st.phase = SXP_PHASE_RELEASED;
    g_st.tick_ms = now;
    write_state();

    /* Clear the request so a later run does not read a stale one and hand over
     * to nobody. */
    (void)svcrt_file_remove(SXP_REQ_PATH);

    lb_reset();
    lb_str(SXP_TAG);
    lb_str(" released, state published in ");
    lb_u32(g_pub_ms);
    lb_str(" ms; returning to free both RAM blocks\r\n");
    lb_flush();
}

/* ---------------- entry ---------------- */

void MiniMain(void)
{
    uint32 now;
    uint32 last;
    uint32 next_print;
    uint32 next_poll;
    uint32 next_beat;
    uint8  buf[SXP_STATE_SIZE];
    sxp_state_t prev;
    int32  n;

    g_con = svcrt_dev_open("COM1", 115200);

    /* Do not lean on .bss being zeroed on our behalf. */
    sxp_zero((uint8 *)&g_st, (uint32)sizeof(g_st));
    sxp_zero((uint8 *)&prev, (uint32)sizeof(prev));
    g_adopted = 0u;
    g_poll_max_ms = 0u;

    g_st.magic = SXP_STATE_MAGIC;
    g_st.owner = SXP_OWNER;
    g_st.period_ms = SXP_PERIOD_MS;

    now = svcrt_get_time_ms();

    /* Is somebody already in control? The state record answers that, and its
     * owner field is the version identity. */
    n = svcrt_file_read(SXP_STATE_PATH, buf, SXP_STATE_SIZE);
    if((n == (int32)SXP_STATE_SIZE) &&
       (sxp_state_decode(buf, SXP_STATE_SIZE, &prev) != 0) &&
       (prev.phase == SXP_PHASE_ACTIVE))
    {
        if(prev.owner != SXP_OWNER)
        {
            /* A newer version is in control. Refusing is the whole point of
             * carrying a version in the record: an old binary must not seize
             * the loop back from a newer one just because it was started. */
            lb_reset();
            lb_str(SXP_TAG);
            lb_str(" v");
            lb_str(SXP_VERSION_STR);
            lb_str(" refuses to take over: owner ");
            lb_u32(prev.owner);
            lb_str(" is in control (version 0x");
            lb_u32(sxp_owner_version(prev.owner));
            lb_str(") at it=");
            lb_u32(prev.iterations);
            lb_str("\r\n");
            lb_flush();
            return;
        }

        /* Our own state from an earlier run: carry it on instead of starting
         * the loop over from zero. */
        g_st = prev;
        g_st.magic = SXP_STATE_MAGIC;
        g_st.period_ms = SXP_PERIOD_MS;
        g_adopted = 1u;
    }

    if(g_adopted != 0u)
    {
        announce();
        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" adopted state left by an earlier run: it=");
        lb_u32(g_st.iterations);
        lb_str(", pv=");
        lb_milli(g_st.pv);
        lb_str("\r\n");
        lb_flush();
    }
    else
    {
        g_st.iterations = 0u;
        g_st.setpoint = setpoint_at(now);
        g_st.pv = 0;
        g_st.integ = 0;
        g_st.out = 0;
        g_st.gap_max_ms = 0u;
        g_st.overruns = 0u;
        announce();
    }

    g_st.phase = SXP_PHASE_ACTIVE;
    g_st.tick_ms = now;
    write_state();

    /* The publication above is the expensive part of starting up, and it is
     * not a stall of the control loop: report it on its own line and then
     * start the gap counters, so "gap_max" means "the worst the loop ever
     * saw" - which is what a load pause has to be read against. */
    lb_reset();
    lb_str(SXP_TAG);
    lb_str(" state published in ");
    lb_u32(g_pub_ms);
    lb_str(" ms; gap counters reset, loop starts\r\n");
    lb_flush();

    g_st.gap_max_ms = 0u;
    g_st.overruns = 0u;
    g_poll_max_ms = 0u;
    now = svcrt_get_time_ms();
    g_st.tick_ms = now;

    svcrt_heartbeat(SXP_HEARTBEAT_MS);

    last       = now - SXP_PERIOD_MS;   /* so the first dt is one period */
    next_print = now + SXP_PRINT_MS;
    next_poll  = now + SXP_POLL_MS;
    next_beat  = now + SXP_BEAT_MS;

    for(;;)
    {
        uint32 gap;

        now = svcrt_get_time_ms();
        gap = now - last;
        last = now;

        if(gap > g_st.gap_max_ms)
        {
            g_st.gap_max_ms = gap;
        }
        if(gap > (2u * SXP_PERIOD_MS))
        {
            g_st.overruns++;
        }

        g_st.setpoint = setpoint_at(now);
        step(gap);
        g_st.iterations++;
        g_st.tick_ms = now;

        if(due(now, next_beat) != 0)
        {
            svcrt_heartbeat(SXP_HEARTBEAT_MS);
            next_beat = now + SXP_BEAT_MS;
        }

        if(due(now, next_print) != 0)
        {
            print_status(now);
            next_print = now + SXP_PRINT_MS;
        }

        if(due(now, next_poll) != 0)
        {
            uint32 t0;
            uint32 cost;
            int32  pending;

            next_poll = now + SXP_POLL_MS;

            /* Time the poll so the loop's own period can be explained from
             * the device rather than guessed at. The clock has 1 ms
             * resolution, which is enough to see a file system round trip. */
            t0 = svcrt_get_time_ms();
            pending = request_pending();
            cost = svcrt_get_time_ms() - t0;
            if(cost > g_poll_max_ms)
            {
                g_poll_max_ms = cost;
            }

            if(pending != 0)
            {
                /* Read the clock again: the poll just cost milliseconds, and
                 * the release timestamp is what the new version uses to
                 * measure the handover gap. */
                release(svcrt_get_time_ms());
                return;             /* the kernel returns both RAM blocks */
            }
        }

        svcrt_task_wait(SXP_PERIOD_MS);
    }
}
