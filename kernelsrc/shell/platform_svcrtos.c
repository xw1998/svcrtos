/**
* @file platform_svcrtos.c
* @brief ark-shell 平台适配层：SVCrtOS 内核侧实现
* @details 实现 kernelsrc/shell/platform.h 声明的 9 个平台函数：
*          - 串口收发接到内核设备框架的 SHELL_DEV_NAME（默认 COM1，板级
*            注册见 board/stm32f427/svcrt_board.c 的 usart_drv）；
*          - 毫秒时基接到内核 tick（svcrt_kernel_get_time）；
*          - 复位走 Cortex-M 的 AIRCR.SYSRESETREQ，不依赖板级头文件，
*            内核侧因此不必包含任何 STM32 HAL 头。
*
*          ark-shell 本体（kernelsrc/shell/ark_shell/）保持上游原样，
*          平台差异全部收敛在本文件里。
*
*          UART 模式：板级 usart_drv 是「中断收 + 中断发」的固定实现，
*          没有 DMA 通道，所以 POLL/DMA 只记录状态、不改变硬件行为；
*          接口保留是为了与上游 platform.h 的契约一致。
*
* @note 本模块属于内核特权态代码，使用内核内部接口访问设备。
*/

#include "platform.h"

#include "svcrt_config.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "svcrt_partition.h"      /* SHELL_DEV_NAME / SHELL_DEV_ARG */

/* 发送忙等上限：中断发送在 TxFIFO 满时写不进去，等一小会儿再试。
 * 上限只用于防止串口彻底卡死时死循环，不是流量控制。 */
#define SHELL_TX_SPIN_LIMIT    (200000u)

/* Cortex-M 复位控制：AIRCR 写 0x05FA0004（VECTKEY + SYSRESETREQ）。
 * 直接按地址访问系统控制块，避免内核侧包含 CMSIS 头。 */
#define SHELL_AIRCR_ADDR       (0xE000ED0Cu)
#define SHELL_AIRCR_SYSRESET   (0x05FA0004u)

static int32       g_shell_uart = -1;
static uart_mode_t g_uart_mode  = UART_MODE_IRQ;

void platform_uart_init(uint32_t baudrate)
{
    char name[] = SHELL_DEV_NAME;

    if(g_shell_uart >= 0)
    {
        return;
    }

    g_shell_uart = svcrt_dev_open_internal(name,
                                           (uint32)((baudrate != 0u) ? baudrate
                                                                     : (uint32)SHELL_DEV_ARG));
    g_uart_mode = UART_MODE_IRQ;
}

void platform_uart_send(unsigned char byte)
{
    uint32 spin = 0u;

    if(g_shell_uart < 0)
    {
        return;
    }

    while((svcrt_dev_write_internal(g_shell_uart, (uint8 *)&byte, 1) != 1) &&
          (spin < SHELL_TX_SPIN_LIMIT))
    {
        spin++;
    }
}

void platform_uart_send_string(const char *str)
{
    if((str == 0) || (g_shell_uart < 0))
    {
        return;
    }

    /* 逐字节写：板级发送 FIFO 只有 128 字节，整串一次写会写不进去；
     * 逐字节写同时避免了在任务栈上开临时缓冲。 */
    while(*str != '\0')
    {
        uint32 spin = 0u;

        while((svcrt_dev_write_internal(g_shell_uart, (uint8 *)str, 1) != 1) &&
              (spin < SHELL_TX_SPIN_LIMIT))
        {
            spin++;
        }
        str++;
    }
}

void platform_uart_send_buf(const unsigned char *buf, uint16_t len)
{
    uint16_t i;

    if((buf == 0) || (g_shell_uart < 0))
    {
        return;
    }

    for(i = 0u; i < len; i++)
    {
        uint32 spin = 0u;

        while((svcrt_dev_write_internal(g_shell_uart, (uint8 *)&buf[i], 1) != 1) &&
              (spin < SHELL_TX_SPIN_LIMIT))
        {
            spin++;
        }
    }
}

int platform_uart_recv(void)
{
    uint8 b = 0u;

    if(g_shell_uart < 0)
    {
        return -1;
    }

    /* 板级读是「非阻塞取 FIFO」：有字节返回 1，空返回 0 */
    if(svcrt_dev_read_internal(g_shell_uart, &b, 1) == 1)
    {
        return (int)b;
    }

    return -1;
}

uint32_t platform_tick_ms(void)
{
    return (uint32_t)svcrt_kernel_get_time();
}

void platform_uart_set_mode(uart_mode_t mode)
{
    /* 板级驱动固定为中断收发，没有可切换的硬件路径；
     * 这里只更新记录值，保证 platform_uart_get_mode 自洽。 */
    g_uart_mode = mode;
}

uart_mode_t platform_uart_get_mode(void)
{
    return g_uart_mode;
}

void platform_cleanup(void)
{
    /* MCU 上无需恢复终端状态（PC 模拟专用接口） */
}

void platform_reboot(void)
{
    /* Cortex-M 系统复位请求：AIRCR 写 VECTKEY(0x05FA) | SYSRESETREQ。
     * 无需关中断：复位控制器不看 PRIMASK，请求一旦写入即生效。
     * 这里不用内联汇编也不用 CMSIS，使本文件在 AC5 / AC6 / GCC 下都能编译。 */
    *(volatile uint32 *)SHELL_AIRCR_ADDR = SHELL_AIRCR_SYSRESET;

    /* 正常情况下核心应在数微秒内复位，执行不到这里；真到了就自旋等着 */
    for(;;)
    {
    }
}

int32 svcrt_shell_uart_handle(void)
{
    return g_shell_uart;
}

int32 svcrt_shell_uart_open(void)
{
    if(g_shell_uart < 0)
    {
        platform_uart_init((uint32_t)SHELL_DEV_ARG);
    }

    return g_shell_uart;
}
