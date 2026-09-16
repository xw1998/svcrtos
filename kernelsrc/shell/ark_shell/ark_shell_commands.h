/* ============================================================
 * ark_shell_commands.h - Command Registration System Declaration
 *
 * Defines command struct, registration macro, command table,
 * and lookup function.
 * ============================================================ */
#ifndef ARK_SHELL_COMMANDS_H
#define ARK_SHELL_COMMANDS_H

#include "ark_shell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * ark_shell_cmd_t - Description of a single command
 *
 *   name      - command name (e.g. "help")
 *   func      - handler function pointer, signature int func(int argc, char *argv[])
 *   help      - help text
 *   max_args  - max argument count (including command name itself)
 * ============================================================ */
typedef struct {
    const char *name;
    int       (*func)(int argc, char *argv[]);
    const char *help;
    int         max_args;
} ark_shell_cmd_t;

/* ============================================================
 * ARK_SHELL_CMD - Command registration macro
 *
 * Usage: ARK_SHELL_CMD("name", cmd_func, "help text", max_args)
 * Uses C99 compound literal, works in both array initializers
 * and runtime assignment (e.g. g_cmd_table[i] = ARK_SHELL_CMD(...))
 * ============================================================ */
#define ARK_SHELL_CMD(_name, _func, _help, _max_args) \
    (ark_shell_cmd_t){ (_name), (_func), (_help), (_max_args) }

/* Global command table (defined in ark_shell_commands.c) */
extern ark_shell_cmd_t g_cmd_table[];
extern int             g_cmd_count;

/* Initialize built-in commands */
void ark_shell_commands_init(void);

/* Find command. Returns pointer if found, NULL if not (case-insensitive) */
const ark_shell_cmd_t *ark_shell_find_command(const char *name);

/* ============================================================
 * Built-in command handler declarations
 * ============================================================ */
int cmd_help(int argc, char *argv[]);
int cmd_clear(int argc, char *argv[]);
int cmd_version(int argc, char *argv[]);
int cmd_echo(int argc, char *argv[]);
int cmd_reboot(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_COMMANDS_H */
