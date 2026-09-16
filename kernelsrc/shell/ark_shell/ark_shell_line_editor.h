/* ============================================================
 * ark_shell_line_editor.h - Line Editor Declaration
 *
 * Manages the current input line: char insert/delete,
 * cursor movement, screen refresh.
 * ============================================================ */
#ifndef ARK_SHELL_LINE_EDITOR_H
#define ARK_SHELL_LINE_EDITOR_H

#include "ark_shell_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * line_editor_t - Line editor data structure
 *
 *   buf    - line content buffer
 *   len    - current line content length
 *   pos    - cursor position (0 ~ len)
 *   prompt - prompt string pointer
 * ============================================================ */
typedef struct {
    char        buf[ARK_SHELL_LINE_SIZE];
    int         len;
    int         pos;
    const char *prompt;
} line_editor_t;

/* Initialize line editor */
void line_editor_init(line_editor_t *le, const char *prompt);

/* Insert a character at cursor position (with terminal refresh) */
void line_editor_insert_char(line_editor_t *le, char ch);

/* Backspace: delete character before cursor */
void line_editor_backspace(line_editor_t *le);

/* Delete key: delete character at cursor position */
void line_editor_delete(line_editor_t *le);

/* Move cursor left one position */
void line_editor_move_left(line_editor_t *le);

/* Move cursor right one position */
void line_editor_move_right(line_editor_t *le);

/* Move cursor to start of line */
void line_editor_move_home(line_editor_t *le);

/* Move cursor to end of line */
void line_editor_move_end(line_editor_t *le);

/* Delete from start of line to cursor (Ctrl+U) */
void line_editor_delete_to_home(line_editor_t *le);

/* Delete from cursor to end of line (Ctrl+K) */
void line_editor_delete_to_end(line_editor_t *le);

/* Delete word before cursor (Ctrl+W) */
void line_editor_delete_word(line_editor_t *le);

/* Clear current line (no terminal refresh) */
void line_editor_clear(line_editor_t *le);

/* Set editor content from string (for history navigation), refresh terminal */
void line_editor_set_string(line_editor_t *le, const char *str);

/* Get current line string (returns buf pointer, ensures '\0' terminated) */
char *line_editor_get_string(line_editor_t *le);

/* Redraw current line (clear line then redraw from prompt) */
void line_editor_redraw(line_editor_t *le);

/* Clear screen and redraw current line (Ctrl+L) */
void line_editor_clear_screen(line_editor_t *le);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_LINE_EDITOR_H */
