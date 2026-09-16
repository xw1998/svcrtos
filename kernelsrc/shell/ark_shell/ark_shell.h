/* ============================================================
 * ark_shell.h - Shell Main Struct and Core API
 *
 * This is the top-level header of the entire Ark Shell framework.
 * It contains:
 *   - ark_shell_t main struct (packs all subsystem states together)
 *   - ark_shell_init() initialization
 *   - ark_shell_run() main loop (non-blocking, processes one byte per call)
 *   - ark_shell_set_uart_mode() runtime UART mode switching
 *
 * Typical usage:
 *   #include "ark_shell.h"
 *   ark_shell_t g_shell;
 *   ark_shell_init(&g_shell, 115200);
 *   while (1) {
 *       ark_shell_run(&g_shell);
 *       // ... other tasks ...
 *   }
 * ============================================================ */
#ifndef ARK_SHELL_H
#define ARK_SHELL_H

#include "ark_shell_config.h"
#include "platform.h"
#include "ark_shell_line_editor.h"
#include "ark_shell_history.h"
#include "ark_shell_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * ark_shell_state_t - Byte processing state machine states
 * ============================================================ */
typedef enum {
    STATE_NORMAL = 0,   /* Normal state */
    STATE_ESC,          /* Received ESC(0x1B), waiting for next byte */
    STATE_ESC_BRACKET,  /* Received ESC[ or ESCO, waiting for sequence end */
    STATE_SEARCH        /* Ctrl+R history search in progress (optional) */
} ark_shell_state_t;

/* ============================================================
 * ark_shell_t - Shell main struct
 *
 * The entire Shell needs only this one global instance variable.
 * ============================================================ */
typedef struct ark_shell_s {
    line_editor_t     editor;                              /* Line editor */
    history_t         history;                             /* Command history */
    ark_shell_state_t state;                               /* State machine current state */
    int               ctrl_c_flag;                         /* Ctrl+C interrupt flag */
    uart_mode_t       uart_mode;                           /* Current UART working mode */
    char              esc_buf[ARK_SHELL_ESC_BUF_SIZE];     /* ESC sequence buffer */
    int               esc_len;                             /* ESC sequence received length */
    uint32_t          esc_start_time;                      /* Timestamp when entering ESC state */
#if ARK_SHELL_ENABLE_HISTORY_SEARCH
    char              search_buf[ARK_SHELL_LINE_SIZE];     /* Search keyword */
    int               search_len;                          /* Search keyword length */
    const char       *search_match;                        /* Search match result */
#endif
} ark_shell_t;

/* ============================================================
 * ark_shell_init - Shell initialization
 *
 * Parameters:
 *   shell    - Shell instance pointer
 *   baudrate - UART baud rate (e.g. 115200)
 * Calls platform_uart_init(), initializes editor, history, state machine
 * ============================================================ */
void ark_shell_init(ark_shell_t *shell, uint32_t baudrate);

/* ============================================================
 * ark_shell_run - Shell main loop (non-blocking)
 *
 * Parameters: shell - Shell instance pointer
 * Reads one byte from UART and processes it. Returns immediately if no data.
 * Call repeatedly in main() while(1) loop.
 * ============================================================ */
void ark_shell_run(ark_shell_t *shell);

/* ============================================================
 * ark_shell_process_byte - Process a single byte (state machine entry)
 *
 * Parameters:
 *   shell - Shell instance pointer
 *   byte  - received byte
 * ============================================================ */
void ark_shell_process_byte(ark_shell_t *shell, unsigned char byte);

/* ============================================================
 * ark_shell_execute_line - Execute current command in editor
 *
 * Steps: add to history -> tokenize -> lookup table -> execute -> clear editor
 * ============================================================ */
void ark_shell_execute_line(ark_shell_t *shell);

/* ============================================================
 * ark_shell_set_uart_mode - Switch UART working mode at runtime
 *
 * Parameters:
 *   shell - Shell instance pointer
 *   mode  - new mode (POLL / IRQ / DMA)
 * Wraps platform_uart_set_mode(), updates shell internal state
 * ============================================================ */
void ark_shell_set_uart_mode(ark_shell_t *shell, uart_mode_t mode);

/* ============================================================
 * ark_shell_printf - Formatted output to UART (needs platform snprintf)
 *
 * Parameters: similar to printf
 * Can be disabled if platform lacks snprintf
 * ============================================================ */
void ark_shell_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_H */
