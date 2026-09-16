/* ============================================================
 * ark_shell_autocomplete.c - Tab Autocomplete Implementation
 *
 * Strategy:
 *   1. Extract the "current word" before cursor
 *   2. Prefix-match against command table
 *   3. Single match: complete and add a trailing space
 *   4. Multiple matches: complete common prefix;
 *      press Tab twice to list all candidates
 * ============================================================ */
#include "ark_shell_autocomplete.h"
#include "ark_shell.h"
#include "platform.h"
#include "ark_shell_commands.h"
#include "ark_shell_config.h"
#include "ark_shell_line_editor.h"

#include <string.h>

/* ============================================================
 * Helper: check if character is a word separator (space/tab)
 * ============================================================ */
static int is_separator(char c)
{
    return (c == ' ' || c == '\t');
}

/* ============================================================
 * Helper: compute common prefix length of two strings
 * ============================================================ */
static int common_prefix_len(const char *a, const char *b)
{
    int n = 0;
    while (a[n] != '\0' && b[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* ============================================================
 * ark_shell_autocomplete - Perform Tab completion
 *
 * Full flow:
 *   1. Find start position and length of current word before cursor
 *   2. If first word: match against command table;
 *      otherwise generic completion (currently only command names supported)
 *   3. Prefix-match candidates
 *   4. Single candidate: complete to end of word + a space
 *   5. Multiple candidates: complete common prefix;
 *      if Tab pressed again with same prefix, print candidate list
 * ============================================================ */
void ark_shell_autocomplete(struct ark_shell_s *shell)
{
    line_editor_t *le;
    char word[ARK_SHELL_LINE_SIZE];
    int  word_len;
    int  word_start;
    int  i;
    int  match_count = 0;
    int  first_match_idx = -1;
    int  common_len = 0;
    int  is_first_word;
    static char s_last_prefix[ARK_SHELL_LINE_SIZE] = "";
    static int  s_last_listed = 0;
    int  prefix_unchanged;

    if (shell == NULL) {
        return;
    }
    le = &shell->editor;

    /* ---- 1. Extract current word ---- */
    word_start = le->pos;
    while (word_start > 0 && !is_separator(le->buf[word_start - 1])) {
        word_start--;
    }
    word_len = le->pos - word_start;

    /* Copy to word (avoid buffer overlap issues) */
    for (i = 0; i < word_len && i < ARK_SHELL_LINE_SIZE - 1; i++) {
        word[i] = le->buf[word_start + i];
    }
    word[i] = '\0';

    /* Determine if this is the first word */
    is_first_word = 1;
    for (i = 0; i < word_start; i++) {
        if (!is_separator(le->buf[i])) {
            is_first_word = 0;
            break;
        }
    }

    /* Completion for non-first words not implemented (can extend to arg completion) */
    if (!is_first_word) {
        /* Audible bell */
        platform_uart_send('\a');
        return;
    }

    /* ---- 2. Prefix-match against command table ---- */
    for (i = 0; i < g_cmd_count; i++) {
        if (strncmp(g_cmd_table[i].name, word, (size_t)word_len) == 0) {
            if (match_count == 0) {
                first_match_idx = i;
                common_len = (int)strlen(g_cmd_table[i].name);
            } else {
                int plen = common_prefix_len(g_cmd_table[first_match_idx].name,
                                             g_cmd_table[i].name);
                if (plen < common_len) {
                    common_len = plen;
                }
            }
            match_count++;
        }
    }

    if (match_count == 0) {
        /* No match: bell */
        platform_uart_send('\a');
        return;
    }

    /* ---- 3. Check if prefix unchanged (for "press Tab twice to list") ---- */
    prefix_unchanged = (strcmp(word, s_last_prefix) == 0);

    if (match_count == 1) {
        /* Single match: complete full command name + space */
        const char *full = g_cmd_table[first_match_idx].name;
        int full_len = (int)strlen(full);
        int j;

        /* Insert remaining chars from cursor position */
        for (j = word_len; j < full_len; j++) {
            line_editor_insert_char(le, full[j]);
        }
        /* Append a space */
        line_editor_insert_char(le, ' ');

        /* Reset state */
        s_last_prefix[0] = '\0';
        s_last_listed = 0;
    } else {
        /* Multiple matches: complete common prefix */
        const char *first = g_cmd_table[first_match_idx].name;
        int j;

        if (common_len > word_len) {
            for (j = word_len; j < common_len; j++) {
                line_editor_insert_char(le, first[j]);
            }
            /* Update prefix and record */
            for (j = 0; j < common_len && j < ARK_SHELL_LINE_SIZE - 1; j++) {
                s_last_prefix[j] = first[j];
            }
            s_last_prefix[j] = '\0';
            s_last_listed = 0;
        } else {
            /* Common prefix already complete, cannot complete further */
            if (prefix_unchanged && s_last_listed) {
                /* Already listed once, don't repeat on Tab */
                platform_uart_send('\a');
                return;
            }
            if (prefix_unchanged || common_len == word_len) {
                /* List all candidates */
                platform_uart_send_string("\r\n");
                for (i = 0; i < g_cmd_count; i++) {
                    if (strncmp(g_cmd_table[i].name, word, (size_t)word_len) == 0) {
                        platform_uart_send_string("  ");
                        platform_uart_send_string(g_cmd_table[i].name);
                        platform_uart_send_string("\r\n");
                    }
                }
                /* Redraw prompt and current line */
                platform_uart_send_string(le->prompt);
                platform_uart_send_buf((const unsigned char *)le->buf,
                                       (uint16_t)le->len);
                /* Move cursor back to logical position */
                {
                    int back = le->len - le->pos;
                    if (back > 0) {
                        char seq[16];
                        int k = 0;
                        seq[k++] = '\x1b';
                        seq[k++] = '[';
                        {
                            char tmp[12];
                            int tl = 0;
                            int v = back;
                            int m;
                            if (v == 0) tmp[tl++] = '0';
                            while (v > 0) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
                            for (m = tl - 1; m >= 0; m--) seq[k++] = tmp[m];
                        }
                        seq[k++] = 'D';
                        platform_uart_send_buf((const unsigned char *)seq, (uint16_t)k);
                    }
                }
                s_last_listed = 1;
            } else {
                /* Prefix changed, record new prefix */
                int n = common_len < ARK_SHELL_LINE_SIZE - 1 ? common_len : ARK_SHELL_LINE_SIZE - 1;
                for (j = 0; j < n; j++) {
                    s_last_prefix[j] = first[j];
                }
                s_last_prefix[j] = '\0';
                s_last_listed = 0;
            }
        }
    }
}
