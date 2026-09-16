/* ============================================================
 * ark_shell_history.c - Command History System Implementation
 *
 * Circular array storage with up/down arrow navigation and search.
 * Current input before pressing up is saved to saved_line,
 * restored when navigating back down to the bottom.
 * ============================================================ */
#include "ark_shell_history.h"
#include "platform.h"
#include <string.h>

/* ============================================================
 * Helper: copy string (length-limited, ensures '\0' termination)
 * ============================================================ */
static void hist_copy_str(char *dst, const char *src, int max_len)
{
    int i = 0;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ============================================================
 * history_init - Initialize history system
 * ============================================================ */
void history_init(history_t *hist)
{
    hist->count         = 0;
    hist->current_index = 0;
    hist->saved_line[0] = '\0';
    /* buf array is statically allocated, no explicit zeroing needed */
}

/* ============================================================
 * history_add - Add a command to history
 *
 * Rules:
 *   1. Empty or whitespace-only lines are not added
 *   2. Duplicate of most recent entry is skipped
 *   3. Circular overwrite of oldest entry when full
 *   4. current_index reset to count (free input state)
 * ============================================================ */
void history_add(history_t *hist, const char *line)
{
    int i;
    int write_idx;
    int has_non_space = 0;

    if (hist == NULL || line == NULL) {
        return;
    }

    /* Check for empty/whitespace-only line */
    for (i = 0; line[i] != '\0'; i++) {
        if (line[i] != ' ') {
            has_non_space = 1;
            break;
        }
    }
    if (!has_non_space) {
        return;
    }

    /* Skip if same as most recent entry */
    if (hist->count > 0) {
        int last_idx = (hist->count <= ARK_SHELL_HISTORY_CAPACITY)
                     ? (hist->count - 1)
                     : (ARK_SHELL_HISTORY_CAPACITY - 1);
        if (strcmp(hist->buf[last_idx], line) == 0) {
            /* Reset view position */
            hist->current_index = hist->count;
            hist->saved_line[0] = '\0';
            return;
        }
    }

    /* Write position (circular) */
    if (hist->count < ARK_SHELL_HISTORY_CAPACITY) {
        write_idx = hist->count;
        hist->count++;
    } else {
        /* Circular overwrite: shift everything left by one, oldest is dropped */
        for (i = 0; i < ARK_SHELL_HISTORY_CAPACITY - 1; i++) {
            strcpy(hist->buf[i], hist->buf[i + 1]);
        }
        write_idx = ARK_SHELL_HISTORY_CAPACITY - 1;
    }

    hist_copy_str(hist->buf[write_idx], line, ARK_SHELL_HISTORY_LINE_SIZE);

    /* Reset view position to after newest (free input state) */
    hist->current_index = hist->count;
    hist->saved_line[0] = '\0';
}

/* ============================================================
 * history_up - Press up arrow: show previous history
 *
 * Returns: 0=ok, -1=at oldest
 *
 * Logic:
 *   - First up press: save current editor content to saved_line
 *   - Decrement current_index (not below 0)
 *   - Display buf[current_index]
 * ============================================================ */
int history_up(history_t *hist, line_editor_t *le)
{
    int target;

    if (hist == NULL || le == NULL) {
        return -1;
    }

    if (hist->count == 0) {
        return -1;  /* No history */
    }

    /* Calculate target index */
    if (hist->current_index > hist->count) {
        hist->current_index = hist->count;
    }

    if (hist->current_index == 0) {
        return -1;  /* Already at oldest */
    }

    /* First up press: save current input */
    if (hist->current_index == hist->count) {
        char *cur = line_editor_get_string(le);
        hist_copy_str(hist->saved_line, cur, ARK_SHELL_HISTORY_LINE_SIZE);
    }

    target = hist->current_index - 1;
    hist->current_index = target;

    /* Display history command */
    line_editor_set_string(le, hist->buf[target]);

    return 0;
}

/* ============================================================
 * history_down - Press down arrow: show next history
 *
 * Returns: 0=ok, -1=at newest
 *
 * Logic:
 *   - Increment current_index (not exceeding count)
 *   - If reached count, restore saved_line
 * ============================================================ */
int history_down(history_t *hist, line_editor_t *le)
{
    if (hist == NULL || le == NULL) {
        return -1;
    }

    if (hist->count == 0) {
        return -1;
    }

    if (hist->current_index >= hist->count) {
        return -1;  /* Already in free input state */
    }

    hist->current_index++;

    if (hist->current_index >= hist->count) {
        /* Restore saved current input */
        line_editor_set_string(le, hist->saved_line);
    } else {
        line_editor_set_string(le, hist->buf[hist->current_index]);
    }

    return 0;
}

/* ============================================================
 * history_search - Search history commands (for Ctrl+R)
 *
 * Parameters: keyword - search keyword
 * Returns: pointer to matching command (read-only), NULL if not found
 *
 * Logic: Scan from newest to oldest, return first match containing keyword
 * ============================================================ */
const char *history_search(const history_t *hist, const char *keyword)
{
    int i;
    int start;

    if (hist == NULL || keyword == NULL || hist->count == 0) {
        return NULL;
    }

    /* Search from newest */
    start = (hist->count <= ARK_SHELL_HISTORY_CAPACITY)
          ? (hist->count - 1)
          : (ARK_SHELL_HISTORY_CAPACITY - 1);

    for (i = start; i >= 0; i--) {
        if (strstr(hist->buf[i], keyword) != NULL) {
            return hist->buf[i];
        }
    }

    return NULL;
}
