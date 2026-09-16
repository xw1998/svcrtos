/* ============================================================
 * ark_shell_commands.c - Command Registration System Implementation
 *
 * Contains built-in commands, command table, command lookup,
 * and tokenizer function.
 * ============================================================ */
#include "ark_shell_commands.h"
#include "ark_shell_config.h"
#include "platform.h"

#include <string.h>

/* ============================================================
 * Global Command Table
 *
 * Commands are registered using the ARK_SHELL_CMD macro.
 * Users can add commands before calling ark_shell_commands_init()
 * in main.c, or directly append here.
 * ============================================================ */
ark_shell_cmd_t g_cmd_table[ARK_SHELL_MAX_COMMANDS + 1];
int             g_cmd_count = 0;

/* ============================================================
 * Internal helper: case-insensitive string compare
 * Returns 0 if equal
 * ============================================================ */
static int cmd_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ============================================================
 * ark_shell_find_command - Find command (case-insensitive)
 * ============================================================ */
const ark_shell_cmd_t *ark_shell_find_command(const char *name)
{
    int i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < g_cmd_count; i++) {
        if (cmd_strcasecmp(g_cmd_table[i].name, name) == 0) {
            return &g_cmd_table[i];
        }
    }
    return NULL;
}

/* ============================================================
 * ark_shell_commands_init - Initialize built-in command table
 *
 * Users can append custom commands after calling this function
 * ============================================================ */
void ark_shell_commands_init(void)
{
    int i = 0;

    /* Register built-in commands */
    g_cmd_table[i++] = ARK_SHELL_CMD("help",   cmd_help,   "Show all commands",          2);
    g_cmd_table[i++] = ARK_SHELL_CMD("clear",  cmd_clear,  "Clear screen",               1);
    g_cmd_table[i++] = ARK_SHELL_CMD("version",cmd_version,"Show shell version",         1);
    g_cmd_table[i++] = ARK_SHELL_CMD("echo",   cmd_echo,   "Echo arguments: echo [..]",  ARK_SHELL_MAX_ARGS);
    g_cmd_table[i++] = ARK_SHELL_CMD("reboot", cmd_reboot, "Reboot system",              1);

    /* Sentinel (for user custom command appending) */
    g_cmd_table[i].name = NULL;
    g_cmd_table[i].func = NULL;
    g_cmd_table[i].help = NULL;
    g_cmd_table[i].max_args = 0;

    g_cmd_count = i;
}

/* ============================================================
 * cmd_help - Show all commands
 * ============================================================ */
int cmd_help(int argc, char *argv[])
{
    int i;
    (void)argc;
    (void)argv;

    platform_uart_send_string("\r\nAvailable commands:\r\n");
    for (i = 0; i < g_cmd_count; i++) {
        platform_uart_send_string("  ");
        platform_uart_send_string(g_cmd_table[i].name);
        platform_uart_send_string("  -  ");
        platform_uart_send_string(g_cmd_table[i].help);
        platform_uart_send_string("\r\n");
    }
    platform_uart_send_string("\r\n");
    return 0;
}

/* ============================================================
 * cmd_clear - Clear screen
 * ============================================================ */
int cmd_clear(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* ESC[2J = clear screen, ESC[H = cursor to home */
    platform_uart_send_string("\x1b[2J\x1b[H");
    return 0;
}

/* ============================================================
 * cmd_version - Show version
 * ============================================================ */
int cmd_version(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    platform_uart_send_string("\r\nArk Shell v1.0\r\n");
    platform_uart_send_string("Build: " __DATE__ " " __TIME__ "\r\n");
    return 0;
}

/* ============================================================
 * cmd_echo - Echo arguments
 * ============================================================ */
int cmd_echo(int argc, char *argv[])
{
    int i;
    platform_uart_send_string("\r\n");
    for (i = 1; i < argc; i++) {
        platform_uart_send_string(argv[i]);
        if (i < argc - 1) {
            platform_uart_send(' ');
        }
    }
    platform_uart_send_string("\r\n");
    return 0;
}

/* ============================================================
 * cmd_reboot - Reboot system
 *
 * On PC outputs a message, on MCU calls NVIC_SystemReset
 * ============================================================ */
int cmd_reboot(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    platform_uart_send_string("\r\nRebooting...\r\n");

#if defined(ARK_SHELL_USE_PLATFORM_REBOOT)
    /* Port supplies the real reset; it never returns. */
    platform_reboot();
#else
    /* PC simulation: no actual reset */
    platform_uart_send_string("(simulated)\r\n");
#endif
    return 0;
}
