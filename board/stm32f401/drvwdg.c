/**
* @file drvwdg.c
* @brief STM32F401 independent watchdog (IWDG) - see svcrt_hal.h for the contract
* @details Register access only; four registers, no HAL module. The IWDG is
*          clocked from the LSI, which this file turns on itself, so nothing
*          here depends on the PLL settings or on the system clock being
*          correct.
*
*          Two properties shape the code:
*            - the counter cannot be stopped once started (short of an option
*              byte change), so the decision to start it belongs to the caller
*              (SVCRT_WDG_ENABLE) and this file never starts it on its own;
*            - the LSI is only accurate to about +/- 50%, so the timeout is
*              nominal. The kernel guard treats it as "roughly this long", not
*              as a deadline it can reason about to the millisecond.
*
* @author xw
* @date 2026.09.21
*/

#include "svcrt_types.h"
#include "svcrt_hal.h"
#include "svcrt_board_config.h"

/* IWDG register block (STM32F4 RM0090 section 22). */
#define WDG_IWDG_BASE       (0x40003000u)
#define WDG_KR              (*(volatile uint32 *)(WDG_IWDG_BASE + 0x00u))
#define WDG_PR              (*(volatile uint32 *)(WDG_IWDG_BASE + 0x04u))
#define WDG_RLR             (*(volatile uint32 *)(WDG_IWDG_BASE + 0x08u))
#define WDG_SR              (*(volatile uint32 *)(WDG_IWDG_BASE + 0x0Cu))

#define WDG_KR_UNLOCK       (0x5555u)   /* allow PR / RLR writes */
#define WDG_KR_START        (0xCCCCu)   /* start the counter      */
#define WDG_KR_FEED         (0xAAAAu)   /* reload the counter     */

#define WDG_SR_PVU          (1u << 0)   /* prescaler update in progress */
#define WDG_SR_RVU          (1u << 1)   /* reload update in progress    */

/* LSI nominal frequency used for the timeout arithmetic. */
#define WDG_LSI_HZ          (32000u)

/* RCC control/status register. The IWDG is clocked from the LSI, and on
 * this part the prescaler/reload updates are only carried out while the
 * IWDG counter is running, so the sequence below has to turn the LSI on
 * and start the counter before it programs anything. */
#define WDG_RCC_CSR         (*(volatile uint32 *)(0x40023874u))
#define WDG_RCC_CSR_LSION   (1u << 0)
#define WDG_RCC_CSR_LSIRDY  (1u << 1)

/* Bounded spins: every wait below has to end even when the hardware
 * never answers, because the alternative is a kernel that hangs before
 * the scheduler runs and a board that looks dead. */
#define WDG_SPIN_LIMIT      (100000u)

/* Largest prescaler the IWDG offers is /256. */
#define WDG_PR_MAX          (6u)

/* Debug freeze: while the core is halted under a debugger the counter must
 * stop, otherwise a flashing or inspection session longer than the timeout
 * resets the target in the middle of it. The bit lives in the debug unit and
 * is cleared by every reset, so it is written here on each boot rather than
 * once at bring-up. (IWDG_STOP = bit 12, WWDG_STOP = bit 11 of DBGMCU_APB1_FZ.) */
#define WDG_DBGMCU_APB1_FZ  (*(volatile uint32 *)0xE0042008u)

static uint32 g_wdg_timeout_ms = 0u;

int32 svcrt_port_wdg_init(uint32 timeout_ms)
{
    uint32 pr;
    uint32 ticks;
    uint32 rlr;
    uint32 spin;
    uint32 hw_pr;
    uint32 hw_rlr;

    if(timeout_ms == 0u)
    {
        return -1;
    }

    g_wdg_timeout_ms = 0u;

    /* Pick the smallest prescaler that still fits 12 bits of reload.
     * t = (RLR + 1) * 4 * 2^PR / LSI_HZ, so with the timeout in ms:
     *     RLR + 1 = timeout_ms * 32 / (4 * 2^PR) */
    for(pr = 0u; pr <= WDG_PR_MAX; pr++)
    {
        ticks = (uint32)((timeout_ms * (WDG_LSI_HZ / 1000u)) / (4u << pr));

        if((ticks >= 1u) && (ticks <= 0x1000u))
        {
            break;
        }
    }

    if(pr > WDG_PR_MAX)
    {
        /* The requested timeout cannot be represented; do not start the
         * counter with a timeout the caller did not ask for. */
        return -1;
    }

    rlr = ticks - 1u;

    /* The LSI has to be running before the IWDG answers at all. Whether
     * something else already turned it on is not known here, so read back
     * and wait for the ready flag rather than assume it. */
    WDG_RCC_CSR |= WDG_RCC_CSR_LSION;
    for(spin = 0u; spin < WDG_SPIN_LIMIT; spin++)
    {
        if((WDG_RCC_CSR & WDG_RCC_CSR_LSIRDY) != 0u)
        {
            break;
        }
    }
    if((WDG_RCC_CSR & WDG_RCC_CSR_LSIRDY) == 0u)
    {
        /* Nothing has been started yet, so refusing here leaves the board
         * exactly as it was. */
        return -1;
    }

    /* Freeze the counter while the core is halted under a debugger, before
     * the counter exists: a flashing or inspection session longer than the
     * timeout must not reset the target in the middle of itself. */
    WDG_DBGMCU_APB1_FZ |= (uint32)((1u << 12) | (1u << 11));

    /* Start first, program second. Writing PR before the start leaves
     * WDG_SR.PVU set forever on this part, because the update is only
     * carried out while the counter is clocked - that is a hang, not a
     * failed write, so the order is not a matter of taste. The counter runs
     * with its reset value (about half a second nominal) until the reload
     * below lands, which is why the configuration has to stay short. */
    WDG_KR = WDG_KR_START;
    WDG_KR = WDG_KR_UNLOCK;

    WDG_PR = pr;
    for(spin = 0u; spin < WDG_SPIN_LIMIT; spin++)
    {
        if((WDG_SR & WDG_SR_PVU) == 0u)
        {
            break;
        }
    }

    WDG_RLR = rlr;
    for(spin = 0u; spin < WDG_SPIN_LIMIT; spin++)
    {
        if((WDG_SR & WDG_SR_RVU) == 0u)
        {
            break;
        }
    }

    WDG_KR = WDG_KR_FEED;

    /* Report what the registers hold, not what was asked for: from here on
     * the counter cannot be stopped, so a timeout that did not take is a
     * fact the reader of `guard` has to see, not a reason to fail. */
    hw_pr  = (uint32)(WDG_PR  & 0x7u);
    hw_rlr = (uint32)(WDG_RLR & 0xFFFu);

    g_wdg_timeout_ms = (uint32)(((hw_rlr + 1u) * (4u << hw_pr) * 1000u) / WDG_LSI_HZ);

    return 0;
}

void svcrt_port_wdg_feed(void)
{
    WDG_KR = WDG_KR_FEED;
}

uint32 svcrt_port_wdg_timeout_ms(void)
{
    return g_wdg_timeout_ms;
}
