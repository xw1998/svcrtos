/* ============================================================
 * ark_shell_history.h - Command History System Declaration
 *
 * Circular array storage with up/down arrow navigation.
 * ============================================================ */
#ifndef ARK_SHELL_HISTORY_H
#define ARK_SHELL_HISTORY_H

#include "ark_shell_config.h"
#include "ark_shell_line_editor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * history_t - History system data structure
 *
 *   buf[CAPACITY][LINE_SIZE] - circular array
 *   saved_line               - temp save (current input before pressing up)
 *   current_index            - current view position (== count means free input)
 *   count                    - number of stored entries
 * ============================================================ */
typedef struct {
    char buf[ARK_SHELL_HISTORY_CAPACITY][ARK_SHELL_HISTORY_LINE_SIZE];
    char saved_line[ARK_SHELL_HISTORY_LINE_SIZE];
    int  current_index;
    int  count;
} history_t;

/* Initialize history system */
void history_init(history_t *hist);

/* Add a command to history (called on Enter). Empty/duplicate lines are skipped */
void history_add(history_t *hist, const char *line);

/* Press up arrow: show previous history command. Returns 0=ok, -1=at oldest */
int history_up(history_t *hist, line_editor_t *le);

/* Press down arrow: show next history command. Returns 0=ok, -1=at newest */
int history_down(history_t *hist, line_editor_t *le);

/* Search history (for Ctrl+R). Returns matching command pointer, NULL if not found */
const char *history_search(const history_t *hist, const char *keyword);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_HISTORY_H */
