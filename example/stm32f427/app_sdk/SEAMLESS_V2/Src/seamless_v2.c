/**
* @file seamless_v2.c
* @brief Seamless upgrade demo, version 1.1.0: the control loop that takes over.
* @details Same loop as version 1.0.0, retuned and running twice as fast, plus
*          the other half of the handover:
*
*          1. If nobody is in control, start the loop (this version is usable
*             on its own).
*          2. If an older version is in control, ask it to hand over, then wait
*             for it to publish a RELEASED state. Only then continue from that
*             state - never before, because taking control while the incumbent
*             still holds it would put two writers on one controlled variable.
*          3. If the incumbent does not release within the timeout, report it
*             and exit, leaving the incumbent in control. That keeps the system
*             consistent without a rollback mechanism; it is not one.
*
*          The retuning is deliberate: period 10 ms -> 5 ms and different PI
*          gains, so the console shows a different loop after the handover and
*          "the new version took over" is something the log proves rather than
*          something the reader is asked to believe.
*
* @note Part of the seamless upgrade example; not part of the kernel.
*/

#include "svcrt.h"
#include "seamless_proto.h"

/* ---------------- build-time identity ---------------- */

#define SXP_VERSION_STR   "1.1.0"
#define SXP_VERSION_NUM   SXP_VER_V2
#define SXP_OWNER         SXP_OWNER_V2
#define SXP_TAG           "[v2]"

/* ---------------- timing ---------------- */

/* Twice the rate of version 1.0.0. */
#define SXP_PERIOD_MS     (5u)

/* How often the loop looks for a handover request, and how often the takeover
 * wait re-reads the incumbent's state record.
 *
 * Ten times the control period, same reasoning as in version 1.0.0: one poll is
 * one littlefs open/read/close over SPI NOR and it is measured at roughly 13 ms
 * (the "pr=" column), so polling at 5 ms would silently turn this loop into a
 * file-system-rate loop. */
#define SXP_POLL_MS       (50u)

#define SXP_PRINT_MS      (1000u)
#define SXP_BEAT_MS       (100u)
#define SXP_HEARTBEAT_MS  (500u)

/* How long to wait for the incumbent to release. Past this, the polite
 * assumption ("it is still working") stops being believable and the correct
 * move is to leave it in control and say so. */
#define SXP_TAKEOVER_MS   (5000u)

/* ---------------- control law, all in thousandths ---------------- */

#define SXP_KP_MILLI      (1200)
#define SXP_KI_MILLI      (400)
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

/* Print a packed version as "1.1.0". */
static void lb_version(uint32 v)
{
    lb_u32((v >> 16) & 0xFFu);
    lb_ch('.');
    lb_u32((v >> 8) & 0xFFu);
    lb_ch('.');
    lb_u32(v & 0xFFu);
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

/* Longest handover-request poll seen so far, in ms: device evidence for why
 * this loop's period is not simply SXP_PERIOD_MS. */
static uint32 g_poll_max_ms;

/* How long the last state publication took, in ms: a littlefs commit on the
 * SPI NOR, which is the dominant term in the handover gap. */
static uint32 g_pub_ms;

/* A step profile that is a pure function of the wall clock, identical to the
 * one version 1.0.0 uses. A wrong time base, or state that never arrived,
 * therefore shows up as a jump in the reference rather than hiding inside the
 * numbers. */
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
 * thousandths. dt comes from the actual elapsed time, so the iteration lost to
 * the handover is accounted for by the first iteration after it instead of
 * being silently dropped. */
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

/* Read the state record. Returns 1 when there is a valid one and copies it to
 * *out; 0 for absent, half written, or unreadable because the file cache is
 * busy with a host upload. */
static int read_state(sxp_state_t *out)
{
    uint8 buf[SXP_STATE_SIZE];
    int32 n;

    n = svcrt_file_read(SXP_STATE_PATH, buf, SXP_STATE_SIZE);
    if(n != (int32)SXP_STATE_SIZE)
    {
        return 0;
    }

    return sxp_state_decode(buf, SXP_STATE_SIZE, out);
}

static void write_request(uint32 from_version, uint32 now)
{
    sxp_req_t rq;
    uint8 buf[SXP_REQ_SIZE];

    sxp_zero((uint8 *)&rq, (uint32)sizeof(rq));
    rq.magic = SXP_REQ_MAGIC;
    rq.from_version = from_version;
    rq.to_version = SXP_VERSION_NUM;
    rq.cmd = SXP_CMD_HANDOVER;
    rq.req_tick_ms = now;
    sxp_req_encode(&rq, buf);

    if(svcrt_file_write(SXP_REQ_PATH, buf, SXP_REQ_SIZE) != 0)
    {
        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" WARNING: could not write ");
        lb_str(SXP_REQ_PATH);
        lb_str("\r\n");
        lb_flush();
    }
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

/* Version 1.1.0 prints one more column than 1.0.0: the gap of the iteration
 * that just ran. The extra field is a second, independent way to tell from the
 * log which version is in control. */
static void print_status(uint32 now, uint32 last_gap)
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
    lb_str(" last=");
    lb_u32(last_gap);
    lb_str("\r\n");
    lb_flush();
}

/* ---------------- entry ---------------- */

/* Run the loop until a newer version appears and asks for control. Returns
 * normally when the loop has been released; the caller then finishes. */
static void run_loop(uint32 now)
{
    uint32 last = now - SXP_PERIOD_MS;   /* so the first dt is one period */
    uint32 next_print = now + SXP_PRINT_MS;
    uint32 next_poll = now + SXP_POLL_MS;
    uint32 next_beat = now + SXP_BEAT_MS;
    sxp_req_t rq;
    uint8 buf[SXP_REQ_SIZE];

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
            print_status(now, gap);
            next_print = now + SXP_PRINT_MS;
        }

        if(due(now, next_poll) != 0)
        {
            uint32 t0;
            uint32 cost;
            int32  n;

            next_poll = now + SXP_POLL_MS;

            t0 = svcrt_get_time_ms();
            n = svcrt_file_read(SXP_REQ_PATH, buf, SXP_REQ_SIZE);
            cost = svcrt_get_time_ms() - t0;
            if(cost > g_poll_max_ms)
            {
                g_poll_max_ms = cost;
            }

            /* A request that is not ours does not stop this loop: whoever asks
             * for control gets it by starting a newer version, and that version
             * writes its own request. */
            if((n ==
                (int32)SXP_REQ_SIZE) &&
               (sxp_req_decode(buf, SXP_REQ_SIZE, &rq) != 0) &&
               (rq.cmd == SXP_CMD_HANDOVER) &&
               (rq.to_version > SXP_VERSION_NUM))
            {
                lb_reset();
                lb_str(SXP_TAG);
                lb_str(" handover requested by version ");
                lb_version(rq.to_version);
                lb_str(" at t=");
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
                (void)svcrt_file_remove(SXP_REQ_PATH);

                lb_reset();
                lb_str(SXP_TAG);
                lb_str(" released, state published in ");
                lb_u32(g_pub_ms);
                lb_str(" ms; returning to free both RAM blocks\r\n");
                lb_flush();
                return;
            }
        }

        svcrt_task_wait(SXP_PERIOD_MS);
    }
}

void MiniMain(void)
{
    uint32 now;
    uint32 deadline;
    uint32 handover_gap;
    uint32 released;
    sxp_state_t prev;
    int32 n;

    g_con = svcrt_dev_open("COM1", 115200);

    /* Do not lean on .bss being zeroed on our behalf. */
    sxp_zero((uint8 *)&g_st, (uint32)sizeof(g_st));
    sxp_zero((uint8 *)&prev, (uint32)sizeof(prev));
    g_poll_max_ms = 0u;

    g_st.magic = SXP_STATE_MAGIC;
    g_st.owner = SXP_OWNER;
    g_st.period_ms = SXP_PERIOD_MS;

    now = svcrt_get_time_ms();

    n = read_state(&prev);
    if((n != 0) && (prev.phase == SXP_PHASE_ACTIVE) && (prev.owner == SXP_OWNER))
    {
        /* Our own state, left by an earlier run. There is no way to tell that
         * apart from a twin instance of ourselves, and the guard against two
         * writers is worth more than the convenience of resuming silently, so
         * this is treated as "start over from the persisted numbers" and said
         * out loud. */
        g_st = prev;
        g_st.magic = SXP_STATE_MAGIC;
        g_st.period_ms = SXP_PERIOD_MS;
        g_st.phase = SXP_PHASE_ACTIVE;
        g_st.tick_ms = now;
        announce();
        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" adopted state left by an earlier run of v");
        lb_str(SXP_VERSION_STR);
        lb_str(": it=");
        lb_u32(g_st.iterations);
        lb_str(", pv=");
        lb_milli(g_st.pv);
        lb_str("\r\n");
        lb_flush();
        write_state();
        svcrt_heartbeat(SXP_HEARTBEAT_MS);
        run_loop(now);
        return;
    }

    if((n != 0) && (prev.phase == SXP_PHASE_ACTIVE) && (prev.owner != SXP_OWNER))
    {
        /* Somebody else is in control: ask, then wait for it to say it has
         * released. */
        announce();
        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" asking version ");
        lb_version(sxp_owner_version(prev.owner));
        lb_str(" (owner ");
        lb_u32(prev.owner);
        lb_str(", it=");
        lb_u32(prev.iterations);
        lb_str(") to hand over at t=");
        lb_u32(now);
        lb_str(" ms\r\n");
        lb_flush();

        write_request(sxp_owner_version(prev.owner), now);

        deadline = now + SXP_TAKEOVER_MS;
        released = 0;
        for(;;)
        {
            now = svcrt_get_time_ms();
            if(due(now, deadline) != 0)
            {
                break;
            }
            svcrt_task_wait(SXP_POLL_MS);

            if((read_state(&prev) != 0) &&
               (prev.phase == SXP_PHASE_RELEASED))
            {
                released = 1;
                break;
            }
        }

        /* Read the clock again: the value captured before the last wait is up
         * to one poll interval stale, and the handover gap is the one number
         * this whole example exists to report. */
        now = svcrt_get_time_ms();

        if(released == 0)
        {
            /* Nothing was granted inside the window. Leave the incumbent
             * alone: two writers on one controlled variable is a worse outcome
             * than a failed upgrade, and reporting it is cheaper than
             * pretending it worked.
             *
             * Take the request back on the way out. A request left on the
             * volume is a request the incumbent will still find, and it would
             * then release the loop to a version that has already given up -
             * nobody in control is the one outcome worse than a slow handover.
             * The requester wrote it, so the requester removes it. */
            (void)svcrt_file_remove(SXP_REQ_PATH);

            lb_reset();
            lb_str(SXP_TAG);
            lb_str(" no release within ");
            lb_u32(SXP_TAKEOVER_MS);
            lb_str(" ms; leaving v");
            lb_version(sxp_owner_version(prev.owner));
            lb_str(" in control, exiting\r\n");
            lb_flush();
            return;
        }

        handover_gap = now - prev.tick_ms;

        /* Continue the incumbent's loop: iterations, plant, integrator and the
         * gap counters all carry over, so nothing is restarted at zero. */
        g_st = prev;
        g_st.magic = SXP_STATE_MAGIC;
        g_st.owner = SXP_OWNER;
        g_st.period_ms = SXP_PERIOD_MS;
        g_st.phase = SXP_PHASE_ACTIVE;
        g_st.tick_ms = now;

        write_state();
        (void)svcrt_file_remove(SXP_REQ_PATH);

        lb_reset();
        lb_str(SXP_TAG);
        lb_str(" took over from v");
        lb_version(sxp_owner_version(prev.owner));
        lb_str(": handover_gap=");
        lb_u32(handover_gap);
        lb_str(" ms (publication " );
        lb_u32(g_pub_ms);
        lb_str(" ms), resumed at it=");
        lb_u32(g_st.iterations);
        lb_str(" sp=");
        lb_milli(g_st.setpoint);
        lb_str(" pv=");
        lb_milli(g_st.pv);
        lb_str(" integ=");
        lb_milli(g_st.integ);
        lb_str(", period now ");
        lb_u32(SXP_PERIOD_MS);
        lb_str(" ms\r\n");
        lb_flush();

        svcrt_heartbeat(SXP_HEARTBEAT_MS);
        run_loop(now);
        return;
    }

    /* Nobody is in control: this version starts the loop on its own. */
    g_st.iterations = 0u;
    g_st.setpoint = setpoint_at(now);
    g_st.pv = 0;
    g_st.integ = 0;
    g_st.out = 0;
    g_st.gap_max_ms = 0u;
    g_st.overruns = 0u;
    g_st.phase = SXP_PHASE_ACTIVE;
    g_st.tick_ms = now;
    announce();
    lb_reset();
    lb_str(SXP_TAG);
    lb_str(" no state on the volume: starting the loop from scratch\r\n");
    lb_flush();
    write_state();
    svcrt_heartbeat(SXP_HEARTBEAT_MS);
    run_loop(now);
}
