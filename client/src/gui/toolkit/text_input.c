/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Text input API.
 *
 * @author Zoey Rose
 */

#include <SDL3/SDL.h>
#include <event.h>
#include <keybind.h>
#include <main.h>
#include <settings.h>
#include <sprite.h>
#include <text.h>
#include <text_input.h>
#include <video.h>
#include <widget.h>
#include <toolkit/toolkit.h>
#include <toolkit/string.h>

static size_t utf8_previous(const char *str, size_t pos) {
    const char *cursor = str + pos;

    SDL_StepBackUTF8(str, &cursor);
    return (size_t)(cursor - str);
}

static size_t utf8_next(const char *str, size_t len, size_t pos) {
    size_t offset = MIN(len, pos);
    const char *cursor = str + offset;
    size_t remaining = len - offset;

    SDL_StepUTF8(&cursor, &remaining);
    return (size_t)(cursor - str);
}

static bool text_input_codepoint_is_space(Uint32 codepoint) {
    return codepoint <= UCHAR_MAX && isspace((unsigned char)codepoint);
}

static void text_input_skip_word(const char *str, size_t len, size_t *pos, int direction) {
    bool whitespace = true;

    while ((direction < 0 && *pos != 0) || (direction > 0 && *pos < len)) {
        size_t next;
        const char *cursor;

        if (direction < 0) {
            next = utf8_previous(str, *pos);
            cursor = str + next;
        } else {
            cursor = str + *pos;
            next = utf8_next(str, len, *pos);
        }
        Uint32 codepoint = SDL_StepUTF8(&cursor, NULL);

        if (text_input_codepoint_is_space(codepoint)) {
            if (!whitespace) {
                break;
            }
        } else if (whitespace) {
            whitespace = false;
        }

        *pos = next;
    }
}

static void text_input_sanitize(char *text, bool multiline) {
    char *cursor = text;

    while (*cursor != '\0') {
        const char *next = cursor;
        Uint32 codepoint = SDL_StepUTF8(&next, NULL);

        if (codepoint == SDL_INVALID_UNICODE_CODEPOINT) {
            *cursor++ = ' ';
        } else {
            if ((codepoint < ' ' && !(multiline && codepoint == '\n')) || codepoint == 0x7f) {
                *cursor = ' ';
            }
            cursor = (char *)next;
        }
    }
}

static bool text_input_add_text(text_input_struct *text_input, const char *text) {
    size_t capacity = MIN(text_input->max, sizeof(text_input->str) - 1);
    size_t available = capacity - MIN(text_input->num, capacity);
    size_t len = MIN(strlen(text), available);

    while (len > 0 && text[len] != '\0' && ((unsigned char)text[len] & 0xc0) == 0x80) {
        len--;
    }
    if (len == 0) {
        return false;
    }

    if (text_input->character_check_func != NULL) {
        for (size_t i = 0; i < len; i++) {
            unsigned char character = (unsigned char)text[i];
            if (character >= 0x80 ||
                !text_input->character_check_func(text_input, (char)character)) {
                return false;
            }
        }
    }

    memmove(text_input->str + text_input->pos + len,
            text_input->str + text_input->pos,
            text_input->num - text_input->pos + 1);
    memcpy(text_input->str + text_input->pos, text, len);
    text_input->pos += len;
    text_input->num += len;
    return true;
}

text_input_history_struct *text_input_history_create(void) {
    text_input_history_struct *tmp;

    tmp = xcalloc(1, sizeof(*tmp));
    utarray_new(tmp->history, &ut_str_icd);

    return tmp;
}

void text_input_history_free(text_input_history_struct *history) {
    utarray_free(history->history);
    free(history);
}

/**
 * Add string to text input history.
 * @param history
 * The history to add to.
 * @param text
 * The text to add to the history.
 */
static void text_input_history_add(text_input_history_struct *history, const char *text) {
    char **p;

    if (!history) {
        return;
    }

    p = (char **)utarray_back(history->history);

    if (p && !strcmp(*p, text)) {
        return;
    }

    utarray_push_back(history->history, &text);

    if (utarray_len(history->history) >
        (size_t)setting_get_int(OPT_CAT_GENERAL, OPT_MAX_INPUT_HISTORY_LINES)) {
        utarray_erase(history->history,
                      0,
                      utarray_len(history->history) -
                          (size_t)setting_get_int(OPT_CAT_GENERAL, OPT_MAX_INPUT_HISTORY_LINES));
    }
}

void text_input_create(text_input_struct *text_input) {
    memset(text_input, 0, sizeof(*text_input));
    text_input->focus = 1;
    text_input->coords.w = 200;
    text_input->max = MIN(sizeof(text_input->str), 256);

    text_input_set_font(text_input, FONT_ARIAL11);
}

/**
 * Destroy data associated with the specified text input. The text input
 * structure itself is not freed.
 * @param text_input
 * Text input to destroy.
 */
void text_input_destroy(text_input_struct *text_input) {
    if (text_input->font != NULL) {
        font_free(text_input->font);
    }
}

void text_input_set_font(text_input_struct *text_input, font_struct *font) {
    if (text_input->font != NULL) {
        font_free(text_input->font);
    }

    FONT_INCREF(font);
    text_input->font = font;
    text_input->coords.h = FONT_HEIGHT(font) + TEXT_INPUT_PADDING * 2;
}

void text_input_reset(text_input_struct *text_input) {
    if (text_input->history) {
        text_input->history->pos = 0;
    }

    text_input_set(text_input, NULL);
}

void text_input_set_history(text_input_struct *text_input, text_input_history_struct *history) {
    text_input->history = history;
}

void text_input_set(text_input_struct *text_input, const char *str) {
    size_t capacity = MIN(text_input->max, sizeof(text_input->str) - 1);

    SDL_utf8strlcpy(text_input->str, str ? str : "", capacity + 1);
    text_input->pos = text_input->num = strlen(text_input->str);
    text_input->composition[0] = '\0';
}

void text_input_set_parent(text_input_struct *text_input, int px, int py) {
    text_input->px = px;
    text_input->py = py;
}

int text_input_mouse_over(text_input_struct *text_input, int mx, int my) {
    mx -= text_input->px;
    my -= text_input->py;

    if (mx >= text_input->coords.x && my >= text_input->coords.y &&
        mx < text_input->coords.x + text_input->coords.w &&
        my < text_input->coords.y + text_input->coords.h) {
        return 1;
    }

    return 0;
}

void text_input_show_edit_password(text_input_struct *text_input) {
    string_replace_char(text_input->str, NULL, '*');
}

int text_input_number_character_check(text_input_struct *text_input, char c) {
    (void)text_input;
    return isdigit((unsigned char)c);
}

/** One visual line, ending before its newline or soft wrap. */
static size_t multiline_line_end(text_input_struct *input, size_t start) {
    size_t pos = start;
    int width = 0;
    int maximum = MAX(1, input->coords.w - TEXT_INPUT_PADDING * 2 -
                             glyph_get_width(input->font, '_'));
    while (pos < input->num && input->str[pos] != '\n') {
        int advance = glyph_get_utf8_width(input->font, input->str + pos);
        if (width + advance > maximum && pos != start) {
            break;
        }
        width += advance;
        pos = utf8_next(input->str, input->num, pos);
    }
    return pos;
}

static size_t multiline_next_line(text_input_struct *input, size_t end) {
    return end < input->num && input->str[end] == '\n' ? end + 1 : end;
}

static void multiline_cursor_line(text_input_struct *input, size_t *start, size_t *row) {
    *start = *row = 0;
    while (*start < input->num) {
        size_t end = multiline_line_end(input, *start);
        /* At a soft wrap the cursor belongs to the following line. */
        if (input->pos < end || (input->pos == end &&
                                (end == input->num || input->str[end] == '\n'))) {
            break;
        }
        *start = multiline_next_line(input, end);
        (*row)++;
    }
}

static int multiline_cursor_width(text_input_struct *input, size_t start) {
    int width = 0;
    for (size_t pos = start; pos < input->pos;
         pos = utf8_next(input->str, input->num, pos)) {
        width += glyph_get_utf8_width(input->font, input->str + pos);
    }
    return width;
}

static void multiline_move(text_input_struct *input, int direction) {
    size_t start, row;
    multiline_cursor_line(input, &start, &row);
    int column = multiline_cursor_width(input, start);
    size_t target = 0;
    if (direction < 0) {
        if (row == 0) {
            return;
        }
        for (size_t i = 0; i + 1 < row; i++) {
            target = multiline_next_line(input, multiline_line_end(input, target));
        }
    } else {
        size_t end = multiline_line_end(input, start);
        if (end == input->num) {
            return;
        }
        target = multiline_next_line(input, end);
    }
    size_t end = multiline_line_end(input, target);
    int width = 0;
    while (target < end) {
        int advance = glyph_get_utf8_width(input->font, input->str + target);
        if (width + advance > column) {
            break;
        }
        width += advance;
        target = utf8_next(input->str, input->num, target);
    }
    input->pos = target;
}

static void multiline_show(text_input_struct *input, SDL_Surface *surface, bool root) {
    size_t cursor_start, cursor_row;
    multiline_cursor_line(input, &cursor_start, &cursor_row);
    int height = FONT_HEIGHT(input->font);
    size_t visible = MAX(1, (input->coords.h - TEXT_INPUT_PADDING * 2) / height);
    if (cursor_row < input->scroll_line) {
        input->scroll_line = cursor_row;
    } else if (cursor_row >= input->scroll_line + visible) {
        input->scroll_line = cursor_row - visible + 1;
    }
    size_t start = 0;
    for (size_t row = 0; row < input->scroll_line; row++) {
        start = multiline_next_line(input, multiline_line_end(input, start));
    }
    for (size_t row = input->scroll_line; row < input->scroll_line + visible; row++) {
        size_t end = multiline_line_end(input, start);
        char line[HUGE_BUF];
        memcpy(line, input->str + start, end - start);
        line[end - start] = '\0';
        int x = input->coords.x + TEXT_INPUT_PADDING;
        int y = input->coords.y + TEXT_INPUT_PADDING + (int)(row - input->scroll_line) * height;
        SDL_Rect box = {.w = input->coords.w - TEXT_INPUT_PADDING * 2, .h = height};
        if (root) {
            text_show_root(input->font, line, x, y, COLOR_WHITE, TEXT_WIDTH, &box);
        } else {
            text_show(surface, input->font, line, x, y, COLOR_WHITE, TEXT_WIDTH, &box);
        }
        if (input->focus && row == cursor_row) {
            int cursor_x = x + multiline_cursor_width(input, cursor_start);
            const char *cursor = input->composition[0] != '\0' ? input->composition : "_";
            box.w = MAX(1, input->coords.x + input->coords.w - cursor_x - TEXT_INPUT_PADDING);
            if (root) {
                text_show_root(input->font, cursor, cursor_x, y, COLOR_WHITE, TEXT_WIDTH, &box);
            } else {
                text_show(surface, input->font, cursor, cursor_x, y, COLOR_WHITE, TEXT_WIDTH, &box);
            }
            SDL_Rect area = {.x = input->px + input->coords.x,
                             .y = input->py + y, .w = input->coords.w, .h = height};
            SDL_SetTextInputArea(ScreenWindow, &area, cursor_x - input->coords.x);
        }
        if (end == input->num) {
            break;
        }
        start = multiline_next_line(input, end);
    }
}

static void text_input_show_impl(text_input_struct *text_input,
                                 SDL_Surface *surface,
                                 bool render_root,
                                 int x,
                                 int y) {
    text_info_struct info;
    int underscore_width;
    size_t pos;
    char *cp, *cp2;
    SDL_Rect box;
    StringBuffer *sb;

    text_input->coords.x = x;
    text_input->coords.y = y;

    rectangle_create(surface,
                     text_input->coords.x,
                     text_input->coords.y,
                     text_input->coords.w,
                     text_input->coords.h,
                     "000000");
    border_create_color(surface, &text_input->coords, 1, "303030");

    if (text_input->multiline) {
        multiline_show(text_input, surface, render_root);
        return;
    }

    cp = NULL;
    box.w = 0;

    if (text_input->show_edit_func) {
        cp = xstrdup(text_input->str);
        text_input->show_edit_func(text_input);
    }

    text_show_character_init(&info);
    underscore_width = glyph_get_width(text_input->font, '_');

    /* Figure out the width by going backwards. */
    for (pos = text_input->pos; pos != 0;) {
        size_t previous = utf8_previous(text_input->str, pos);

        /* Reached the maximum yet? */
        if (box.w + glyph_get_utf8_width(text_input->font, text_input->str + previous) +
                underscore_width >
            text_input->coords.w - TEXT_INPUT_PADDING * 2) {
            break;
        }

        text_show_character(&text_input->font,
                            text_input->font,
                            NULL,
                            false,
                            &box,
                            text_input->str + previous,
                            NULL,
                            NULL,
                            0,
                            NULL,
                            NULL,
                            &info);
        pos = previous;
    }

    if (text_input->focus) {
        SDL_Rect input_area = {
            .x = text_input->px + text_input->coords.x,
            .y = text_input->py + text_input->coords.y,
            .w = text_input->coords.w,
            .h = text_input->coords.h,
        };

        SDL_SetTextInputArea(ScreenWindow, &input_area, TEXT_INPUT_PADDING + box.w);
    }

    sb = stringbuffer_new();
    stringbuffer_append_string_len(sb, text_input->str + pos, text_input->pos - pos);

    if (text_input->focus && text_input->show_edit_func == NULL &&
        text_input->composition[0] != '\0') {
        stringbuffer_append_string(sb, text_input->composition);
        stringbuffer_append_char(sb, '_');
    } else if (text_input->focus) {
        stringbuffer_append_char(sb, '_');
    }

    if (text_input->str[text_input->pos] != '\0') {
        stringbuffer_append_string(sb, (text_input->str + pos) + (text_input->pos - pos));
    }

    box.w = text_input->coords.w - TEXT_INPUT_PADDING * 2;
    box.h = text_input->coords.h - TEXT_INPUT_PADDING * 2;

    cp2 = stringbuffer_finish(sb);
    if (render_root) {
        text_show_root(text_input->font,
                       cp2,
                       text_input->coords.x + TEXT_INPUT_PADDING,
                       text_input->coords.y + TEXT_INPUT_PADDING,
                       COLOR_WHITE,
                       text_input->text_flags | TEXT_WIDTH,
                       &box);
    } else {
        text_show(surface,
                  text_input->font,
                  cp2,
                  text_input->coords.x + TEXT_INPUT_PADDING,
                  text_input->coords.y + TEXT_INPUT_PADDING,
                  COLOR_WHITE,
                  text_input->text_flags | TEXT_WIDTH,
                  &box);
    }
    free(cp2);

    if (cp) {
        text_input_set(text_input, cp);
        free(cp);
    }
}

void text_input_show(text_input_struct *text_input, SDL_Surface *surface, int x, int y) {
    text_input_show_impl(text_input, surface, false, x, y);
}

void text_input_show_root(text_input_struct *text_input, int x, int y) {
    text_input_show_impl(text_input, NULL, true, x, y);
}

void text_input_add_char(text_input_struct *text_input, char c) {
    char text[] = {c, '\0'};
    text_input_add_text(text_input, text);
}

int text_input_event(text_input_struct *text_input, SDL_Event *event) {
    if (!text_input->focus) {
        return 0;
    }

    if (text_input->multiline && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT &&
        text_input_mouse_over(text_input, event->button.x, event->button.y)) {
        int row = MAX(0, ((int)event->button.y - text_input->py - text_input->coords.y -
                         TEXT_INPUT_PADDING) / FONT_HEIGHT(text_input->font));
        size_t start = 0;
        for (size_t i = 0; i < text_input->scroll_line + (size_t)row; i++) {
            size_t end = multiline_line_end(text_input, start);
            if (end == text_input->num) {
                start = end;
                break;
            }
            start = multiline_next_line(text_input, end);
        }
        size_t end = multiline_line_end(text_input, start);
        int target = MAX(0, (int)event->button.x - text_input->px - text_input->coords.x -
                            TEXT_INPUT_PADDING);
        int width = 0;
        while (start < end) {
            int advance = glyph_get_utf8_width(text_input->font, text_input->str + start);
            if (width + advance / 2 >= target) {
                break;
            }
            width += advance;
            start = utf8_next(text_input->str, text_input->num, start);
        }
        text_input->pos = start;
        text_input->composition[0] = '\0';
        return 1;
    }

    if (event->type == SDL_EVENT_TEXT_INPUT) {
        text_input->composition[0] = '\0';
        return text_input_add_text(text_input, event->text.text);
    }
    if (event->type == SDL_EVENT_TEXT_EDITING) {
        SDL_utf8strlcpy(text_input->composition, event->edit.text, sizeof(text_input->composition));
        return 1;
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        if (keybind_command_matches_event("?PASTE", &event->key)) {
            char *clipboard_contents = SDL_GetClipboardText();

            if (clipboard_contents) {
                text_input_sanitize(clipboard_contents, text_input->multiline);
                text_input_add_text(text_input, clipboard_contents);
                SDL_free(clipboard_contents);
            }

            return 1;
        } else if (IS_ENTER(event->key.key)) {
            if (text_input->multiline) {
                text_input_add_char(text_input, '\n');
                return 1;
            }
            if (*text_input->str != '\0') {
                text_input_history_add(text_input->history, text_input->str);
            }

            return 1;
        } else if (event->key.key == SDLK_BACKSPACE) {
            if (text_input->num && text_input->pos) {
                size_t i, j;

                i = j = text_input->pos;

                if (event->key.mod & SDL_KMOD_CTRL) {
                    text_input_skip_word(text_input->str, text_input->num, &i, -1);
                } else {
                    i = utf8_previous(text_input->str, i);
                }

                while (j <= text_input->num) {
                    text_input->str[i++] = text_input->str[j++];
                }

                text_input->pos -= (j - i);
                text_input->num -= (j - i);
            }

            return 1;
        } else if (event->key.key == SDLK_DELETE) {
            if (text_input->pos != text_input->num) {
                size_t i, j;

                i = j = text_input->pos;

                if (event->key.mod & SDL_KMOD_CTRL) {
                    text_input_skip_word(text_input->str, text_input->num, &i, 1);
                } else {
                    i = utf8_next(text_input->str, text_input->num, i);
                }

                while (i <= text_input->num) {
                    text_input->str[j++] = text_input->str[i++];
                }

                text_input->num -= (i - j);
            }

            return 1;
        } else if (event->key.key == SDLK_LEFT) {
            if (event->key.mod & SDL_KMOD_CTRL) {
                size_t i;

                i = text_input->pos;
                text_input_skip_word(text_input->str, text_input->num, &i, -1);
                text_input->pos = i;
            } else if (text_input->pos != 0) {
                text_input->pos = utf8_previous(text_input->str, text_input->pos);
            }

            return 1;
        } else if (event->key.key == SDLK_RIGHT) {
            if (event->key.mod & SDL_KMOD_CTRL) {
                size_t i;

                i = text_input->pos;
                text_input_skip_word(text_input->str, text_input->num, &i, 1);
                text_input->pos = i;
            } else if (text_input->pos < text_input->num) {
                text_input->pos = utf8_next(text_input->str, text_input->num, text_input->pos);
            }

            return 1;
        } else if (event->key.key == SDLK_UP) {
            if (text_input->multiline) {
                multiline_move(text_input, -1);
                return 1;
            }
            if (text_input->history) {
                char **p;

                p = (char **)utarray_eltptr(text_input->history->history,
                                            utarray_len(text_input->history->history) - 1 -
                                                text_input->history->pos);

                if (p) {
                    if (text_input->history->pos == 0) {
                        SDL_utf8strlcpy(text_input->str_editing,
                                        text_input->str,
                                        sizeof(text_input->str_editing));
                    }

                    text_input->history->pos++;
                    text_input_set(text_input, *p);
                }
            }

            return 1;
        } else if (event->key.key == SDLK_DOWN) {
            if (text_input->multiline) {
                multiline_move(text_input, 1);
                return 1;
            }
            if (text_input->history) {
                if (text_input->history->pos > 0) {
                    text_input->history->pos--;

                    if (text_input->history->pos == 0) {
                        text_input_set(text_input, text_input->str_editing);
                        text_input->str_editing[0] = '\0';
                    } else {
                        char **p;

                        p = (char **)utarray_eltptr(text_input->history->history,
                                                    utarray_len(text_input->history->history) -
                                                        text_input->history->pos);

                        if (p) {
                            text_input_set(text_input, *p);
                        }
                    }
                } else if (*text_input->str != '\0') {
                    text_input_history_add(text_input->history, text_input->str);
                    text_input_set(text_input, NULL);
                }
            }

            return 1;
        } else if (event->key.key == SDLK_HOME) {
            if (text_input->multiline && !(event->key.mod & SDL_KMOD_CTRL)) {
                size_t start, row;
                multiline_cursor_line(text_input, &start, &row);
                text_input->pos = start;
            } else {
                text_input->pos = 0;
            }
            return 1;
        } else if (event->key.key == SDLK_END) {
            if (text_input->multiline && !(event->key.mod & SDL_KMOD_CTRL)) {
                size_t start, row;
                multiline_cursor_line(text_input, &start, &row);
                text_input->pos = multiline_line_end(text_input, start);
            } else {
                text_input->pos = text_input->num;
            }
            return 1;
        } else if (event->key.key == SDLK_RSHIFT || event->key.key == SDLK_LSHIFT) {
            return 1;
        }
    }

    return 0;
}
