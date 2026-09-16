/* ============================================================
 * ark_shell_line_editor.c - Line Editor Implementation
 *
 * Manages insert/delete/modify of current input line,
 * with real-time terminal refresh.
 * Redraw strategy: save cursor -> clear to EOL -> redraw -> restore cursor
 * ============================================================ */
#include "ark_shell_line_editor.h"
#include "platform.h"
#include <string.h>

/* ============================================================
 * Helper: send ANSI cursor-left n columns
 * Format: ESC [ <n> D
 * ============================================================ */
static void send_cursor_left(int n)
{
    char seq[16];
    int  i = 0;

    if (n <= 0) {
        return;
    }

    seq[i++] = '\x1b';
    seq[i++] = '[';
    /* Convert number to string */
    {
        char tmp[12];
        int  len = 0;
        int  val = n;
        int  j;

        if (val == 0) {
            tmp[len++] = '0';
        }
        while (val > 0) {
            tmp[len++] = (char)('0' + val % 10);
            val /= 10;
        }
        for (j = len - 1; j >= 0; j--) {
            seq[i++] = tmp[j];
        }
    }
    seq[i++] = 'D';
    platform_uart_send_buf((const unsigned char *)seq, (uint16_t)i);
}

/* ============================================================
 * Helper: send ANSI cursor-right n columns
 * Format: ESC [ <n> C
 * ============================================================ */
static void send_cursor_right(int n)
{
    char seq[16];
    int  i = 0;

    if (n <= 0) {
        return;
    }

    seq[i++] = '\x1b';
    seq[i++] = '[';
    {
        char tmp[12];
        int  len = 0;
        int  val = n;
        int  j;

        if (val == 0) {
            tmp[len++] = '0';
        }
        while (val > 0) {
            tmp[len++] = (char)('0' + val % 10);
            val /= 10;
        }
        for (j = len - 1; j >= 0; j--) {
            seq[i++] = tmp[j];
        }
    }
    seq[i++] = 'C';
    platform_uart_send_buf((const unsigned char *)seq, (uint16_t)i);
}

/* ============================================================
 * line_editor_init
 * ============================================================ */
void line_editor_init(line_editor_t *le, const char *prompt)
{
    le->buf[0] = '\0';
    le->len    = 0;
    le->pos    = 0;
    le->prompt = prompt;
}

/* ============================================================
 * line_editor_insert_char - Insert a character at cursor position
 *
 * Redraw strategy:
 *   1. Move cursor left 1 (back to insert position)
 *   2. Clear to end of line
 *   3. Print from insert position to buffer end
 *   4. Move cursor left back to logical cursor position
 * ============================================================ */
void line_editor_insert_char(line_editor_t *le, char ch)
{
    int i;
    int move_back;

    /* Discard if buffer full */
    if (le->len >= ARK_SHELL_LINE_SIZE - 1) {
        return;
    }

    /* Shift characters from cursor to end right by one (copy backwards) */
    for (i = le->len; i > le->pos; i--) {
        le->buf[i] = le->buf[i - 1];
    }

    /* Write new character */
    le->buf[le->pos] = ch;
    le->len++;
    le->pos++;
    le->buf[le->len] = '\0';

    /* ---- Terminal redraw ---- */
    /* Step 1: move cursor left 1 to insert position */
    platform_uart_send_string("\x1b[D");

    /* Step 2: clear from current position to end of line */
    platform_uart_send_string("\x1b[K");

    /* Step 3: print from insert position to end */
    platform_uart_send_buf((const unsigned char *)&le->buf[le->pos - 1],
                           (uint16_t)(le->len - (le->pos - 1)));

    /* Step 4: move cursor back to logical position */
    move_back = le->len - le->pos;
    send_cursor_left(move_back);
}

/* ============================================================
 * line_editor_backspace - Backspace: delete character before cursor
 * ============================================================ */
void line_editor_backspace(line_editor_t *le)
{
    int i;
    int move_back;

    if (le->pos == 0) {
        return;  /* Cursor at start of line */
    }

    /* Shift characters from cursor to end left by one */
    for (i = le->pos; i < le->len; i++) {
        le->buf[i - 1] = le->buf[i];
    }

    le->len--;
    le->pos--;
    le->buf[le->len] = '\0';

    /* ---- Terminal redraw ---- */
    /* Move cursor left 1 */
    platform_uart_send_string("\x1b[D");
    /* Clear to end of line */
    platform_uart_send_string("\x1b[K");
    /* Print from current position to end */
    platform_uart_send_buf((const unsigned char *)&le->buf[le->pos],
                           (uint16_t)(le->len - le->pos));
    /* Move cursor back to logical position */
    move_back = le->len - le->pos;
    send_cursor_left(move_back);
}

/* ============================================================
 * line_editor_delete - Delete key: delete character at cursor position
 * ============================================================ */
void line_editor_delete(line_editor_t *le)
{
    int i;
    int move_back;

    if (le->pos >= le->len) {
        return;  /* Cursor at end of line */
    }

    /* Shift characters from cursor+1 to end left by one */
    for (i = le->pos; i < le->len - 1; i++) {
        le->buf[i] = le->buf[i + 1];
    }

    le->len--;
    le->buf[le->len] = '\0';
    /* pos unchanged */

    /* ---- Terminal redraw ---- */
    /* Cursor already at correct position, clear to end of line */
    platform_uart_send_string("\x1b[K");
    /* Print from current position to end */
    platform_uart_send_buf((const unsigned char *)&le->buf[le->pos],
                           (uint16_t)(le->len - le->pos));
    /* Move cursor back to logical position */
    move_back = le->len - le->pos;
    send_cursor_left(move_back);
}

/* ============================================================
 * line_editor_move_left - Move cursor left one position
 * ============================================================ */
void line_editor_move_left(line_editor_t *le)
{
    if (le->pos > 0) {
        le->pos--;
        platform_uart_send_string("\x1b[1D");
    }
}

/* ============================================================
 * line_editor_move_right - Move cursor right one position
 * ============================================================ */
void line_editor_move_right(line_editor_t *le)
{
    if (le->pos < le->len) {
        le->pos++;
        platform_uart_send_string("\x1b[1C");
    }
}

/* ============================================================
 * line_editor_move_home - Move cursor to start of line (Ctrl+A / Home)
 * ============================================================ */
void line_editor_move_home(line_editor_t *le)
{
    if (le->pos > 0) {
        send_cursor_left(le->pos);
        le->pos = 0;
    }
}

/* ============================================================
 * line_editor_move_end - Move cursor to end of line (Ctrl+E / End)
 * ============================================================ */
void line_editor_move_end(line_editor_t *le)
{
    if (le->pos < le->len) {
        send_cursor_right(le->len - le->pos);
        le->pos = le->len;
    }
}

/* ============================================================
 * line_editor_delete_to_home - Delete from start to cursor (Ctrl+U)
 * ============================================================ */
void line_editor_delete_to_home(line_editor_t *le)
{
    int i;
    int deleted;

    if (le->pos == 0) {
        return;
    }

    deleted = le->pos;

    /* Move chars from pos~len-1 to start */
    for (i = 0; i < le->len - le->pos; i++) {
        le->buf[i] = le->buf[le->pos + i];
    }
    le->len -= deleted;
    le->pos  = 0;
    le->buf[le->len] = '\0';

    /* Move cursor to start of line */
    send_cursor_left(deleted);
    /* Clear to end of line */
    platform_uart_send_string("\x1b[K");
    /* Redraw */
    platform_uart_send_buf((const unsigned char *)le->buf, (uint16_t)le->len);
    /* Cursor stays at start (pos=0), no further movement needed */
}

/* ============================================================
 * line_editor_delete_to_end - Delete from cursor to end (Ctrl+K)
 * ============================================================ */
void line_editor_delete_to_end(line_editor_t *le)
{
    if (le->pos >= le->len) {
        return;
    }

    le->len = le->pos;
    le->buf[le->len] = '\0';

    /* Just clear to end of line, cursor stays */
    platform_uart_send_string("\x1b[K");
}

/* ============================================================
 * line_editor_delete_word - Delete word before cursor (Ctrl+W)
 *
 * Skip spaces leftward, then delete consecutive non-space chars.
 * ============================================================ */
void line_editor_delete_word(line_editor_t *le)
{
    int word_start;
    int i;

    if (le->pos == 0) {
        return;
    }

    /* Skip spaces leftward */
    word_start = le->pos - 1;
    while (word_start > 0 && le->buf[word_start] == ' ') {
        word_start--;
    }
    /* Skip non-space chars leftward */
    while (word_start > 0 && le->buf[word_start - 1] != ' ') {
        word_start--;
    }

    /* Delete chars from word_start ~ pos-1 */
    {
        int deleted = le->pos - word_start;
        for (i = word_start; i < le->len - deleted; i++) {
            le->buf[i] = le->buf[i + deleted];
        }
        le->len -= deleted;
        le->pos  = word_start;
        le->buf[le->len] = '\0';

        /* Redraw */
        send_cursor_left(deleted);
        platform_uart_send_string("\x1b[K");
        platform_uart_send_buf((const unsigned char *)&le->buf[le->pos],
                               (uint16_t)(le->len - le->pos));
        /* Move cursor back to pos */
        send_cursor_left(le->len - le->pos);
    }
}

/* ============================================================
 * line_editor_clear - Clear current line (no terminal refresh)
 * ============================================================ */
void line_editor_clear(line_editor_t *le)
{
    le->buf[0] = '\0';
    le->len    = 0;
    le->pos    = 0;
}

/* ============================================================
 * line_editor_set_string - Set editor content from string and refresh
 *
 * Used for history navigation: clear current display, write new content.
 * ============================================================ */
void line_editor_set_string(line_editor_t *le, const char *str)
{
    int new_len;
    int i;

    /* Move cursor to start of line */
    send_cursor_left(le->pos);
    /* Clear to end of line */
    platform_uart_send_string("\x1b[K");

    /* Copy string to buffer */
    new_len = 0;
    if (str != NULL) {
        while (str[new_len] != '\0' && new_len < ARK_SHELL_LINE_SIZE - 1) {
            le->buf[new_len] = str[new_len];
            new_len++;
        }
    }
    le->buf[new_len] = '\0';
    le->len = new_len;
    le->pos = new_len;

    /* Print new content */
    if (new_len > 0) {
        platform_uart_send_buf((const unsigned char *)le->buf, (uint16_t)new_len);
    }
    (void)i;
}

/* ============================================================
 * line_editor_get_string - Get current line string
 * ============================================================ */
char *line_editor_get_string(line_editor_t *le)
{
    le->buf[le->len] = '\0';
    return le->buf;
}

/* ============================================================
 * line_editor_redraw - Redraw current line
 * ============================================================ */
void line_editor_redraw(line_editor_t *le)
{
    /* Carriage return to start of line */
    platform_uart_send('\r');
    /* Clear to end of line */
    platform_uart_send_string("\x1b[K");
    /* Print prompt */
    platform_uart_send_string(le->prompt);
    /* Print content */
    if (le->len > 0) {
        platform_uart_send_buf((const unsigned char *)le->buf, (uint16_t)le->len);
    }
    /* Move cursor to logical position */
    send_cursor_left(le->len - le->pos);
}

/* ============================================================
 * line_editor_clear_screen - Clear screen and redraw (Ctrl+L)
 * ============================================================ */
void line_editor_clear_screen(line_editor_t *le)
{
    /* ESC[2J = clear screen, ESC[H = cursor to home */
    platform_uart_send_string("\x1b[2J\x1b[H");
    /* Redraw prompt and current line */
    platform_uart_send_string(le->prompt);
    if (le->len > 0) {
        platform_uart_send_buf((const unsigned char *)le->buf, (uint16_t)le->len);
    }
    /* Move cursor to logical position */
    send_cursor_left(le->len - le->pos);
}
