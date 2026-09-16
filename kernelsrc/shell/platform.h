/* ============================================================
 * platform.h - Platform Abstraction Layer Interface
 *
 * To port to a new MCU, implement only the 8 functions declared
 * in this header. Reference implementations are provided in
 * platform_linux.c (PC simulation) and platform_port_template.c
 * (template for STM32 and other MCUs).
 *
 * Three UART working modes:
 *   POLL - Polling TX/RX, simplest, high CPU usage
 *   IRQ  - Interrupt RX + polling TX, balanced (default)
 *   DMA  - DMA TX/RX, most efficient, most complex
 * ============================================================ */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * uart_mode_t - UART working mode enum
 * ============================================================ */
typedef enum {
    UART_MODE_POLL = 0,   /* Polling mode */
    UART_MODE_IRQ  = 1,   /* Interrupt mode (default) */
    UART_MODE_DMA  = 2    /* DMA mode */
} uart_mode_t;

/* ============================================================
 * platform_uart_init - UART initialization
 *
 * Parameters:
 *   baudrate - baud rate (e.g. 115200)
 * Configures GPIO/USART/interrupts/DMA and sets default mode (IRQ)
 * ============================================================ */
void platform_uart_init(uint32_t baudrate);

/* ============================================================
 * platform_uart_send - Send a single byte (blocking until sent)
 * ============================================================ */
void platform_uart_send(unsigned char byte);

/* ============================================================
 * platform_uart_send_string - Send a null-terminated string
 * ============================================================ */
void platform_uart_send_string(const char *str);

/* ============================================================
 * platform_uart_send_buf - Send a byte buffer of given length
 *
 * Parameters:
 *   buf - data pointer
 *   len - number of bytes
 * ============================================================ */
void platform_uart_send_buf(const unsigned char *buf, uint16_t len);

/* ============================================================
 * platform_uart_recv - Receive a single byte (non-blocking)
 *
 * Returns:
 *   0..255 - received byte
 *   -1     - no data
 * Regardless of working mode, this interface returns the next
 * byte. In IRQ/DMA mode the byte comes from a ring buffer;
 * in POLL mode it reads the hardware register directly.
 * ============================================================ */
int platform_uart_recv(void);

/* ============================================================
 * platform_tick_ms - Get system millisecond tick
 *
 * Returns: milliseconds since boot (32-bit, wraps in ~49 days)
 * Used for ESC sequence timeout detection, etc.
 * ============================================================ */
uint32_t platform_tick_ms(void);

/* ============================================================
 * platform_uart_set_mode - Switch UART working mode at runtime
 *
 * Parameters: mode - new mode (POLL / IRQ / DMA)
 * May require reconfiguring the USART peripheral
 * ============================================================ */
void platform_uart_set_mode(uart_mode_t mode);

/* ============================================================
 * platform_uart_get_mode - Get current UART working mode
 * ============================================================ */
uart_mode_t platform_uart_get_mode(void);

/* ============================================================
 * platform_cleanup - Platform cleanup (PC simulation only)
 *
 * On MCU this can be an empty implementation
 * ============================================================ */
void platform_cleanup(void);

/* ============================================================
 * ARK_SHELL_USE_PLATFORM_REBOOT - Port-provided reset hook
 *
 * Define it in the port to make the built-in 'reboot' command call
 * platform_reboot() instead of only printing a message.
 * SVCrtOS defines it (see platform_svcrtos.c): the hook triggers an
 * AIRCR.SYSRESETREQ reset and never returns.
 * ============================================================ */
#ifndef ARK_SHELL_USE_PLATFORM_REBOOT
#define ARK_SHELL_USE_PLATFORM_REBOOT   1
#endif
void platform_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
