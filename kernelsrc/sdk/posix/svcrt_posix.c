/**
* @file svcrt_posix.c
* @brief SVCrtOS POSIX compatibility layer - implementation.
* @details Three blocks, in this order:
*            1. errno        - one cell per thread, addressed through SVC 0x1B
*            2. heap         - first fit arena with coalescing, in App RAM
*            3. the facades  - pthread / semaphore / mqueue / unistd / time
*
*          pthread_create deserves a word. The kernel thread service takes a
*          bare "void (*)(void)", while POSIX hands over a start routine plus
*          an argument. The bridge is a trampoline plus one slot per thread:
*          the creator builds the slot, then starts the thread, and only then
*          releases a start gate semaphore. The trampoline finds its own slot
*          through the kernel task id - which is why thread creation cannot
*          simply run and hope the slot is filled in: the gate makes the
*          ordering explicit instead of relying on luck with preemption.
*
* @author xw
*/
#include <stdarg.h>
#include "svcrt_posix.h"
#include "svcrt_win_compat.h"
#include "svcrt_features.h"  /* feature gates: SVCRT_USE_POSIX */
#include "svcrt_net_abi.h"   /* wire contract of SVC 0x1E (sockets) */
#include "svcrt_svc_call.h"  /* SVCRT_SVC_DECL_1 for the call itself */
#if SVCRT_USE_POSIX

/* ==========================================================================
 * 1. errno
 * ========================================================================== */

/* One cell per possible task id. The kernel task table is the index space, so
 * the array is sized by the same ceiling the kernel uses - but it lives here,
 * in App RAM, because errno is userspace state.
 */
#define SVCRT_POSIX_ERRNO_SLOTS  (64)

static int32 svcrt_posix_errno_cells[SVCRT_POSIX_ERRNO_SLOTS];

int32 *svcrt_posix_errno_location(void)
{
    int32 tid = svcrt_thread_self();

    if(tid <= 0)
    {
        return &svcrt_posix_errno_cells[0];
    }
    if(tid >= SVCRT_POSIX_ERRNO_SLOTS)
    {
        /* Task ids beyond the table share the last cell rather than running
         * off the end. Losing per thread precision there is a documented
         * ceiling, not an out of bounds write. */
        tid = SVCRT_POSIX_ERRNO_SLOTS - 1;
    }
    return &svcrt_posix_errno_cells[tid];
}

/* The C library has an errno of its own, reached through
 * __aeabi_errno_addr(). Its <errno.h> defines the name "errno"
 * unconditionally, so an App that includes <stdio.h> - or anything else
 * that drags <errno.h> in - before our header keeps the library's macro,
 * while this file keeps ours: two cells, one written and one read, and
 * every failure reported as "errno=0". Redefining the library's accessor
 * here points both names at the same per-thread cell, whatever the
 * include order. (Defining the symbol also keeps the library's own errno
 * object out of the image, for the same reason.) */
volatile int *__aeabi_errno_addr(void);

volatile int *__aeabi_errno_addr(void)
{
    return (volatile int *)svcrt_posix_errno_location();
}

/* ==========================================================================
 * 2. heap
 * ========================================================================== */

typedef struct svcrt_heap_hdr
{
    uint32 size;        /* whole block, header included, multiple of 8 */
    uint32 used;
} svcrt_heap_hdr_t;

#define SVCRT_HEAP_HDR_SIZE  ((uint32)sizeof(svcrt_heap_hdr_t))
#define SVCRT_HEAP_ALIGN     (8u)

/* A union with a 64 bit member is the portable way to ask both armcc and
 * armclang for 8 byte alignment without compiler specific attributes. */
typedef union
{
    uint32 words[SVCRT_POSIX_HEAP_SIZE / 4u];
    unsigned long long align8;
} svcrt_heap_arena_t;

typedef union
{
    uint32 words[SVCRT_POSIX_STACK_WORDS];
    unsigned long long align8;
} svcrt_thread_stack_t;

/* The arena must hold at least one header and stay a multiple of the block
 * alignment, otherwise splitting would produce misaligned payloads. */
typedef char svcrt_heap_size_check[
    ((SVCRT_POSIX_HEAP_SIZE >= (SVCRT_HEAP_HDR_SIZE + 64u)) &&
     ((SVCRT_POSIX_HEAP_SIZE % SVCRT_HEAP_ALIGN) == 0u)) ? 1 : -1];
typedef char svcrt_heap_hdr_check[(SVCRT_HEAP_HDR_SIZE == 8u) ? 1 : -1];

static svcrt_heap_arena_t svcrt_posix_heap_arena;
static uint32 svcrt_posix_heap_ready = 0u;

static void svcrt_posix_heap_init(void)
{
    svcrt_heap_hdr_t *blk;

    if(svcrt_posix_heap_ready != 0u)
    {
        return;
    }
    blk = (svcrt_heap_hdr_t *)((void *)&svcrt_posix_heap_arena);
    blk->size = SVCRT_POSIX_HEAP_SIZE;
    blk->used = 0u;
    svcrt_posix_heap_ready = 1u;
}

/* Merge every pair of neighbouring free blocks. Walking by block size is what
 * makes this O(n) in the number of blocks and needs no free list. */
static void svcrt_posix_heap_coalesce(void)
{
    uint32 start = (uint32)&svcrt_posix_heap_arena;
    uint32 end   = start + SVCRT_POSIX_HEAP_SIZE;
    svcrt_heap_hdr_t *blk = (svcrt_heap_hdr_t *)((void *)&svcrt_posix_heap_arena);

    while(((uint32)blk + SVCRT_HEAP_HDR_SIZE) <= end)
    {
        uint32 next = (uint32)blk + blk->size;

        if((next + SVCRT_HEAP_HDR_SIZE) > end)
        {
            break;
        }
        if((blk->used == 0u) && (((svcrt_heap_hdr_t *)next)->used == 0u))
        {
            svcrt_heap_hdr_t *nb = (svcrt_heap_hdr_t *)next;
            blk->size += nb->size;
            continue;               /* try to swallow a third block too */
        }
        blk = (svcrt_heap_hdr_t *)next;
    }
}

void *svcrt_posix_malloc(uint32 size)
{
    uint32 start;
    uint32 end;
    uint32 need;
    svcrt_heap_hdr_t *blk;

    if(size == 0u)
    {
        errno = EINVAL;
        return 0;
    }

    /* Round the payload up, then add the header. Both steps are checked for
     * wrap around so a huge request fails cleanly instead of wrapping into a
     * "small" allocation. */
    need = (size + (SVCRT_HEAP_ALIGN - 1u)) & ~(SVCRT_HEAP_ALIGN - 1u);
    if(need < size)
    {
        errno = ENOMEM;
        return 0;
    }
    if(need > (SVCRT_POSIX_HEAP_SIZE - SVCRT_HEAP_HDR_SIZE))
    {
        errno = ENOMEM;
        return 0;
    }
    need += SVCRT_HEAP_HDR_SIZE;

    svcrt_posix_heap_init();
    start = (uint32)&svcrt_posix_heap_arena;
    end   = start + SVCRT_POSIX_HEAP_SIZE;
    blk   = (svcrt_heap_hdr_t *)((void *)&svcrt_posix_heap_arena);

    while(((uint32)blk + SVCRT_HEAP_HDR_SIZE) <= end)
    {
        if((blk->used == 0u) && (blk->size >= need))
        {
            /* Split only when the remainder can still hold a header plus a
             * useful payload; otherwise hand over the whole block. */
            if(blk->size >= (need + SVCRT_HEAP_HDR_SIZE + SVCRT_HEAP_ALIGN))
            {
                svcrt_heap_hdr_t *tail = (svcrt_heap_hdr_t *)((uint32)blk + need);

                tail->size = blk->size - need;
                tail->used = 0u;
                blk->size  = need;
            }
            blk->used = 1u;
            return (void *)((uint32)blk + SVCRT_HEAP_HDR_SIZE);
        }
        blk = (svcrt_heap_hdr_t *)((uint32)blk + blk->size);
    }

    errno = ENOMEM;
    return 0;
}

void svcrt_posix_free(void *ptr)
{
    uint32 start = (uint32)&svcrt_posix_heap_arena;
    uint32 end   = start + SVCRT_POSIX_HEAP_SIZE;
    svcrt_heap_hdr_t *blk;

    if(ptr == 0)
    {
        return;
    }
    blk = (svcrt_heap_hdr_t *)((uint32)ptr - SVCRT_HEAP_HDR_SIZE);
    /* Refuse a pointer that is not inside the arena instead of writing the
     * "used" flag into whatever happens to sit there. */
    if(((uint32)blk < start) || (((uint32)blk + SVCRT_HEAP_HDR_SIZE) > end))
    {
        errno = EINVAL;
        return;
    }
    blk->used = 0u;
    svcrt_posix_heap_coalesce();
}

void *svcrt_posix_calloc(uint32 nmemb, uint32 size)
{
    uint32 total = nmemb * size;
    void  *p;

    if((nmemb != 0u) && ((total / nmemb) != size))
    {
        errno = ENOMEM;                 /* multiplication overflowed */
        return 0;
    }
    p = svcrt_posix_malloc(total);
    if(p != 0)
    {
        (void)svcrt_posix_memset(p, 0, total);
    }
    return p;
}

void *svcrt_posix_realloc(void *ptr, uint32 size)
{
    svcrt_heap_hdr_t *blk;
    uint32 old_payload;
    uint32 copy;
    void  *fresh;

    if(ptr == 0)
    {
        return svcrt_posix_malloc(size);
    }
    if(size == 0u)
    {
        svcrt_posix_free(ptr);
        return 0;
    }

    blk = (svcrt_heap_hdr_t *)((uint32)ptr - SVCRT_HEAP_HDR_SIZE);
    old_payload = blk->size - SVCRT_HEAP_HDR_SIZE;
    if(old_payload >= size)
    {
        return ptr;                     /* already big enough, keep the block */
    }

    fresh = svcrt_posix_malloc(size);
    if(fresh == 0)
    {
        return 0;
    }
    copy = (old_payload < size) ? old_payload : size;
    (void)svcrt_posix_memcpy(fresh, ptr, copy);
    svcrt_posix_free(ptr);
    return fresh;
}

uint32 svcrt_posix_heap_total(void)
{
    return SVCRT_POSIX_HEAP_SIZE;
}

uint32 svcrt_posix_heap_free_bytes(void)
{
    uint32 start = (uint32)&svcrt_posix_heap_arena;
    uint32 end   = start + SVCRT_POSIX_HEAP_SIZE;
    svcrt_heap_hdr_t *blk;
    uint32 free_bytes = 0u;

    svcrt_posix_heap_init();
    blk = (svcrt_heap_hdr_t *)((void *)&svcrt_posix_heap_arena);
    while(((uint32)blk + SVCRT_HEAP_HDR_SIZE) <= end)
    {
        if(blk->used == 0u)
        {
            free_bytes += (blk->size - SVCRT_HEAP_HDR_SIZE);
        }
        blk = (svcrt_heap_hdr_t *)((uint32)blk + blk->size);
    }
    return free_bytes;
}

/* ==========================================================================
 * libc bits the layer needs. Implemented here rather than pulled from the C
 * library so the compatibility layer has no library dependency of its own.
 * ========================================================================== */

void *svcrt_posix_memset(void *dst, int value, uint32 len)
{
    uint8 *p = (uint8 *)dst;
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        p[i] = (uint8)value;
    }
    return dst;
}

void *svcrt_posix_memcpy(void *dst, const void *src, uint32 len)
{
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        d[i] = s[i];
    }
    return dst;
}

uint32 svcrt_posix_strlen(const char *s)
{
    uint32 n = 0u;

    if(s == 0)
    {
        return 0u;
    }
    while(s[n] != '\0')
    {
        n++;
    }
    return n;
}

/* ==========================================================================
 * 3. pthread
 * ========================================================================== */

typedef struct
{
    void   *(*start)(void *);       /* entry point                           */
    void    *arg;
    void    *retval;
    uint32   in_use;
    int32    task_id;               /* kernel task id, -1 once the thread left */
    int32    gate;                  /* start gate, posted by the creator     */
    int32    done;                  /* posted by the trampoline on exit      */
    uint32  *stack;
    uint32   stack_bytes;
} svcrt_pth_slot_t;

static svcrt_pth_slot_t svcrt_pth_slots[SVCRT_POSIX_THREAD_MAX];
static svcrt_thread_stack_t svcrt_pth_stacks[SVCRT_POSIX_THREAD_MAX];
static uint32 svcrt_pth_ready = 0u;
static uint32 svcrt_pth_next_id = 1u;
/* 创建者在 svcrt_thread_create() 之前发布的槽位号。
 * 内核分配 task_id 只能在 create 返回之后才落到槽位上，这个窗口里新线程
 * 可能已经被调度到；没有这个握手，它就只能靠"扫不到自己"来退出。 */
static volatile int32 svcrt_pth_boot_slot = -1;

static void svcrt_pth_init(void)
{
    uint32 i;

    if(svcrt_pth_ready != 0u)
    {
        return;
    }
    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        svcrt_pth_slots[i].in_use   = 0u;
        svcrt_pth_slots[i].task_id  = -1;
        svcrt_pth_slots[i].gate     = -1;
        svcrt_pth_slots[i].done     = -1;
    }
    svcrt_pth_ready = 1u;
}

/* The trampoline is the kernel visible entry point. It has no argument, so it
 * recovers its slot from the kernel task id - the creator guarantees that
 * field is filled in before the gate is released. */
/* Trampoline bookkeeping for the App side shell dump ("pthinfo").  A join
 * that never returns has four possible stories, and three of them are
 * countable: the slot was claimed normally, it was claimed through the boot
 * handshake, the worker gave up and left without a slot, or the completion
 * post happened.  Reporting an opinion instead of one of those was the old
 * behaviour. */
volatile int32 svcrt_posix_pth_dbg_claim = 0;   /* slot claimed by task id    */
volatile int32 svcrt_posix_pth_dbg_boot  = 0;   /* claimed via boot_slot      */
volatile int32 svcrt_posix_pth_dbg_lost  = 0;   /* gave up: no slot to be     */
volatile int32 svcrt_posix_pth_dbg_post  = 0;   /* trampoline posted done     */
volatile int32 svcrt_posix_pth_dbg_exit  = 0;   /* pthread_exit() posted done */

int32 svcrt_posix_pth_dbg_slots(svcrt_pth_dbg_t *out)
{
    uint32 i;

    if(out == 0)
    {
        return -1;
    }
    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        out[i].in_use  = svcrt_pth_slots[i].in_use;
        out[i].task_id = svcrt_pth_slots[i].task_id;
        out[i].gate    = svcrt_pth_slots[i].gate;
        out[i].done    = svcrt_pth_slots[i].done;
    }
    return 0;
}

static void svcrt_pth_trampoline(void)
{
    int32 tid = svcrt_thread_self();
    uint32 i;
    uint32 idx = (uint32)-1;

    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        if((svcrt_pth_slots[i].in_use != 0u) && (svcrt_pth_slots[i].task_id == tid))
        {
            idx = i;
            svcrt_posix_pth_dbg_claim++;
            break;
        }
    }
    if(idx == (uint32)-1)
    {
        int32 pending = svcrt_pth_boot_slot;

        /* 扫不到自己不等于"没有自己"：内核刚把本线程放上就绪表时，创建者
         * 还没来得及把 task_id 写进槽位。这里用创建者预先发布的槽位号认领，
         * 然后等它把 task_id 补齐（这正是那把 start gate 存在的理由）。
         * 直接退出会让 pthread_join 永远等不到 done。 */
        if((pending >= 0) && (pending < (int32)SVCRT_POSIX_THREAD_MAX) &&
           (svcrt_pth_slots[pending].in_use != 0u))
        {
            uint32 spin;

            idx = (uint32)pending;
            svcrt_posix_pth_dbg_boot++;
            for(spin = 0u; spin < 200u; spin++)
            {
                if(svcrt_pth_slots[idx].task_id == tid)
                {
                    break;
                }
                /* 有界等待：闸门会在创建者发布完槽位后被 post */
                (void)svcrt_sem_wait(svcrt_pth_slots[idx].gate, 5);
            }
            if(svcrt_pth_slots[idx].task_id != tid)
            {
                idx = (uint32)-1;
            }
        }
    }
    if(idx == (uint32)-1)
    {
        svcrt_posix_pth_dbg_lost++;
        /* Still nobody to be. Ending the thread is the only safe answer -
         * running somebody else's start routine would be far worse. But it
         * must not be silent: the slot bookkeeping stays wrong forever. */
        svcrt_thread_exit();
        return;
    }

    {
        svcrt_pth_slot_t *slot = &svcrt_pth_slots[idx];
        void *retval;

        retval = slot->start(slot->arg);

        slot->retval = retval;
        svcrt_posix_pth_dbg_post++;
        /* Order matters: publish the result, wake the joiner, and only then
         * drop the task id so a recycled id cannot be matched to this slot. */
        (void)svcrt_sem_post(slot->done);
        slot->task_id = -1;
    }
    svcrt_thread_exit();
}

int svcrt_posix_pthread_attr_init(svcrt_pthread_attr_t *attr)
{
    if(attr == 0)
    {
        errno = EINVAL;
        return -1;
    }
    attr->stackaddr = 0;
    attr->stacksize = 0u;
    attr->priority  = 0u;
    attr->period_ms = 0u;
    return 0;
}

int svcrt_posix_pthread_create(svcrt_pthread_t *thread,
                               const svcrt_pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg)
{
    uint32 i;
    int32  idx = -1;
    svcrt_pth_slot_t *slot;
    uint32 priority;
    uint32 stack_bytes;
    uint32 *stack;
    char   name[8];
    int32  task_id;

    if((thread == 0) || (start_routine == 0))
    {
        errno = EINVAL;
        return -1;
    }

    svcrt_pth_init();
    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        if(svcrt_pth_slots[i].in_use == 0u)
        {
            idx = (int32)i;
            break;
        }
    }
    if(idx < 0)
    {
        errno = EAGAIN;
        return -1;
    }
    slot = &svcrt_pth_slots[idx];

    if((attr != 0) && (attr->stackaddr != 0))
    {
        stack       = (uint32 *)attr->stackaddr;
        stack_bytes = attr->stacksize;
    }
    else
    {
        stack       = svcrt_pth_stacks[idx].words;
        stack_bytes = (uint32)SVCRT_POSIX_STACK_WORDS * 4u;
    }
    if(stack_bytes < 128u)
    {
        errno = EINVAL;
        return -1;
    }

    priority = ((attr != 0) && (attr->priority != 0u))
                   ? attr->priority : (uint32)SVCRT_POSIX_DEFAULT_PRIO;

    /* Two semaphores per thread: the start gate, and the completion signal a
     * joiner waits on. Names must stay within the kernel's 8 byte window. */
    name[0] = 'p'; name[1] = 't'; name[2] = 'h';
    name[3] = (char)('0' + (svcrt_pth_next_id % 10u));
    name[4] = 'g'; name[5] = '\0';
    slot->gate = svcrt_sem_create(name, 0);
    if(slot->gate < 0)
    {
        errno = ENOMEM;
        return -1;
    }
    name[4] = 'd';
    slot->done = svcrt_sem_create(name, 0);
    if(slot->done < 0)
    {
        (void)svcrt_sem_delete(slot->gate);
        slot->gate = -1;
        errno = ENOMEM;
        return -1;
    }
    svcrt_pth_next_id++;

    slot->start     = start_routine;
    slot->arg       = arg;
    slot->retval    = 0;
    slot->stack     = stack;
    slot->stack_bytes = stack_bytes;
    slot->task_id   = -1;
    slot->in_use    = 1u;

    /* 先把槽位号放出去，再建线程：新线程可能立刻被调度到，它需要一个
     * 与 task_id 无关的握手点来认领自己的槽位。 */
    svcrt_pth_boot_slot = idx;

    task_id = svcrt_thread_create(svcrt_pth_trampoline, stack, stack_bytes,
                                  priority, ((attr != 0) ? attr->period_ms : 0u));
    if(task_id <= 0)
    {
        (void)svcrt_sem_delete(slot->gate);
        (void)svcrt_sem_delete(slot->done);
        slot->gate = -1;
        slot->done = -1;
        slot->in_use = 0u;
        svcrt_pth_boot_slot = -1;
        /* The kernel refuses for one of a small set of reasons; the two a
         * caller can act on are "no task slot" and "bad arguments". */
        errno = (task_id == -1) ? EINVAL : EAGAIN;
        return -1;
    }

    slot->task_id = task_id;
    *thread = (svcrt_pthread_t)(idx + 1);

    /* Release the start gate only now: every field the trampoline reads is in
     * place, so the new thread cannot observe a half built slot. */
    (void)svcrt_sem_post(slot->gate);
    svcrt_pth_boot_slot = -1;
    return 0;
}

int svcrt_posix_pthread_join(svcrt_pthread_t thread, void **retval)
{
    uint32 idx;
    svcrt_pth_slot_t *slot;

    if((thread == 0u) || (thread > (svcrt_pthread_t)SVCRT_POSIX_THREAD_MAX))
    {
        errno = ESRCH;
        return -1;
    }
    idx  = (uint32)thread - 1u;
    slot = &svcrt_pth_slots[idx];
    if(slot->in_use == 0u)
    {
        errno = ESRCH;
        return -1;
    }

    /* -1 = wait forever, matching pthread_join's contract. */
    if(svcrt_sem_wait(slot->done, -1) != 0)
    {
        errno = EINTR;
        return -1;
    }

    if(retval != 0)
    {
        *retval = slot->retval;
    }

    (void)svcrt_sem_delete(slot->gate);
    (void)svcrt_sem_delete(slot->done);
    slot->gate = -1;
    slot->done = -1;
    slot->in_use = 0u;
    slot->task_id = -1;
    return 0;
}

void svcrt_posix_pthread_exit(void *retval)
{
    uint32 i;
    int32  tid = svcrt_thread_self();

    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        if((svcrt_pth_slots[i].in_use != 0u) && (svcrt_pth_slots[i].task_id == tid))
        {
            svcrt_pth_slots[i].retval = retval;
            svcrt_posix_pth_dbg_exit++;
            (void)svcrt_sem_post(svcrt_pth_slots[i].done);
            svcrt_pth_slots[i].task_id = -1;
            break;
        }
    }
    svcrt_thread_exit();
}

svcrt_pthread_t svcrt_posix_pthread_self(void)
{
    uint32 i;
    int32  tid = svcrt_thread_self();

    for(i = 0u; i < (uint32)SVCRT_POSIX_THREAD_MAX; i++)
    {
        if((svcrt_pth_slots[i].in_use != 0u) && (svcrt_pth_slots[i].task_id == tid))
        {
            return (svcrt_pthread_t)(i + 1u);
        }
    }
    /* 0 means "not a thread this layer created" - the App main task. */
    return SVCRT_PTHREAD_NULL;
}

/* ---- mutex ------------------------------------------------------------ */

int svcrt_posix_mutex_init(svcrt_pthread_mutex_t *m)
{
    if(m == 0)
    {
        errno = EINVAL;
        return -1;
    }
    m->handle = svcrt_mutex_create("pthmtx");
    if(m->handle < 0)
    {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int svcrt_posix_mutex_destroy(svcrt_pthread_mutex_t *m)
{
    if((m == 0) || (m->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    (void)svcrt_mutex_delete(m->handle);
    m->handle = -1;
    return 0;
}

int svcrt_posix_mutex_lock(svcrt_pthread_mutex_t *m)
{
    if(m == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(m->handle < 0)
    {
        /* PTHREAD_MUTEX_INITIALIZER leaves the handle at -1; create lazily so
         * the common static initialisation idiom works. */
        m->handle = svcrt_mutex_create("pthmtx");
        if(m->handle < 0)
        {
            errno = ENOMEM;
            return -1;
        }
    }
    if(svcrt_mutex_lock(m->handle, -1) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int svcrt_posix_mutex_trylock(svcrt_pthread_mutex_t *m)
{
    if(m == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(m->handle < 0)
    {
        m->handle = svcrt_mutex_create("pthmtx");
        if(m->handle < 0)
        {
            errno = ENOMEM;
            return -1;
        }
    }
    if(svcrt_mutex_lock(m->handle, 0) != 0)
    {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

int svcrt_posix_mutex_unlock(svcrt_pthread_mutex_t *m)
{
    if((m == 0) || (m->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_mutex_unlock(m->handle) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* ==========================================================================
 * 3c. condition variable
 * ========================================================================== */

/* Create on first use so PTHREAD_COND_INITIALIZER works, the same way the
 * mutex does it. The kernel table is fixed, so a failure here is ENOMEM and
 * is reported as such - never a silent success on an unbound handle. */
static int svcrt_posix_cond_bind(svcrt_pthread_cond_t *c)
{
    if(c->handle >= 0)
    {
        return 0;
    }
    c->handle = svcrt_cond_create("pthcond");
    if(c->handle < 0)
    {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int svcrt_posix_cond_init(svcrt_pthread_cond_t *c)
{
    if(c == 0)
    {
        errno = EINVAL;
        return -1;
    }
    c->handle = -1;
    return svcrt_posix_cond_bind(c);
}

int svcrt_posix_cond_destroy(svcrt_pthread_cond_t *c)
{
    if((c == 0) || (c->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    /* Wake anyone still waiting and tell them the object is gone; the kernel
     * does that inside cond_delete. */
    (void)svcrt_cond_delete(c->handle);
    c->handle = -1;
    return 0;
}

/* Shared by cond_wait and cond_timedwait. timeout_ms < 0 means wait forever.
 * Returns 0, or -1 with errno set. */
static int svcrt_posix_cond_wait_ms(svcrt_pthread_cond_t *c,
                                    svcrt_pthread_mutex_t *m, int32 timeout_ms)
{
    int32 rc;

    if((c == 0) || (m == 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(m->handle < 0)
    {
        /* The caller cannot be holding a mutex that was never created. */
        errno = EINVAL;
        return -1;
    }
    if(svcrt_posix_cond_bind(c) != 0)
    {
        return -1;
    }

    rc = svcrt_cond_wait(c->handle, m->handle, timeout_ms);
    if(rc == 0)
    {
        return 0;
    }
    /* Every failure path inside the kernel re-acquires the mutex before it
     * returns, so the caller is still inside its critical section here. */
    if(rc == SVCRT_SYNC_ERR_TIMEOUT)
    {
        errno = ETIMEDOUT;
    }
    else
    {
        errno = EINVAL;
    }
    return -1;
}

int svcrt_posix_cond_wait(svcrt_pthread_cond_t *c, svcrt_pthread_mutex_t *m)
{
    return svcrt_posix_cond_wait_ms(c, m, -1);
}

int svcrt_posix_cond_timedwait(svcrt_pthread_cond_t *c, svcrt_pthread_mutex_t *m,
                               const struct timespec *abstime)
{
    uint32 now_ms;
    uint32 tar_ms;
    int32  rel_ms;

    if(abstime == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(abstime->tv_nsec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    /* tv_sec is 32 bit here. Rather than let the multiply wrap into a
     * deadline in the past, anything past the representable millisecond
     * range is taken as "wait forever" - which is what the caller asking for
     * year 2100 effectively wants on a board with no wall clock. */
    if(abstime->tv_sec > ((0xFFFFFFFFu - 999u) / 1000u))
    {
        return svcrt_posix_cond_wait_ms(c, m, -1);
    }

    tar_ms = (abstime->tv_sec * 1000u) + (uint32)(abstime->tv_nsec / 1000000);
    now_ms = svcrt_get_time_ms();

    if(tar_ms <= now_ms)
    {
        /* Already expired: the POSIX answer is ETIMEDOUT, and the mutex must
         * still be held on return - so check that before bailing out. */
        if((c == 0) || (m == 0) || (m->handle < 0))
        {
            errno = EINVAL;
            return -1;
        }
        errno = ETIMEDOUT;
        return -1;
    }

    rel_ms = (int32)(tar_ms - now_ms);
    return svcrt_posix_cond_wait_ms(c, m, rel_ms);
}

int svcrt_posix_cond_signal(svcrt_pthread_cond_t *c)
{
    if((c == 0) || (c->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_cond_signal(c->handle) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int svcrt_posix_cond_broadcast(svcrt_pthread_cond_t *c)
{
    if((c == 0) || (c->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_cond_broadcast(c->handle) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* ==========================================================================
 * 3b. semaphore
 * ========================================================================== */

int svcrt_posix_sem_init(svcrt_sem_t *sem, int pshared, uint32 value)
{
    if(sem == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(pshared != 0)
    {
        /* A process shared semaphore would have to be named so another App
         * could find it. SVCrtOS has no such registry, so say so rather than
         * silently build something only this App can see. */
        errno = EOPNOTSUPP;
        return -1;
    }
    sem->handle = svcrt_sem_create("posixs", (int32)value);
    if(sem->handle < 0)
    {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int svcrt_posix_sem_destroy(svcrt_sem_t *sem)
{
    if((sem == 0) || (sem->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    (void)svcrt_sem_delete(sem->handle);
    sem->handle = -1;
    return 0;
}

int svcrt_posix_sem_wait(svcrt_sem_t *sem)
{
    if((sem == 0) || (sem->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_sem_wait(sem->handle, -1) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int svcrt_posix_sem_trywait(svcrt_sem_t *sem)
{
    if((sem == 0) || (sem->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_sem_wait(sem->handle, 0) != 0)
    {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int svcrt_posix_sem_timedwait(svcrt_sem_t *sem, uint32 timeout_ms)
{
    if((sem == 0) || (sem->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_sem_wait(sem->handle, (int32)timeout_ms) != 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

int svcrt_posix_sem_post(svcrt_sem_t *sem)
{
    if((sem == 0) || (sem->handle < 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_sem_post(sem->handle) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int svcrt_posix_sem_getvalue(svcrt_sem_t *sem, int *value)
{
    (void)sem;
    (void)value;
    /* The kernel semaphore service has no "current count" query, and guessing
     * one from this side would race with every other task. Report it as
     * unsupported instead of returning a number that might be wrong. */
    errno = ENOSYS;
    return -1;
}

/* ==========================================================================
 * 3c. message queue
 * ========================================================================== */

svcrt_mqd_t svcrt_posix_mq_open(const char *name, int oflag)
{
    (void)oflag;
    if(name == 0)
    {
        errno = EINVAL;
        return MQ_FAILED;
    }
    /* Queues are created, not looked up: there is no namespace to search, so
     * "open an existing queue by name" cannot be honoured. */
    return svcrt_mq_create((char *)name);
}

int svcrt_posix_mq_close(svcrt_mqd_t mqdes)
{
    if(mqdes < 0)
    {
        errno = EINVAL;
        return -1;
    }
    return (svcrt_mq_delete(mqdes) == 0) ? 0 : -1;
}

int svcrt_posix_mq_unlink(const char *name)
{
    (void)name;
    errno = ENOSYS;         /* no namespace, so nothing to unlink by name */
    return -1;
}

int svcrt_posix_mq_send(svcrt_mqd_t mqdes, const void *msg, uint32 len_words)
{
    if((mqdes < 0) || (msg == 0))
    {
        errno = EINVAL;
        return -1;
    }
    return (svcrt_mq_send(mqdes, (void *)msg, (int32)len_words, 0) == 0) ? 0 : -1;
}

int svcrt_posix_mq_receive(svcrt_mqd_t mqdes, void *msg, uint32 len_words,
                           uint32 timeout_ms)
{
    if((mqdes < 0) || (msg == 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(svcrt_mq_recv(mqdes, msg, (int32)len_words, (int32)timeout_ms) < 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

/* ==========================================================================
 * 3d. unistd / time
 * ========================================================================== */

/* ---- file descriptors ----------------------------------------------------
 *
 * A POSIX fd is an index into the small table below, starting at 3 so that
 * 0/1/2 keep their usual meaning. One table holds both kinds of descriptor
 * that open() can return: a path in the namespace (a kernel handle from
 * svcrt_path_open) and a device (a kernel handle from svcrt_dev_open).
 * Everything above the table goes straight to the device API - that is how a
 * handle obtained from svcrt_dev_open() itself keeps working, so an App that
 * mixes the two styles is not silently broken by the table.
 *
 * The table is required for one reason: the kernel's VFS handle is not an fd
 * the App should hand around, and close() has to know whether to go to the
 * path close or the device close.
 */
#define SVCRT_POSIX_FD_MAX   (8)
#define SVCRT_POSIX_FD_BASE  (3)

#define SVCRT_POSIX_FD_FREE  (0u)
#define SVCRT_POSIX_FD_DEV   (1u)
#define SVCRT_POSIX_FD_VFS   (2u)

/* A socket. It shares this table rather than getting a second descriptor
 * space: close(), read(), write(), fstat() and select() all have to see
 * sockets too, and mixing two numbering spaces is how a caller ends up
 * closing the wrong object. */
#define SVCRT_POSIX_FD_SOCK  (3u)

typedef struct
{
    uint8 used;
    uint8 kind;
    uint8 is_dir;
    uint32 mode;        /* S_IFMT type bits, captured at open; 0 = unknown */

/* Socket only, i.e. when kind == SVCRT_POSIX_FD_SOCK; unused for a path
 * or a device. nonblock is a promise this layer keeps itself (the
 * kernel's socket calls never block), and the two timeouts are
 * SO_RCVTIMEO / SO_SNDTIMEO, honoured by the blocking loops here. */
    uint32 nonblock;
    uint32 sotype;
    uint32 rcv_to;
    uint32 snd_to;
    int32 handle;
} svcrt_posix_fd_t;

static svcrt_posix_fd_t g_posix_fd[SVCRT_POSIX_FD_MAX];

/* Translate a kernel VFS error code into errno. The codes are ark_vfs's
 * (-1..-15, see svcrt_vfs.h) and an unknown one is reported as EIO rather
 * than guessed at. SVCRT_VFS_EINVAL is what the kernel returns for a bad
 * path or buffer, so it does not get confused with "no such file". */
static int svcrt_posix_errno_from_vfs(int32 rc)
{
    switch(rc)
    {
    case -1:  return ENOENT;
    case -2:  return EINVAL;
    case -3:  return EMFILE;
    case -4:  return EBADF;
    case -5:  return EISDIR;
    case -6:  return ENOTDIR;
    case -7:  return EEXIST;
    case -8:  return ENOTEMPTY;
    case -9:  return EROFS;
    case -10: return ENOSPC;
    case -11: return EIO;
    case -12: return ENOSYS;
    case -13: return ENAMETOOLONG;
    case -14: return ELOOP;
    case -15: return EBUSY;
    default:  return EIO;
    }
}

/* The entry an fd lives in, or NULL when the fd is not ours (then the caller
 * falls through to the device API - the pre-table behaviour). */
static svcrt_posix_fd_t *svcrt_posix_fd_slot(int fd)
{
    if((fd < SVCRT_POSIX_FD_BASE) ||
       (fd >= (SVCRT_POSIX_FD_BASE + SVCRT_POSIX_FD_MAX)))
    {
        return 0;
    }
    if(g_posix_fd[fd - SVCRT_POSIX_FD_BASE].used == 0u)
    {
        return 0;
    }
    return &g_posix_fd[fd - SVCRT_POSIX_FD_BASE];
}

static int svcrt_posix_fd_alloc(uint8 kind, int32 handle, uint8 is_dir,
                                uint32 mode)
{
    int i;

    for(i = 0; i < (int)SVCRT_POSIX_FD_MAX; i++)
    {
        if(g_posix_fd[i].used == 0u)
        {
            g_posix_fd[i].used   = 1u;
            g_posix_fd[i].kind   = kind;
            g_posix_fd[i].is_dir = is_dir;
            g_posix_fd[i].mode   = mode;
            g_posix_fd[i].handle = handle;
            g_posix_fd[i].nonblock = 0u;
            g_posix_fd[i].sotype   = 0u;
            g_posix_fd[i].rcv_to   = 0xFFFFFFFFu;   /* wait for ever */
            g_posix_fd[i].snd_to   = 0xFFFFFFFFu;
            return SVCRT_POSIX_FD_BASE + i;
        }
    }
    errno = EMFILE;     /* all fds in use - not "the file is missing" */
    return -1;
}

int svcrt_posix_open(const char *name, int flags, ...)
{
    int fd;

    if(name == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if(name[0] == '/')
    {
        /* A path in the namespace: the file system serves it. */
        uint32 size = 0u;
        uint32 mode = 0u;
        int32  h;

        /* Ask the type *before* opening, so fstat() can answer for a character
         * device - it has no seek, so its size cannot be found by seeking to
         * the end. Best effort: a path about to be created has no type yet,
         * and 0 then means "unknown", not "regular". */
        if(svcrt_path_stat(name, &size, &mode) < 0)
        {
            mode = 0u;
        }

        h = svcrt_path_open(name, (uint32)flags);
        if(h < 0)
        {
            errno = svcrt_posix_errno_from_vfs(h);
            return -1;
        }
        fd = svcrt_posix_fd_alloc(SVCRT_POSIX_FD_VFS, h,
                                  (uint8)(((flags & O_DIRECTORY) != 0) ? 1u : 0u),
                                  mode);
        if(fd < 0)
        {
            (void)svcrt_path_close(h);      /* do not leak the kernel handle */
            return -1;                      /* errno already set */
        }
        return fd;
    }

    /* A device name. The kernel device open takes no flags: a device is
     * whatever the driver registered. Asking for something the device side
     * cannot honour is refused here instead of being quietly dropped - a
     * caller that asked O_CREAT should not end up believing it got a file. */
    if((flags & O_ACCMODE) == O_RDWR)
    {
        errno = EOPNOTSUPP;
        return -1;
    }
    if((flags & O_DIRECTORY) != 0)
    {
        errno = ENOTDIR;
        return -1;
    }
    if((flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    {
        int32 h = svcrt_dev_open((char *)name, 0u);

        if(h < 0)
        {
            errno = ENODEV;
            return -1;
        }
        fd = svcrt_posix_fd_alloc(SVCRT_POSIX_FD_DEV, h, 0u, S_IFCHR);
        if(fd < 0)
        {
            (void)svcrt_dev_close(h);
            return -1;
        }
        return fd;
    }
}

/* Defined in the socket block at the end of this file. read() and write()
 * hand a socket descriptor to them, so an App keeps one set of verbs for
 * every kind of descriptor. */
static int   svcrt_posix_sock_close(svcrt_posix_fd_t *e);
svcrt_ssize_t svcrt_posix_recv(int fd, void *buf, uint32 len, int flags);
svcrt_ssize_t svcrt_posix_send(int fd, const void *buf, uint32 len, int flags);

int svcrt_posix_close(int fd)
{
    svcrt_posix_fd_t *e;
    int32 r;

    if(fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    e = svcrt_posix_fd_slot(fd);
    if(e != 0)
    {
        if(e->kind == SVCRT_POSIX_FD_SOCK)
        {
            /* The service closes the kernel side, and the descriptor is
             * released either way - see svcrt_posix_sock_close(). */
            return svcrt_posix_sock_close(e);
        }
        r = (e->kind == SVCRT_POSIX_FD_VFS) ? svcrt_path_close(e->handle)
                                            : svcrt_dev_close(e->handle);
        e->used = 0u;
        if(r < 0)
        {
            errno = svcrt_posix_errno_from_vfs(r);
            return -1;
        }
        return 0;
    }
    /* Not ours: a raw device handle from svcrt_dev_open(), as before. */
    if(svcrt_dev_close(fd) != 0)
    {
        errno = EBADF;
        return -1;
    }
    return 0;
}

svcrt_ssize_t svcrt_posix_read(int fd, void *buf, uint32 count)
{
    svcrt_posix_fd_t *e;
    int32 r;

    if((fd < 0) || (buf == 0))
    {
        errno = EBADF;
        return -1;
    }
    e = svcrt_posix_fd_slot(fd);
    if(e != 0)
    {
        if(e->kind == SVCRT_POSIX_FD_SOCK)
        {
            return svcrt_posix_recv(fd, buf, count, 0);
        }
        if(e->kind != SVCRT_POSIX_FD_VFS)
        {
            r = svcrt_dev_read(e->handle, buf, (int32)count);
        }
        else
        {
            r = svcrt_path_read(e->handle, buf, count);
        }
    }
    else
    {
        r = svcrt_dev_read(fd, buf, (int32)count);
    }
    if(r < 0)
    {
        errno = (e != 0) ? svcrt_posix_errno_from_vfs(r) : EIO;
        return -1;
    }
    return (svcrt_ssize_t)r;
}

svcrt_ssize_t svcrt_posix_write(int fd, const void *buf, uint32 count)
{
    svcrt_posix_fd_t *e;
    int32 r;

    if((fd < 0) || (buf == 0))
    {
        errno = EBADF;
        return -1;
    }
    e = svcrt_posix_fd_slot(fd);
    if(e != 0)
    {
        if(e->kind == SVCRT_POSIX_FD_SOCK)
        {
            return svcrt_posix_send(fd, buf, count, 0);
        }
        if(e->kind != SVCRT_POSIX_FD_VFS)
        {
            r = svcrt_dev_write(e->handle, (void *)buf, (int32)count);
        }
        else
        {
            r = svcrt_path_write(e->handle, buf, count);
        }
    }
    else
    {
        r = svcrt_dev_write(fd, (void *)buf, (int32)count);
    }
    if(r < 0)
    {
        errno = (e != 0) ? svcrt_posix_errno_from_vfs(r) : EIO;
        return -1;
    }
    return (svcrt_ssize_t)r;
}

svcrt_off_t svcrt_posix_lseek(int fd, svcrt_off_t off, int whence)
{
    svcrt_posix_fd_t *e;
    int32 r;

    if(fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    if((whence != SEEK_SET) && (whence != SEEK_CUR) && (whence != SEEK_END))
    {
        errno = EINVAL;
        return -1;
    }
    e = svcrt_posix_fd_slot(fd);
    if((e == 0) || (e->kind != SVCRT_POSIX_FD_VFS))
    {
        /* A device has no offset to move. */
        errno = ESPIPE;
        return -1;
    }
    r = svcrt_path_seek(e->handle, (int32)off, (uint32)whence);
    if(r < 0)
    {
        errno = svcrt_posix_errno_from_vfs(r);
        return -1;
    }
    return (svcrt_off_t)r;      /* the new absolute offset */
}

int svcrt_posix_unlink(const char *path)
{
    int32 r;

    if(path == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(path[0] != '/')
    {
        /* A device name has nothing to unlink. */
        errno = EINVAL;
        return -1;
    }
    r = svcrt_path_unlink(path);
    if(r < 0)
    {
        errno = svcrt_posix_errno_from_vfs(r);
        return -1;
    }
    return 0;
}

static int svcrt_posix_stat_fill(struct stat *st, uint32 mode, uint32 size)
{
    st->st_mode  = (svcrt_mode_t)mode;
    st->st_size  = (svcrt_off_t)size;
    st->st_mtime = svcrt_posix_time(0);
    return 0;
}

int svcrt_posix_stat(const char *path, struct stat *st)
{
    uint32 size = 0u;
    uint32 mode = 0u;
    int32  r;

    if((path == 0) || (st == 0))
    {
        errno = EINVAL;
        return -1;
    }
    if(path[0] != '/')
    {
        /* A device can be stat'ed through fstat once opened, but stat() by a
         * bare device name would have to guess a type - so it is refused. */
        errno = EINVAL;
        return -1;
    }
    r = svcrt_path_stat(path, &size, &mode);
    if(r < 0)
    {
        errno = svcrt_posix_errno_from_vfs(r);
        return -1;
    }
    return svcrt_posix_stat_fill(st, mode, size);
}

int svcrt_posix_fstat(int fd, struct stat *st)
{
    svcrt_posix_fd_t *e;

    if((fd < 0) || (st == 0))
    {
        errno = EBADF;
        return -1;
    }
    if((e != 0) && (e->kind == SVCRT_POSIX_FD_SOCK))
    {
        /* No size and no offset, and neither the file service nor the
         * device API is the right place to ask about a socket. */
        return svcrt_posix_stat_fill(st, S_IFSOCK, 0u);
    }
    e = svcrt_posix_fd_slot(fd);
    if((e == 0) || (e->kind == SVCRT_POSIX_FD_DEV))
    {
        /* A device: character type, no size. */
        return svcrt_posix_stat_fill(st, S_IFCHR, 0u);
    }
    /* A path opened by name can itself be a device: the namespace mirrors the
     * device registry under /dev. The type comes from what open() saw, so a
     * character device never reaches the seek-based size below - it cannot be
     * seeked, and a failed fstat is a worse answer than a typed one. */
    if(S_ISDIR(e->mode))
    {
        return svcrt_posix_stat_fill(st, S_IFDIR, 0u);
    }
    if(S_ISCHR(e->mode))
    {
        return svcrt_posix_stat_fill(st, S_IFCHR, 0u);
    }
    /* The size of an open file: the kernel tracks the offset, so SEEK_END is
     * the size of the file. The offset is put back afterwards - and if that
     * fails the call reports an error rather than leaving the position
     * somewhere the caller did not ask for. There is no path cached here on
     * purpose: a path could go stale, an offset cannot. */
    {
        int32 cur = svcrt_path_seek(e->handle, 0, SVCRT_PATH_SEEK_CUR);
        int32 end;

        if(cur < 0)
        {
            errno = svcrt_posix_errno_from_vfs(cur);
            return -1;
        }
        end = svcrt_path_seek(e->handle, 0, SVCRT_PATH_SEEK_END);
        if(end < 0)
        {
            errno = svcrt_posix_errno_from_vfs(end);
            return -1;
        }
        if(svcrt_path_seek(e->handle, cur, SVCRT_PATH_SEEK_SET) < 0)
        {
            errno = EIO;
            return -1;
        }
        return svcrt_posix_stat_fill(st, S_IFREG, (uint32)end);
    }
}

/* ---- directory streams -------------------------------------------------- */

struct svcrt_posix_dir
{
    uint8 used;
    int   fd;           /* the POSIX fd, so closedir() closes it the one way */
    int32 handle;       /* the kernel handle, for readdir                    */
    struct dirent ent;
};

static struct svcrt_posix_dir g_posix_dir[SVCRT_POSIX_DIR_MAX];

DIR *svcrt_posix_opendir(const char *path)
{
    int i;
    int fd;
    svcrt_posix_fd_t *e;

    if(path == 0)
    {
        errno = EINVAL;
        return 0;
    }
    for(i = 0; i < (int)SVCRT_POSIX_DIR_MAX; i++)
    {
        if(g_posix_dir[i].used == 0u)
        {
            break;
        }
    }
    if(i >= (int)SVCRT_POSIX_DIR_MAX)
    {
        errno = ENOMEM;     /* streams are a fixed pool, not a malloc */
        return 0;
    }
    fd = svcrt_posix_open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0)
    {
        return 0;
    }
    e = svcrt_posix_fd_slot(fd);
    if((e == 0) || (e->is_dir == 0u))
    {
        (void)svcrt_posix_close(fd);
        errno = ENOTDIR;
        return 0;
    }
    g_posix_dir[i].used   = 1u;
    g_posix_dir[i].fd     = fd;
    g_posix_dir[i].handle = e->handle;
    return &g_posix_dir[i];
}

struct dirent *svcrt_posix_readdir(DIR *d)
{
    uint32 mode = 0u;
    uint32 size = 0u;
    int32  r;

    if((d == 0) || (d->used == 0u))
    {
        errno = EBADF;
        return 0;
    }
    r = svcrt_path_readdir(d->handle, d->ent.d_name, SVCRT_POSIX_NAME_MAX,
                           &mode, &size);
    if(r == 1)
    {
        errno = 0;      /* end of the directory, and that is not an error */
        return 0;
    }
    if(r < 0)
    {
        errno = svcrt_posix_errno_from_vfs(r);
        return 0;
    }
    d->ent.d_type = (uint32)IFTODT(mode);
    d->ent.d_size = (svcrt_off_t)size;
    return &d->ent;
}

int svcrt_posix_closedir(DIR *d)
{
    int r;

    if((d == 0) || (d->used == 0u))
    {
        errno = EBADF;
        return -1;
    }
    r = svcrt_posix_close(d->fd);
    d->used = 0u;
    return r;
}

uint32 svcrt_posix_sleep(uint32 seconds)
{
    if(seconds == 0u)
    {
        svcrt_posix_sched_yield();
        return 0u;
    }
    svcrt_task_wait(seconds * 1000u);
    return 0u;
}

int svcrt_posix_usleep(svcrt_useconds_t usec)
{
    uint32 ms;

    if(usec < 0)
    {
        errno = EINVAL;
        return -1;
    }
    /* The kernel waits in milliseconds. Rounding up guarantees the sleep is at
     * least as long as asked for; rounding down would return early, which is
     * the one thing a sleep must never do. */
    ms = ((uint32)usec + 999u) / 1000u;
    svcrt_task_wait(ms);
    return 0;
}

int svcrt_posix_nanosleep(const struct timespec *req, struct timespec *rem)
{
    uint32 ms;

    if(req == 0)
    {
        errno = EINVAL;
        return -1;
    }
    /* tv_sec is unsigned here, so it can never be negative; what it can do is
     * overflow once multiplied by 1000, which is what gets checked instead.
     * tv_nsec is signed and has to sit in [0, 999999999]. */
    if((req->tv_nsec < 0) || (req->tv_nsec > 999999999))
    {
        errno = EINVAL;
        return -1;
    }
    if(req->tv_sec > (0xFFFFFFFFu / 1000u))
    {
        errno = EINVAL;
        return -1;
    }
    ms = ((uint32)req->tv_sec * 1000u) +
         (((uint32)req->tv_nsec + 999999u) / 1000000u);
    if(rem != 0)
    {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }
    svcrt_task_wait(ms);
    return 0;
}

svcrt_time_t svcrt_posix_time(svcrt_time_t *tloc)
{
    svcrt_time_t now = (svcrt_time_t)(svcrt_get_time_ms() / 1000u);

    if(tloc != 0)
    {
        *tloc = now;
    }
    return now;
}

int svcrt_posix_clock_gettime(svcrt_clockid_t clk, struct timespec *ts)
{
    uint32 ms;

    if(ts == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if((clk != SVCRT_CLOCK_REALTIME) && (clk != SVCRT_CLOCK_MONOTONIC))
    {
        errno = EINVAL;
        return -1;
    }
    ms = svcrt_get_time_ms();
    ts->tv_sec  = (svcrt_time_t)(ms / 1000u);
    ts->tv_nsec = (int32)((ms % 1000u) * 1000000u);
    return 0;
}

int svcrt_posix_sched_yield(void)
{
    /* A 0 ms wait puts the caller back on the ready list and lets the
     * scheduler pick the next task, which is exactly yield's contract here. */
    svcrt_task_wait(0u);
    return 0;
}

/* ==========================================================================
 * 4. Windows style names (see svcrt_win_compat.h)
 * ========================================================================== */

#define SVCRT_WAIT_OBJECT_0  (0u)
#define SVCRT_WAIT_TIMEOUT   (0x102u)

svcrt_HANDLE svcrt_win_CreateThread(void *security, uint32 stack_size,
                                    svcrt_DWORD (*start)(svcrt_LPVOID),
                                    svcrt_LPVOID arg, uint32 flags,
                                    uint32 *thread_id)
{
    svcrt_pthread_attr_t attr;
    svcrt_pthread_t thread = SVCRT_PTHREAD_NULL;

    (void)security;
    (void)flags;

    if(start == 0)
    {
        errno = EINVAL;
        return 0;
    }
    /* Win32 hands over a stack SIZE and lets the OS find the memory. Here the
     * caller has to own the stack, so a request larger than the built in pool
     * is refused rather than quietly satisfied with less than was asked. */
    if((stack_size != 0u) && (stack_size > ((uint32)SVCRT_POSIX_STACK_WORDS * 4u)))
    {
        errno = EOPNOTSUPP;
        return 0;
    }

    (void)svcrt_posix_pthread_attr_init(&attr);
    /* A Win32 thread routine and a POSIX one pass their argument and result in
     * r0 under AAPCS (WINAPI is empty on ARM), so the pointer converts and
     * calls directly. */
    if(svcrt_posix_pthread_create(&thread, &attr,
                                  (void *(*)(void *))(void *)start,
                                  (void *)arg) != 0)
    {
        return 0;
    }

    if(thread_id != 0)
    {
        *thread_id = (uint32)thread;
    }
    return (svcrt_HANDLE)(uint32)thread;
}

svcrt_DWORD svcrt_win_WaitForSingleObject(svcrt_HANDLE h, uint32 ms)
{
    uint32 id = (uint32)h;

    if(id == 0u)
    {
        errno = EINVAL;
        return (svcrt_DWORD)(-1);
    }
    if(ms != (uint32)INFINITE)
    {
        /* Only an unbounded wait maps onto pthread_join. A bounded one would
         * have to give up without reaping the thread, which a caller reading
         * WAIT_TIMEOUT cannot tell apart from "still running, handle intact". */
        return (svcrt_DWORD)SVCRT_WAIT_TIMEOUT;
    }
    if(svcrt_posix_pthread_join((svcrt_pthread_t)id, 0) != 0)
    {
        return (svcrt_DWORD)(-1);
    }
    return (svcrt_DWORD)SVCRT_WAIT_OBJECT_0;
}

svcrt_BOOL svcrt_win_CloseHandle(svcrt_HANDLE h)
{
    (void)h;
    /* A thread handle is a thread slot, and the slot is what pthread_join
     * releases. Closing without waiting would leave the thread holding its
     * slot, so say so instead of pretending the handle is gone. */
    errno = EOPNOTSUPP;
    return FALSE;
}

svcrt_BOOL svcrt_win_InitializeCriticalSection(svcrt_CRITICAL_SECTION *cs)
{
    if(cs == 0)
    {
        errno = EINVAL;
        return FALSE;
    }
    return (svcrt_posix_mutex_init(&cs->mtx) == 0) ? TRUE : FALSE;
}

void svcrt_win_EnterCriticalSection(svcrt_CRITICAL_SECTION *cs)
{
    if(cs != 0)
    {
        (void)svcrt_posix_mutex_lock(&cs->mtx);
    }
}

void svcrt_win_LeaveCriticalSection(svcrt_CRITICAL_SECTION *cs)
{
    if(cs != 0)
    {
        (void)svcrt_posix_mutex_unlock(&cs->mtx);
    }
}

void svcrt_win_DeleteCriticalSection(svcrt_CRITICAL_SECTION *cs)
{
    if(cs != 0)
    {
        (void)svcrt_posix_mutex_destroy(&cs->mtx);
    }
}

int32 svcrt_win_strcpy_s(char *dst, uint32 dst_size, const char *src)
{
    uint32 i;

    if((dst == 0) || (dst_size == 0u))
    {
        errno = EINVAL;
        return -1;
    }
    if(src == 0)
    {
        dst[0] = '\0';
        errno = EINVAL;
        return -1;
    }
    for(i = 0u; i < (dst_size - 1u); i++)
    {
        dst[i] = src[i];
        if(src[i] == '\0')
        {
            return 0;
        }
    }
    /* No room for the terminator: leave a valid empty string behind so the
     * caller cannot accidentally read past the buffer. */
    dst[0] = '\0';
    errno = ERANGE;
    return -1;
}

int32 svcrt_win_strcat_s(char *dst, uint32 dst_size, const char *src)
{
    uint32 len;
    uint32 i;

    if((dst == 0) || (dst_size == 0u) || (src == 0))
    {
        errno = EINVAL;
        return -1;
    }
    len = svcrt_posix_strlen(dst);
    if(len >= dst_size)
    {
        dst[0] = '\0';
        errno = ERANGE;
        return -1;
    }
    for(i = 0u; (len + i) < (dst_size - 1u); i++)
    {
        dst[len + i] = src[i];
        if(src[i] == '\0')
        {
            return 0;
        }
    }
    dst[0] = '\0';
    errno = ERANGE;
    return -1;
}

int svcrt_win_snprintf_s(char *dst, uint32 dst_size, const char *fmt, ...)
{
    va_list ap;
    int written;

    if((dst == 0) || (dst_size == 0u) || (fmt == 0))
    {
        errno = EINVAL;
        return -1;
    }
    va_start(ap, fmt);
    written = svcrt_ufmt_v(dst, dst_size, fmt, ap);
    va_end(ap);

    if((written < 0) || ((uint32)written >= dst_size))
    {
        /* Truncation is a failure in the Windows contract, so report it as
         * one instead of handing back a silently shortened string. */
        dst[0] = '\0';
        errno = ERANGE;
        return -1;
    }
    return written;
}

/* ==========================================================================
 * 4. sockets - the POSIX face over SVC 0x1E
 *
 * The kernel's socket service never waits: an SVC handler cannot yield, so a
 * request is handed to a service task and answered EPENDING until it is done,
 * and re-issuing the very same call is how the caller polls (the contract is
 * in svcrt_net_abi.h). Blocking semantics therefore live here, in the App,
 * and are built from two primitives:
 *
 *   1. one request, waited for (svcrt_posix_net_req) - a scheduling wait, not
 *      a network wait, because the service task is an ordinary task;
 *   2. a readiness probe (SVCRT_NET_SUB_POLL), which is how a caller waits
 *      for a socket to become readable or writable without holding the
 *      request slot for ever.
 *
 * Retries, O_NONBLOCK, SO_RCVTIMEO and SO_SNDTIMEO are all composed from
 * those two, which is why the kernel never has to know about any of them.
 *
 * Ownership: the kernel pins a socket to the task that created it, so an App
 * must use the descriptor from the thread that opened it. Sharing a socket
 * between threads is a design decision with a lock in it, not something this
 * layer can guess at.
 * ========================================================================== */

SVCRT_SVC_DECL_1(int32, 0x1E, svcrt_call_net, uint32 *);

/* Budget for a call that waits on the service task rather than on the
 * network. The reply is a scheduling delay; a whole second is already far
 * past anything healthy, and an expiry is reported instead of sat on. */
#define SVCRT_POSIX_NET_CMD_BUDGET_MS  (1000u)
#define SVCRT_POSIX_NET_FOREVER        (0xFFFFFFFFu)

/* ---------------------------------------------------------------- errors */

/* Kernel wire error -> errno. An unknown code is EIO: picking the closest
 * sounding errno would invent a cause the kernel never reported. */
static int svcrt_posix_errno_from_net(int32 rc)
{
    switch(rc)
    {
    case SVCRT_NET_EPENDING:     return ETIMEDOUT;  /* the service never
                                                       answered in budget */
    case SVCRT_NET_EBUSY:        return EBUSY;
    case SVCRT_NET_ENOTSUP:      return EOPNOTSUPP;
    case SVCRT_NET_EINVAL:       return EINVAL;
    case SVCRT_NET_EWOULDBLOCK:  return EAGAIN;
    case SVCRT_NET_EINPROGRESS:  return EINPROGRESS;
    case SVCRT_NET_EALREADY:     return EALREADY;
    case SVCRT_NET_EISCONN:      return EISCONN;
    case SVCRT_NET_ENOTCONN:     return ENOTCONN;
    case SVCRT_NET_ECONNREFUSED: return ECONNREFUSED;
    case SVCRT_NET_ECONNRESET:   return ECONNRESET;
    case SVCRT_NET_ECONNABORTED: return ECONNABORTED;
    case SVCRT_NET_EHOSTUNREACH: return EHOSTUNREACH;
    case SVCRT_NET_EMSGSIZE:     return EMSGSIZE;
    case SVCRT_NET_ENOBUFS:      return ENOBUFS;
    default:                     return EIO;
    }
}

/* -------------------------------------------------------------- requests */

/* One request, from issue to reply. The argument block must stay identical
 * between issues: the kernel recognises its own pending request by the whole
 * block, and a different block is a different call - which is exactly how a
 * caller that gave up on a request stops being handed that request's reply. */
static int32 svcrt_posix_net_req(const uint32 *p, uint32 budget_ms)
{
    uint32 t0 = svcrt_get_time_ms();
    int32  r;

    for(;;)
    {
        r = svcrt_call_net((uint32 *)p);
        if((r != SVCRT_NET_EPENDING) && (r != SVCRT_NET_EBUSY))
        {
            return r;
        }
        if((budget_ms != SVCRT_POSIX_NET_FOREVER) &&
           ((uint32)(svcrt_get_time_ms() - t0) >= budget_ms))
        {
            /* EPENDING: still ours, just unfinished. EBUSY: another task's
             * request owns the slot. Either way this is not an answer, and
             * the caller must not treat it as one. */
            return r;
        }
        svcrt_task_wait(1u);
    }
}

/* A control call: create, bind, listen, close, options, shutdown, name. None
 * of them has a notion of blocking, so a request the service never answered
 * is an error, and the reply - a handle or 0 - comes back through *out. */
static int svcrt_posix_net_ctl(uint32 *p, int32 *out)
{
    int32 r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);

    if(r < 0)
    {
        errno = svcrt_posix_errno_from_net(r);
        return -1;
    }
    *out = r;
    return 0;
}

/* Readiness of one socket as a SVCRT_NET_POLL_* mask, or -1 with errno set.
 * Zero ready bits is a valid answer, not an error. */
static int32 svcrt_posix_net_bits(uint32 handle, uint32 flags)
{
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    p[0] = SVCRT_NET_SUB_POLL;
    p[1] = handle;
    p[2] = flags;
    r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
    if(r < 0)
    {
        errno = svcrt_posix_errno_from_net(r);
        return -1;
    }
    return r;
}

/* Wait until a socket is ready in any of `flags`. Returns the ready bits, 0
 * when budget_ms ran out, or -1 with errno set. */
static int32 svcrt_posix_sock_wait(uint32 handle, uint32 flags, uint32 budget_ms)
{
    uint32 t0 = svcrt_get_time_ms();
    int32  r;

    for(;;)
    {
        r = svcrt_posix_net_bits(handle, flags);
        if(r < 0)
        {
            return -1;
        }
        if(r != 0)
        {
            return r;
        }
        if((budget_ms != SVCRT_POSIX_NET_FOREVER) &&
           ((uint32)(svcrt_get_time_ms() - t0) >= budget_ms))
        {
            return 0;
        }
        svcrt_task_wait(1u);
    }
}

/* ------------------------------------------------------- descriptor glue */

static svcrt_posix_fd_t *svcrt_posix_sock_slot(int fd)
{
    svcrt_posix_fd_t *e = svcrt_posix_fd_slot(fd);

    if((e == 0) || (e->kind != SVCRT_POSIX_FD_SOCK))
    {
        errno = ENOTSOCK;
        return 0;
    }
    return e;
}

/* Hand a kernel socket handle straight back, leaving errno alone: every
 * caller is on a failure path and is about to report the errno it already
 * has, so cleanup must not overwrite it. */
static void svcrt_posix_sock_return(int32 handle)
{
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int    saved = errno;

    p[0] = SVCRT_NET_SUB_CLOSE;
    p[1] = (uint32)handle;
    (void)svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
    errno = saved;
}

/* The App descriptor of a socket is released before the kernel is asked:
 * a refusal from the service must not leave a descriptor that can never be
 * used again. Returns the kernel's verdict so the caller can report it. */
static int svcrt_posix_sock_close(svcrt_posix_fd_t *e)
{
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  dummy;

    p[0] = SVCRT_NET_SUB_CLOSE;
    p[1] = (uint32)e->handle;
    e->used   = 0u;
    e->kind   = SVCRT_POSIX_FD_FREE;
    e->handle = -1;
    return svcrt_posix_net_ctl(p, &dummy);
}

/* ------------------------------------------------------------ addresses */

/* struct sockaddr -> the two scalars the kernel takes. A short address or a
 * foreign family is refused rather than zero filled: a silently zeroed
 * address is a bind to every interface, which is not what the caller said. */
static int svcrt_posix_addr_in(const struct sockaddr *addr, socklen_t len,
                               uint32 *ip, uint32 *port)
{
    const struct sockaddr_in *sin;

    if(addr == 0)
    {
        errno = EFAULT;
        return -1;
    }
    if(len < (socklen_t)sizeof(struct sockaddr_in))
    {
        errno = EINVAL;
        return -1;
    }
    if(addr->sa_family != (sa_family_t)AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    sin   = (const struct sockaddr_in *)addr;
    *ip   = svcrt_posix_ntohl(sin->sin_addr.s_addr);
    *port = (uint32)svcrt_posix_ntohs(sin->sin_port);
    return 0;
}

/* The two scalars back into the caller's struct sockaddr. A NULL address, or
 * a NULL length, is legal POSIX and means "not interested". When the buffer
 * is smaller than the address, the bytes that fit are written and the full
 * length is reported - what POSIX describes, and what a caller can tell
 * apart from "no address". */
static void svcrt_posix_addr_out(struct sockaddr *addr, socklen_t *len,
                                 uint32 ip, uint32 port)
{
    struct sockaddr_in sin;
    uint32 n;

    if((addr == 0) || (len == 0))
    {
        return;
    }
    (void)svcrt_posix_memset(&sin, 0, (uint32)sizeof(sin));
    sin.sin_family      = (sa_family_t)AF_INET;
    sin.sin_port        = svcrt_posix_htons((uint16)port);
    sin.sin_addr.s_addr = svcrt_posix_htonl(ip);

    n = (uint32)(*len);
    if(n > (uint32)sizeof(sin))
    {
        n = (uint32)sizeof(sin);
    }
    if(n != 0u)
    {
        (void)svcrt_posix_memcpy(addr, &sin, n);
    }
    *len = (socklen_t)sizeof(sin);
}

/* ------------------------------------------------------- byte order, IPv4
 * SVCrtOS is 32-bit and Cortex-M is little endian, so the network byte order
 * conversions are exactly these byte swaps. They are plain arithmetic: no
 * kernel call, no lwIP, nothing to be conditional about. */
uint16 svcrt_posix_htons(uint16 x)
{
    return (uint16)(((x & 0x00FFu) << 8) | ((x & 0xFF00u) >> 8));
}

uint16 svcrt_posix_ntohs(uint16 x)
{
    return svcrt_posix_htons(x);
}

uint32 svcrt_posix_htonl(uint32 x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}

uint32 svcrt_posix_ntohl(uint32 x)
{
    return svcrt_posix_htonl(x);
}

/* Strict dotted quad: exactly four decimal octets, nothing else. Returns 0
 * and fills four bytes in network order, or -1. */
static int svcrt_posix_parse_ipv4(const char *src, uint8 *out4)
{
    uint32 acc  = 0u;
    int    seen = 0;
    int    part = 0;
    int    i;

    if(src == 0)
    {
        return -1;
    }
    for(i = 0; ; i++)
    {
        char c = src[i];

        if((c >= '0') && (c <= '9'))
        {
            acc = acc * 10u + (uint32)(c - '0');
            if(acc > 255u)
            {
                return -1;
            }
            seen = 1;
        }
        else if((c == '.') || (c == '\0'))
        {
            if(seen == 0)
            {
                return -1;              /* "1..2", "", ".1" */
            }
            out4[part] = (uint8)acc;
            part++;
            acc  = 0u;
            seen = 0;
            if(c == '\0')
            {
                break;
            }
            if(part >= 4)
            {
                return -1;              /* five parts */
            }
        }
        else
        {
            return -1;
        }
    }
    return (part == 4) ? 0 : -1;
}

in_addr_t svcrt_posix_inet_addr(const char *cp)
{
    uint8 b[4];

    if(svcrt_posix_parse_ipv4(cp, b) != 0)
    {
        return (in_addr_t)INADDR_NONE;
    }
    return (in_addr_t)svcrt_posix_htonl(((uint32)b[0] << 24) |
                                        ((uint32)b[1] << 16) |
                                        ((uint32)b[2] << 8)  |
                                        (uint32)b[3]);
}

static char *svcrt_posix_put_octet(char *p, uint32 v)
{
    if(v >= 100u)
    {
        *p++ = (char)('0' + (v / 100u));
        v %= 100u;
        *p++ = (char)('0' + (v / 10u));
        v %= 10u;
    }
    else if(v >= 10u)
    {
        *p++ = (char)('0' + (v / 10u));
        v %= 10u;
    }
    *p++ = (char)('0' + v);
    return p;
}

char *svcrt_posix_inet_ntoa(struct in_addr in)
{
    static char buf[16];
    char  *p = buf;
    uint32 v = svcrt_posix_ntohl(in.s_addr);

    p = svcrt_posix_put_octet(p, (v >> 24) & 0xFFu);
    *p++ = '.';
    p = svcrt_posix_put_octet(p, (v >> 16) & 0xFFu);
    *p++ = '.';
    p = svcrt_posix_put_octet(p, (v >> 8) & 0xFFu);
    *p++ = '.';
    p = svcrt_posix_put_octet(p, v & 0xFFu);
    *p = '\0';
    return buf;
}

int svcrt_posix_inet_pton(int af, const char *src, void *dst)
{
    uint8 b[4];

    if(af != AF_INET)
    {
        errno = ENOSYS;         /* no such family here, and not a bad address */
        return -1;
    }
    if((src == 0) || (dst == 0))
    {
        errno = EFAULT;
        return -1;
    }
    if(svcrt_posix_parse_ipv4(src, b) != 0)
    {
        return 0;               /* POSIX: 0 means "not a valid address" */
    }
    (void)svcrt_posix_memcpy(dst, b, 4u);
    return 1;
}

const char *svcrt_posix_inet_ntop(int af, const void *src, char *dst,
                                  socklen_t dst_size)
{
    struct in_addr in;
    uint32 len;

    if(af != AF_INET)
    {
        errno = ENOSYS;
        return 0;
    }
    if((src == 0) || (dst == 0))
    {
        errno = EFAULT;
        return 0;
    }
    if(dst_size < 16u)
    {
        errno = ENOSPC;         /* the longest AF_INET text is 15 + NUL */
        return 0;
    }
    (void)svcrt_posix_memcpy(&in, src, 4u);
    len = svcrt_posix_strlen(svcrt_posix_inet_ntoa(in));
    (void)svcrt_posix_memcpy(dst, svcrt_posix_inet_ntoa(in), len + 1u);
    return dst;
}

/* -------------------------------------------------------------- facades */

int svcrt_posix_socket(int domain, int type, int protocol)
{
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    svcrt_posix_fd_t *e;
    int32  h;
    int    fd;

    if(domain != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if((type != SOCK_STREAM) && (type != SOCK_DGRAM))
    {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }
    if((protocol != 0) && (protocol != IPPROTO_TCP) && (protocol != IPPROTO_UDP))
    {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    p[0] = SVCRT_NET_SUB_SOCKET;
    p[1] = (uint32)type;
    if(svcrt_posix_net_ctl(p, &h) < 0)
    {
        return -1;      /* EOPNOTSUPP when the kernel was built without lwIP */
    }

    fd = svcrt_posix_fd_alloc(SVCRT_POSIX_FD_SOCK, h, 0u, 0u);
    if(fd < 0)
    {
        /* Out of App descriptors: the kernel handle must not outlive the
         * failure, and errno stays EMFILE from fd_alloc. */
        svcrt_posix_sock_return(h);
        return -1;
    }
    e = svcrt_posix_fd_slot(fd);
    if(e != 0)
    {
        e->sotype = (uint32)type;
    }
    return fd;
}

int svcrt_posix_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(svcrt_posix_addr_in(addr, len, &ip, &port) < 0)
    {
        return -1;
    }
    p[0] = SVCRT_NET_SUB_BIND;
    p[1] = (uint32)e->handle;
    p[2] = ip;
    p[3] = port;
    return svcrt_posix_net_ctl(p, &r);
}

int svcrt_posix_listen(int fd, int backlog)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if((backlog < 0) || (backlog > 255))
    {
        /* The kernel takes a byte and would refuse anything larger; saying so
         * here keeps the caller's errno about its argument, not about us. */
        errno = EINVAL;
        return -1;
    }
    p[0] = SVCRT_NET_SUB_LISTEN;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)backlog;
    return svcrt_posix_net_ctl(p, &r);
}

int svcrt_posix_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    svcrt_posix_fd_t *n;
    int32  h  = 0;
    int32  r  = 0;
    int    nfd;

    if(e == 0)
    {
        return -1;
    }

    p[0] = SVCRT_NET_SUB_ACCEPT;
    p[1] = (uint32)e->handle;

    for(;;)
    {
        r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
        if(r > 0)
        {
            h = r;
            break;
        }
        if(r != SVCRT_NET_EWOULDBLOCK)
        {
            errno = svcrt_posix_errno_from_net(r);
            return -1;
        }
        if(e->nonblock != 0u)
        {
            errno = EAGAIN;
            return -1;
        }
        r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_IN,
                                  e->rcv_to);
        if(r < 0)
        {
            return -1;
        }
        if(r == 0)
        {
            errno = EAGAIN;         /* the receive timeout ran out */
            return -1;
        }
    }

    nfd = svcrt_posix_fd_alloc(SVCRT_POSIX_FD_SOCK, h, 0u, 0u);
    if(nfd < 0)
    {
        svcrt_posix_sock_return(h);
        return -1;
    }
    n = svcrt_posix_fd_slot(nfd);
    if(n != 0)
    {
        /* lwIP's accepted socket is of the same type as the listener, and it
         * starts blocking on this side too - O_NONBLOCK is per descriptor. */
        n->sotype = e->sotype;
    }

    if((addr != 0) && (len != 0))
    {
        uint32 q[SVCRT_NET_ARG_WORDS] = {0u};
        uint32 ip = 0u;
        uint32 port = 0u;

        /* ACCEPT's reply carries no address, so ask for it separately. If that
         * fails the descriptor is still good: report "no address" by a zero
         * length instead of leaving whatever happened to be in the buffer. */
        q[0] = SVCRT_NET_SUB_GETPEERNAME;
        q[1] = (uint32)h;
        q[2] = (uint32)&ip;
        q[3] = (uint32)&port;
        if(svcrt_posix_net_ctl(q, &r) < 0)
        {
            *len = 0;
        }
        else
        {
            svcrt_posix_addr_out(addr, len, ip, port);
        }
    }
    return nfd;
}

int svcrt_posix_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(svcrt_posix_addr_in(addr, len, &ip, &port) < 0)
    {
        return -1;
    }

    p[0] = SVCRT_NET_SUB_CONNECT;
    p[1] = (uint32)e->handle;
    p[2] = ip;
    p[3] = port;
    r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
    if(r == 0)
    {
        return 0;
    }
    if(r != SVCRT_NET_EINPROGRESS)
    {
        errno = svcrt_posix_errno_from_net(r);
        return -1;
    }

    /* Still connecting. A non-blocking caller is done here: POSIX puts the
     * rest on select() / SO_ERROR, and reporting success now would be a lie. */
    if(e->nonblock != 0u)
    {
        errno = EINPROGRESS;
        return -1;
    }

    r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_OUT, e->snd_to);
    if(r < 0)
    {
        return -1;
    }
    if(r == 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }

    /* Writable means the attempt finished - not that it worked. The outcome
     * only exists in SO_ERROR, so read it rather than assume. */
    {
        uint32 q[SVCRT_NET_ARG_WORDS] = {0u};
        uint32 so = 0u;

        q[0] = SVCRT_NET_SUB_GETSOCKOPT;
        q[1] = (uint32)e->handle;
        q[2] = SVCRT_NET_SO_ERROR;
        q[3] = (uint32)&so;
        if(svcrt_posix_net_ctl(q, &r) < 0)
        {
            return -1;
        }
        if((int32)so < 0)
        {
            errno = svcrt_posix_errno_from_net((int32)so);
            return -1;
        }
    }
    return 0;
}

svcrt_ssize_t svcrt_posix_send(int fd, const void *buf, uint32 len, int flags)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(len == 0u)
    {
        return 0;               /* POSIX: nothing to send, nothing to report */
    }

    p[0] = SVCRT_NET_SUB_SEND;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)buf;
    p[3] = len;
    p[4] = (uint32)flags;

    for(;;)
    {
        r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
        if(r >= 0)
        {
            return (svcrt_ssize_t)r;    /* a short count is normal for a socket */
        }
        if(r != SVCRT_NET_EWOULDBLOCK)
        {
            errno = svcrt_posix_errno_from_net(r);
            return -1;
        }
        if((e->nonblock != 0u) || ((flags & MSG_DONTWAIT) != 0))
        {
            errno = EAGAIN;
            return -1;
        }
        r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_OUT,
                                  e->snd_to);
        if(r < 0)
        {
            return -1;
        }
        if(r == 0)
        {
            errno = EAGAIN;         /* the send timeout ran out */
            return -1;
        }
    }
}

svcrt_ssize_t svcrt_posix_recv(int fd, void *buf, uint32 len, int flags)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(len == 0u)
    {
        return 0;
    }

    p[0] = SVCRT_NET_SUB_RECV;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)buf;
    p[3] = len;
    p[4] = (uint32)flags;

    for(;;)
    {
        r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
        if(r >= 0)
        {
            return (svcrt_ssize_t)r;
        }
        if(r != SVCRT_NET_EWOULDBLOCK)
        {
            errno = svcrt_posix_errno_from_net(r);
            return -1;
        }
        if((e->nonblock != 0u) || ((flags & MSG_DONTWAIT) != 0))
        {
            errno = EAGAIN;
            return -1;
        }
        r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_IN,
                                  e->rcv_to);
        if(r < 0)
        {
            return -1;
        }
        if(r == 0)
        {
            errno = EAGAIN;
            return -1;
        }
    }
}

svcrt_ssize_t svcrt_posix_sendto(int fd, const void *buf, uint32 len, int flags,
                                 const struct sockaddr *addr, socklen_t addrlen)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(len == 0u)
    {
        return 0;
    }
    if(svcrt_posix_addr_in(addr, addrlen, &ip, &port) < 0)
    {
        return -1;
    }

    p[0] = SVCRT_NET_SUB_SENDTO;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)buf;
    p[3] = len;
    p[4] = ip;
    p[5] = port;

    /* Datagrams are all-or-nothing: there is no half of a message to retry,
     * so a not-yet-writable socket is reported rather than waited for unless
     * the caller asked for a blocking send. */
    for(;;)
    {
        r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
        if(r >= 0)
        {
            return (svcrt_ssize_t)r;
        }
        if(r != SVCRT_NET_EWOULDBLOCK)
        {
            errno = svcrt_posix_errno_from_net(r);
            return -1;
        }
        if((e->nonblock != 0u) || ((flags & MSG_DONTWAIT) != 0))
        {
            errno = EAGAIN;
            return -1;
        }
        r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_OUT,
                                  e->snd_to);
        if(r < 0)
        {
            return -1;
        }
        if(r == 0)
        {
            errno = EAGAIN;
            return -1;
        }
    }
}

svcrt_ssize_t svcrt_posix_recvfrom(int fd, void *buf, uint32 len, int flags,
                                   struct sockaddr *addr, socklen_t *addrlen)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(len == 0u)
    {
        return 0;
    }

    p[0] = SVCRT_NET_SUB_RECVFROM;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)buf;
    p[3] = len;
    p[4] = (uint32)&ip;
    p[5] = (uint32)&port;

    for(;;)
    {
        r = svcrt_posix_net_req(p, SVCRT_POSIX_NET_CMD_BUDGET_MS);
        if(r >= 0)
        {
            break;
        }
        if(r != SVCRT_NET_EWOULDBLOCK)
        {
            errno = svcrt_posix_errno_from_net(r);
            return -1;
        }
        if((e->nonblock != 0u) || ((flags & MSG_DONTWAIT) != 0))
        {
            errno = EAGAIN;
            return -1;
        }
        r = svcrt_posix_sock_wait((uint32)e->handle, SVCRT_NET_POLL_IN,
                                  e->rcv_to);
        if(r < 0)
        {
            return -1;
        }
        if(r == 0)
        {
            errno = EAGAIN;
            return -1;
        }
    }

    if((addr != 0) && (addrlen != 0))
    {
        svcrt_posix_addr_out(addr, addrlen, ip, port);
    }
    return (svcrt_ssize_t)r;
}

int svcrt_posix_setsockopt(int fd, int level, int optname,
                           const void *optval, socklen_t optlen)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if(level != SOL_SOCKET)
    {
        errno = ENOPROTOOPT;
        return -1;
    }

    if((optname == SO_RCVTIMEO) || (optname == SO_SNDTIMEO))
    {
        /* Kept here and never sent on: the kernel's socket calls do not block,
         * so a timeout is a property of this layer's blocking loops. A zero
         * timeval means "wait for ever", which is what POSIX says it means.
         * Accepting the option and doing nothing would be the worst of both. */
        const struct timeval *tv = (const struct timeval *)optval;
        uint32 ms;

        if((tv == 0) || (optlen < (socklen_t)sizeof(struct timeval)) ||
           (tv->tv_usec < 0) ||
           (tv->tv_usec >= 1000000))
        {
            errno = EINVAL;
            return -1;
        }
        if((tv->tv_sec == 0) && (tv->tv_usec == 0))
        {
            ms = SVCRT_POSIX_NET_FOREVER;
        }
        else
        {
            /* Rounded up, like every other wait in this file: a timeout never
             * fires early. */
            ms = (uint32)tv->tv_sec * 1000u +
                 (uint32)((tv->tv_usec + 999) / 1000);
        }
        if(optname == SO_RCVTIMEO)
        {
            e->rcv_to = ms;
        }
        else
        {
            e->snd_to = ms;
        }
        return 0;
    }

    if((optname == SO_REUSEADDR) || (optname == SO_RCVBUF) ||
       (optname == SO_SNDBUF))
    {
        int    v;
        uint32 wire;

        if((optval == 0) || (optlen < (socklen_t)sizeof(int)))
        {
            errno = EINVAL;
            return -1;
        }
        v = *(const int *)optval;
        if(optname == SO_REUSEADDR)
        {
            wire = SVCRT_NET_SO_REUSEADDR;
        }
        else if(optname == SO_RCVBUF)
        {
            wire = SVCRT_NET_SO_RCVBUF;
        }
        else
        {
            wire = SVCRT_NET_SO_SNDBUF;
        }
        p[0] = SVCRT_NET_SUB_SETSOCKOPT;
        p[1] = (uint32)e->handle;
        p[2] = wire;
        p[3] = (uint32)v;
        return svcrt_posix_net_ctl(p, &r);
    }

    /* Refused, not ignored: an App that believes a timeout or an option is
     * armed when it is not is worse off than one that is told so. */
    errno = ENOPROTOOPT;
    return -1;
}

int svcrt_posix_getsockopt(int fd, int level, int optname,
                           void *optval, socklen_t *optlen)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if((optval == 0) || (optlen == 0))
    {
        errno = EFAULT;
        return -1;
    }
    if(level != SOL_SOCKET)
    {
        errno = ENOPROTOOPT;
        return -1;
    }

    if(optname == SO_TYPE)
    {
        if(*optlen < (socklen_t)sizeof(int))
        {
            errno = EINVAL;
            return -1;
        }
        *(int *)optval = (int)e->sotype;
        *optlen = (socklen_t)sizeof(int);
        return 0;
    }

    if(optname == SO_ERROR)
    {
        uint32 q[SVCRT_NET_ARG_WORDS] = {0u};
        uint32 so = 0u;

        if(*optlen < (socklen_t)sizeof(int))
        {
            errno = EINVAL;
            return -1;
        }
        q[0] = SVCRT_NET_SUB_GETSOCKOPT;
        q[1] = (uint32)e->handle;
        q[2] = SVCRT_NET_SO_ERROR;
        q[3] = (uint32)&so;
        if(svcrt_posix_net_ctl(q, &r) < 0)
        {
            return -1;
        }
        /* The kernel answers in wire codes (negative); POSIX wants an errno
         * number - positive - and 0 for "no error". */
        *(int *)optval = ((int32)so < 0) ? svcrt_posix_errno_from_net((int32)so)
                                         : 0;
        *optlen = (socklen_t)sizeof(int);
        return 0;
    }

    if((optname == SO_RCVTIMEO) || (optname == SO_SNDTIMEO))
    {
        struct timeval *tv = (struct timeval *)optval;
        uint32 ms = (optname == SO_RCVTIMEO) ? e->rcv_to : e->snd_to;

        if(*optlen < (socklen_t)sizeof(struct timeval))
        {
            errno = EINVAL;
            return -1;
        }
        if(ms == SVCRT_POSIX_NET_FOREVER)
        {
            tv->tv_sec  = 0;
            tv->tv_usec = 0;
        }
        else
        {
            tv->tv_sec  = (svcrt_time_t)(ms / 1000u);
            tv->tv_usec = (svcrt_useconds_t)((ms % 1000u) * 1000u);
        }
        *optlen = (socklen_t)sizeof(struct timeval);
        return 0;
    }

    errno = ENOPROTOOPT;
    return -1;
}

static int svcrt_posix_sock_name(int fd, struct sockaddr *addr, socklen_t *len,
                                 uint32 sub)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if((addr == 0) || (len == 0))
    {
        errno = EFAULT;
        return -1;
    }
    p[0] = sub;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)&ip;
    p[3] = (uint32)&port;
    if(svcrt_posix_net_ctl(p, &r) < 0)
    {
        return -1;
    }
    svcrt_posix_addr_out(addr, len, ip, port);
    return 0;
}

int svcrt_posix_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return svcrt_posix_sock_name(fd, addr, len, SVCRT_NET_SUB_GETSOCKNAME);
}

int svcrt_posix_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return svcrt_posix_sock_name(fd, addr, len, SVCRT_NET_SUB_GETPEERNAME);
}

int svcrt_posix_shutdown(int fd, int how)
{
    svcrt_posix_fd_t *e = svcrt_posix_sock_slot(fd);
    uint32 p[SVCRT_NET_ARG_WORDS] = {0u};
    int32  r;

    if(e == 0)
    {
        return -1;
    }
    if((how != SHUT_RD) && (how != SHUT_WR) && (how != SHUT_RDWR))
    {
        errno = EINVAL;
        return -1;
    }
    p[0] = SVCRT_NET_SUB_SHUTDOWN;
    p[1] = (uint32)e->handle;
    p[2] = (uint32)how;
    return svcrt_posix_net_ctl(p, &r);
}

int svcrt_posix_fcntl(int fd, int cmd, ...)
{
    svcrt_posix_fd_t *e;
    va_list ap;
    int     arg = 0;

    if(fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);

    e = svcrt_posix_fd_slot(fd);
    if(e == 0)
    {
        /* A raw device handle from svcrt_dev_open(): there is no flag store
         * for it here, and the kernel's own read/write is what would block.
         * F_GETFL is answerable; F_SETFL is refused rather than swallowed. */
        if(cmd == F_GETFL)
        {
            return O_RDWR;
        }
        errno = EBADF;
        return -1;
    }
    if(e->kind != SVCRT_POSIX_FD_SOCK)
    {
        /* Paths and devices have no status flag of their own in this layer. */
        errno = EINVAL;
        return -1;
    }
    if(cmd == F_GETFL)
    {
        return (int)(O_RDWR | ((e->nonblock != 0u) ? O_NONBLOCK : 0));
    }
    if(cmd == F_SETFL)
    {
        e->nonblock = ((arg & O_NONBLOCK) != 0) ? 1u : 0u;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int svcrt_posix_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
                       struct timeval *timeout)
{
    uint32 budget = SVCRT_POSIX_NET_FOREVER;
    uint32 t0;
    int    fd;

    if((nfds < 0) || (nfds > FD_SETSIZE))
    {
        /* Above FD_SETSIZE the bit for a descriptor would land in the wrong
         * place, so this is refused rather than masked into something else. */
        errno = EINVAL;
        return -1;
    }
    if(timeout != 0)
    {
        if((timeout->tv_usec < 0) || (timeout->tv_usec >= 1000000))
        {
            errno = EINVAL;
            return -1;
        }
        budget = (uint32)timeout->tv_sec * 1000u +
                 (uint32)((timeout->tv_usec + 999) / 1000);
    }
    t0 = svcrt_get_time_ms();

    for(;;)
    {
        fd_set rout;
        fd_set wout;
        fd_set eout;
        int    ready = 0;

        FD_ZERO(&rout);
        FD_ZERO(&wout);
        FD_ZERO(&eout);

        for(fd = 0; fd < nfds; fd++)
        {
            int want_r = ((rfds != 0) && (FD_ISSET(fd, rfds) != 0u)) ? 1 : 0;
            int want_w = ((wfds != 0) && (FD_ISSET(fd, wfds) != 0u)) ? 1 : 0;
            int want_e = ((efds != 0) && (FD_ISSET(fd, efds) != 0u)) ? 1 : 0;
            svcrt_posix_fd_t *e;
            uint32 flags = 0u;
            int32  bits;

            if((want_r == 0) && (want_w == 0) && (want_e == 0))
            {
                continue;
            }

            e = svcrt_posix_fd_slot(fd);
            if((e == 0) || (e->kind != SVCRT_POSIX_FD_SOCK))
            {
                /* A path, a device, or a raw device handle. None of them has a
                 * "not yet" state, which is the same answer POSIX gives for a
                 * regular file: ready. Blocking on one would never end. */
                if(want_r != 0)
                {
                    FD_SET(fd, &rout);
                }
                if(want_w != 0)
                {
                    FD_SET(fd, &wout);
                }
                if(want_e != 0)
                {
                    FD_SET(fd, &eout);
                }
                ready++;
                continue;
            }

            if(want_r != 0)
            {
                flags |= SVCRT_NET_POLL_IN;
            }
            if(want_w != 0)
            {
                flags |= SVCRT_NET_POLL_OUT;
            }
            if(want_e != 0)
            {
                flags |= SVCRT_NET_POLL_ERR;
            }
            bits = svcrt_posix_net_bits((uint32)e->handle, flags);
            if(bits < 0)
            {
                return -1;
            }
            if((want_r != 0) && ((bits & SVCRT_NET_POLL_IN) != 0))
            {
                FD_SET(fd, &rout);
            }
            if((want_w != 0) && ((bits & SVCRT_NET_POLL_OUT) != 0))
            {
                FD_SET(fd, &wout);
            }
            if((want_e != 0) && ((bits & SVCRT_NET_POLL_ERR) != 0))
            {
                FD_SET(fd, &eout);
            }
            if((bits & flags) != 0)
            {
                ready++;
            }
        }

        if(ready > 0)
        {
            if(rfds != 0)
            {
                *rfds = rout;
            }
            if(wfds != 0)
            {
                *wfds = wout;
            }
            if(efds != 0)
            {
                *efds = eout;
            }
            return ready;
        }

        if((budget != SVCRT_POSIX_NET_FOREVER) &&
           ((uint32)(svcrt_get_time_ms() - t0) >= budget))
        {
            /* The timeout: POSIX clears all three sets. */
            if(rfds != 0)
            {
                FD_ZERO(rfds);
            }
            if(wfds != 0)
            {
                FD_ZERO(wfds);
            }
            if(efds != 0)
            {
                FD_ZERO(efds);
            }
            return 0;
        }
        svcrt_task_wait(1u);
    }
}

#endif /* SVCRT_USE_POSIX */
