/**
* @brief SVCrtOS æ¶ˆæ¯é˜Ÿåˆ—æ¨¡å—å®ç°
* @details å®šé•¿æ‹·è´è¯­ä¹‰çš„æ¶ˆæ¯é˜Ÿåˆ—ï¼š
*          - sendï¼šé˜Ÿåˆ—æœªæ»¡æ—¶æ‹·è´æ¶ˆæ¯å¹¶å”¤é†’ä¸€ä¸ªæ¥æ”¶è€…ï¼›å·²æ»¡æ—¶é˜»å¡ï¼ˆå¸¦è¶…æ—¶ï¼‰æˆ–ç«‹å³å¤±è´¥
*          - recvï¼šé˜Ÿåˆ—éç©ºæ—¶å–å‡ºä¸€æ¡å¹¶å”¤é†’ä¸€ä¸ªå‘é€è€…ï¼›ä¸ºç©ºæ—¶é˜»å¡ï¼ˆå¸¦è¶…æ—¶ï¼‰æˆ–ç«‹å³å¤±è´¥
*          - send_from_isrï¼šä¸­æ–­ä¸Šä¸‹æ–‡å®‰å…¨ï¼Œä»…å…¥é˜Ÿå”¤é†’ï¼Œä¸é˜»å¡ã€ä¸åšä¸Šä¸‹æ–‡åˆ‡æ¢
*          è¿”å›å€¼çº¦å®šï¼š0=æˆåŠŸï¼Œ1=è¶…æ—¶ï¼Œ-1=é”™è¯¯ï¼ˆå¥æŸ„éæ³•/é˜Ÿåˆ—æ»¡ä¸”ä¸ç­‰å¾…/ç­‰å¾…è€…æ»¡ç­‰ï¼‰
*/

#include "svcrt_mq.h"
#include "svcrt_cfg.h"
#include "svcrt_hal.h"

#if (SVCRT_USE_MQ == 1)

static svcrt_mq_obj_t svcrt_mqs[SVCRT_MQ_NUM];

void svcrt_mq_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        svcrt_mqs[i].name[0] = 0;
        svcrt_mqs[i].used    = 0;
        svcrt_mqs[i].head    = 0;
        svcrt_mqs[i].tail    = 0;
        svcrt_mqs[i].count   = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            svcrt_mqs[i].send_waiters[j] = 0;
            svcrt_mqs[i].recv_waiters[j] = 0;
        }
    }
}

static void svcrt_mq_copy_name(char *dst, char *src)
{
    int32 i;
    for(i = 0; i < 7; i++)
    {
        dst[i] = src[i];
        if(src[i] == 0)
            break;
    }
    dst[7] = 0;
}

/* é˜Ÿåˆ—æ“ä½œç»Ÿä¸€å‡è®¾ï¼šè°ƒç”¨è€…å·²æŒæœ‰å…³ä¸­æ–­ä¸´ç•ŒåŒº */
static void svcrt_mq_put(svcrt_mq_obj_t *p_mq, uint32 *p_src)
{
    uint32 *p_dst;
    int32 i;

    p_dst = &p_mq->buf[p_mq->tail * SVCRT_MQ_MSG_WORDS];
    for(i = 0; i < SVCRT_MQ_MSG_WORDS; i++)
        p_dst[i] = p_src[i];
    p_mq->tail = (p_mq->tail + 1) % SVCRT_MQ_DEPTH;
    p_mq->count++;
}

static void svcrt_mq_get(svcrt_mq_obj_t *p_mq, uint32 *p_dst)
{
    uint32 *p_src;
    int32 i;

    p_src = &p_mq->buf[p_mq->head * SVCRT_MQ_MSG_WORDS];
    for(i = 0; i < SVCRT_MQ_MSG_WORDS; i++)
        p_dst[i] = p_src[i];
    p_mq->head = (p_mq->head + 1) % SVCRT_MQ_DEPTH;
    p_mq->count--;
}

static void svcrt_mq_wake_one(svcrt_task_t **waiters)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            waiters[j]->wait_time = 0;
            waiters[j]->wake_reason = 0;
            waiters[j]->status = SVCRT_TASK_READY;
            waiters[j] = 0;
            return;
        }
    }
}

static int32 svcrt_mq_waiter_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == 0)
        {
            waiters[j] = p_tsk;
            return 0;
        }
    }
    return -1;
}

/* ä»ç­‰å¾…åˆ—è¡¨ä¸­æ‘˜é™¤å½“å‰ä»»åŠ¡ï¼ˆè¶…æ—¶å”¤é†’æ—¶ç”±æ¥æ”¶è·¯å¾„æ¸…ç†ï¼‰ */
static void svcrt_mq_waiter_remove(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
        }
    }
}

/* »½ĞÑµÈ´ı¶ÓÁĞÀïµÄÈ«²¿ÈÎÎñ£¨¶ÔÏó±»É¾³ıÊ±ÓÃ£©£º
 * Ô­ÒòÖÃ SVCRT_WAKE_OBJ_DELETED£¬ÈÃµÈ´ıÕß·µ»Ø´íÎó¶ø²»ÊÇÓÀÔ¶Ë¯ÏÂÈ¥¡£ */
static void svcrt_mq_wake_all(svcrt_task_t **waiters, int32 reason)
{
    int32 j;

    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            waiters[j]->wait_time   = 0;
            waiters[j]->wake_reason = reason;
            waiters[j]->status      = SVCRT_TASK_READY;
            waiters[j]              = 0;
        }
    }
}

/* ÈÎÎñÏÂÏßÊÕÊ¬£º°ÑÈÎÎñ´ÓËùÓĞÏûÏ¢¶ÓÁĞµÄÊÕ/·¢µÈ´ı¶ÓÁĞÖĞÕª³ı¡£
 * µ÷ÓÃ·½Ğè×ÔĞĞ±£Ö¤ÁÙ½çÇø£¨±¾º¯Êı²»¿ª¹ØÖĞ¶Ï£©¡£ */
void svcrt_mq_release_task(int32 task_id)
{
    svcrt_task_t *p_tsk;
    int32 i, j;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_tsk = &svcrt_task_table[task_id - 1];

    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        if(svcrt_mqs[i].used == 0)
        {
            continue;
        }

        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            if(svcrt_mqs[i].send_waiters[j] == p_tsk)
            {
                svcrt_mqs[i].send_waiters[j] = 0;
            }
            if(svcrt_mqs[i].recv_waiters[j] == p_tsk)
            {
                svcrt_mqs[i].recv_waiters[j] = 0;
            }
        }
    }
}

int32 svcrt_mq_create_internal(char *name)
{
    int32 i;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        if(svcrt_mqs[i].used == 0)
        {
            svcrt_mq_copy_name(svcrt_mqs[i].name, name);
            svcrt_mqs[i].used  = 1;
            svcrt_mqs[i].head  = 0;
            svcrt_mqs[i].tail  = 0;
            svcrt_mqs[i].count = 0;
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_MQ_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

int32 svcrt_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 ret = 0;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count < SVCRT_MQ_DEPTH)
    {
        svcrt_mq_put(p_mq, buf);
        svcrt_mq_wake_one(p_mq->recv_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return 0;
    }

    /* é˜Ÿåˆ—æ»¡ï¼šä¸ç­‰å¾…åˆ™ç«‹å³å¤±è´¥ */
    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* ×¢Òâ£º´Ë´¦²»·Å¿ªÖĞ¶Ï¡ª¡ªÈë¶ÓÓëÏÂÃæµÄ block_in_critical ±ØĞëÔÚÍ¬Ò»ÁÙ½çÇøÄÚ£¬
     * ·ñÔò ISR µÄ»½ĞÑ»áÍ¶¸øÒ»¸ö»¹Ã»Ë¯ÏÂµÄÈÎÎñ£¨»½ĞÑ¶ªÊ§£©¡£ */

    /* é˜»å¡ç­‰å¾…æ¥æ”¶è€…è…¾å‡ºç©ºé—´ï¼ˆwake_reason: 0=è¢«å”¤é†’ 1=è¶…æ—¶ï¼‰ */
    ret = svcrt_task_block_in_critical((uint32)timeout_ms);   /* ¹ØÖĞ¶Ï·µ»Ø */

    if(ret < 0)
    {
        /* Î´ÄÜ½øÈë×èÈû£¨ÄÚºËÉÏÏÂÎÄ/µ÷¶ÈÆ÷Ëø¶¨£©£º³·ÏúÈë¶Ó */
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == SVCRT_WAKE_OBJ_DELETED)
    {
        /* µÈ´ıÆÚ¼äÏûÏ¢¶ÓÁĞ±»É¾³ı£º¶ÓÁĞÒÑ±»É¾³ı·½Çå¿Õ£¬Ö±½Ó·µ»ØÊ§°Ü */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == 1)
    {
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }
    /* è¢«å”¤é†’æ—¶ç©ºé—´å¯èƒ½å·²è¢«ä»–äººå ç”¨ï¼ˆå¤šä¸ªå‘é€è€…è¢«åŒæ—¶å”¤é†’çš„åœºæ™¯ä¸å­˜åœ¨ï¼Œ
     * ä½†ä¸ºç¨³å¦¥èµ·è§ä»æ£€æŸ¥ä¸€æ¬¡ï¼›å ç”¨åˆ™æŒ‰é”™è¯¯å¤„ç†ï¼‰ */
    if(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_put(p_mq, buf);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 reason;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count > 0)
    {
        svcrt_mq_get(p_mq, buf);
        svcrt_mq_wake_one(p_mq->send_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return 0;
    }

    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* Í¬ mq_send£º²»·Å¿ªÖĞ¶Ï£¬Èë¶ÓÓë block_in_critical ±ØĞëÍ¬´¦Ò»¸öÁÙ½çÇø */

    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* ¹ØÖĞ¶Ï·µ»Ø */

    if(reason < 0)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        /* µÈ´ıÆÚ¼äÏûÏ¢¶ÓÁĞ±»É¾³ı£º¶ÓÁĞÒÑ±»É¾³ı·½Çå¿Õ£¬Ö±½Ó·µ»ØÊ§°Ü */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == 1)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }
    if(p_mq->count <= 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_get(p_mq, buf);
    svcrt_mq_wake_one(p_mq->send_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_mq_send_from_isr_internal(int32 handle, void *buf, int32 len_words)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_put(p_mq, (uint32 *)buf);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    /* ä¸åœ¨ ISR å†…åšä¸Šä¸‹æ–‡åˆ‡æ¢ï¼šå”¤é†’çš„ä»»åŠ¡ç½® READY åç”±ä¸‹ä¸€æ¬¡è°ƒåº¦æ¥ç®¡ */
    return 0;
}

int32 svcrt_mq_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    /* ÏÈ»½ĞÑËùÓĞµÈ´ıÕß£¨Ô­Òò=¶ÔÏóÒÑÉ¾³ı£©£¬·ñÔò¶ÓÁĞÇå¿ÕºóÃ»ÓĞÈËÔÙ»½ĞÑËüÃÇ */
    svcrt_mq_wake_all(svcrt_mqs[idx].send_waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_mq_wake_all(svcrt_mqs[idx].recv_waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_mqs[idx].used    = 0;
    svcrt_mqs[idx].name[0] = 0;
    svcrt_mqs[idx].head    = 0;
    svcrt_mqs[idx].tail    = 0;
    svcrt_mqs[idx].count   = 0;
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

#endif /* SVCRT_USE_MQ */
