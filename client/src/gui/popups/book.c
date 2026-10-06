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
 * Book GUI related code.
 *
 * @author Zoey Rose
 */

#include <SDL3/SDL.h>
#include <book.h>
#include <capture_privacy.h>
#include <event.h>
#include <main.h>
#include <popup.h>
#include <scrollbar.h>
#include <text.h>
#include <texture.h>
#include <sprite.h>
#include <toolkit/toolkit.h>
#include <toolkit/access_code.h>
#include <widget.h>
#include <toolkit/string.h>

/** The book's content. */
static char *book_content = NULL;
static char book_signer[128];
static char book_signed_date[128];
static bool book_sensitive;
#ifdef ATRINIK_WIDGET_TESTS
static bool book_signature_rendered;
static bool book_test_clear_observed;
#endif
/** Name of the book. */
static char book_name[HUGE_BUF];
/** Number of lines in the book. */
static uint32_t book_lines = 0;
/** Number of lines at the end. */
static uint32_t book_scroll_lines = 0;
/** Lines scrolled. */
static uint32_t book_scroll = 0;
/** Help history - used for the 'Back' button. */
UT_array *book_help_history = NULL;
/** Whether the help history is enabled for this book GUI. */
static uint8_t book_help_history_enabled = 0;
/** Scrollbar in the book GUI. */
static scrollbar_struct scrollbar;

static popup_struct *book_popup_get(void) {
    popup_struct *popup;

    for (popup = popup_get_head(); popup != NULL; popup = popup->next) {
        if (popup->texture == texture_get(TEXTURE_TYPE_CLIENT, "book")) {
            return popup;
        }
    }

    return NULL;
}

static void book_state_clear(void) {
    if (book_help_history != NULL) {
        utarray_free(book_help_history);
        book_help_history = NULL;
    }
    book_help_history_enabled = 0;
    if (book_sensitive && book_content != NULL) {
        size_t content_length = strlen(book_content);
        access_code_clear(book_content, content_length);
#ifdef ATRINIK_WIDGET_TESTS
        book_test_clear_observed = true;
        for (size_t i = 0; i < content_length; i++) {
            if (book_content[i] != '\0') {
                book_test_clear_observed = false;
                break;
            }
        }
#endif
    }
    free(book_content);
    book_content = NULL;
    book_sensitive = false;
    book_lines = 0;
    book_scroll_lines = 0;
    book_scroll = 0;
    book_signer[0] = book_signed_date[0] = '\0';
#ifdef ATRINIK_WIDGET_TESTS
    book_signature_rendered = false;
#endif
}

/**
 * Change the book's displayed name.
 * @param name
 * The name to change to.
 * @param len
 * Length of the name.
 */
void book_name_change(const char *name, size_t len) {
    len = MIN(sizeof(book_name) - 1, len);
    size_t copied = 0;
    while (copied < len && name[copied] != '\0') {
        book_name[copied] = name[copied];
        copied++;
    }
    book_name[copied] = '\0';
}

/** @copydoc popup_struct::draw_func */
static int popup_draw_func(popup_struct *popup) {
    if (popup->redraw) {
        SDL_Rect box;

        surface_show(popup->surface, 0, 0, NULL, texture_surface(popup->texture));

        /* Draw the book name. */
        box.w = BOOK_TITLE_WIDTH;
        box.h = BOOK_TITLE_HEIGHT;
        text_show(popup->surface,
                  FONT_SERIF16,
                  book_name,
                  BOOK_TITLE_STARTX,
                  BOOK_TITLE_STARTY,
                  COLOR_HGOLD,
                  TEXT_WORD_WRAP | (book_sensitive ? 0 : TEXT_MARKUP) | TEXT_ALIGN_CENTER,
                  &box);

        /* Sensitive results contain server-controlled labels: render literal text. */
        box.w = BOOK_TEXT_WIDTH;
        box.h = BOOK_TEXT_HEIGHT - (book_signer[0] ? 36 : 0);
        box.y = book_scroll;
        text_color_set(0, 0, 255);
        text_set_selection(&popup->selection_start,
                           &popup->selection_end,
                           &popup->selection_started);
        text_show(popup->surface,
                  FONT_ARIAL11,
                  book_content,
                  BOOK_TEXT_STARTX,
                  BOOK_TEXT_STARTY,
                  COLOR_BLACK,
                  TEXT_WORD_WRAP | (book_sensitive ? 0 : TEXT_MARKUP) | TEXT_LINES_SKIP,
                  &box);
        text_set_selection(NULL, NULL, NULL);

        if (book_signer[0]) {
#ifdef ATRINIK_WIDGET_TESTS
            book_signature_rendered = true;
#endif
            char signature[300];
            snprintf(signature, sizeof(signature), "Signed by %s on %s", book_signer, book_signed_date);
            box.w = BOOK_TEXT_WIDTH;
            box.h = 32;
            text_show(popup->surface, FONT_ARIAL11, signature, BOOK_TEXT_STARTX,
                      BOOK_TEXT_STARTY + BOOK_TEXT_HEIGHT - 32, COLOR_BLACK,
                      TEXT_WORD_WRAP, &box);
        }

        popup->redraw = 0;
    }

    return 1;
}

/** @copydoc popup_struct::draw_post_func */
static int popup_draw_post_func(popup_struct *popup) {
    scrollbar_show(&scrollbar,
                   OfflineRenderSurface,
                   popup->x + BOOK_SCROLLBAR_STARTX,
                   popup->y + BOOK_SCROLLBAR_STARTY);
    surface_show(OfflineRenderSurface, popup->x, popup->y, NULL, TEXTURE_CLIENT("book_border"));

    return 1;
}

/** @copydoc popup_button::event_func */
static int popup_button_event_func(popup_button *button) {
    size_t len;

    (void)button;

    len = utarray_len(book_help_history);

    if (len >= 2) {
        size_t pos;
        char **p;

        pos = len - 2;
        p = (char **)utarray_eltptr(book_help_history, pos);

        if (p) {
            help_show(*p);
            utarray_erase(book_help_history, pos, 2);
        }
    } else {
        utarray_clear(book_help_history);
        help_show("main");
    }

    return 1;
}

/** @copydoc popup_struct::event_func */
static int popup_event_func(popup_struct *popup, SDL_Event *event) {
    if (scrollbar_event(&scrollbar, event)) {
        return 1;
    }

    if (book_help_history_enabled && BUTTON_CHECK_TOOLTIP(&popup->button_left.button)) {
        tooltip_create(event_mouse_x(event), event_mouse_y(event), FONT_ARIAL11, "Go back");
        tooltip_enable_delay(300);
    }

    /* Mouse event and the mouse is inside the book. */
    if (event->type == SDL_EVENT_MOUSE_WHEEL && event->wheel.mouse_x >= popup->x &&
        event->wheel.mouse_x < popup->x + popup->surface->w && event->wheel.mouse_y >= popup->y &&
        event->wheel.mouse_y < popup->y + popup->surface->h) {
        /* Scroll the book. */
        if (event_wheel_y(event) < 0.0f) {
            scrollbar_scroll_adjust(&scrollbar, 1);
            return 1;
        } else if (event_wheel_y(event) > 0.0f) {
            scrollbar_scroll_adjust(&scrollbar, -1);
            return 1;
        }
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        /* Scrolling. */
        if (event->key.key == SDLK_DOWN) {
            scrollbar_scroll_adjust(&scrollbar, 1);
            return 1;
        } else if (event->key.key == SDLK_UP) {
            scrollbar_scroll_adjust(&scrollbar, -1);
            return 1;
        } else if (event->key.key == SDLK_PAGEDOWN) {
            scrollbar_scroll_adjust(&scrollbar, book_scroll_lines);
            return 1;
        } else if (event->key.key == SDLK_PAGEUP) {
            scrollbar_scroll_adjust(&scrollbar, -book_scroll_lines);
            return 1;
        }
    }

    return -1;
}

/** @copydoc popup_struct::destroy_callback_func */
static int popup_destroy_callback(popup_struct *popup) {
    (void)popup;
    book_state_clear();
    return 1;
}

/** @copydoc popup_struct::clipboard_copy_func */
static const char *popup_clipboard_copy_func(popup_struct *popup) {
    (void)popup;
    return book_content;
}

/**
 * Load the book interface.
 * @param data
 * Book's content.
 * @param len
 * Length of 'data'.
 */
static bool book_load_internal(const char *data, int len, bool sensitive, const char *title) {
    SDL_Rect box;
    int pos;
    popup_struct *popup;

    if (data == NULL || len <= 0) {
        popup = book_popup_get();
        if (popup != NULL) {
            popup_destroy(popup);
        } else {
            book_state_clear();
        }
        return true;
    }

    /* Free old book data and reset the values. */
    if (book_sensitive && book_content != NULL) {
        access_code_clear(book_content, strlen(book_content));
    }
    free(book_content);

    book_lines = 0;
    book_scroll_lines = 0;
    book_scroll = 0;

    /* Store the data. */
    book_content = xstrdup(data);
    book_sensitive = sensitive;
    if (sensitive) {
        capture_privacy_block();
    }
    book_name_change(title, strlen(title));

    /* Strip trailing newlines. */
    for (pos = len - 1; pos >= 0; pos--) {
        if (book_content[pos] != '\n') {
            break;
        }

        book_content[pos] = '\0';
    }

    /* No data... */
    if (book_content[0] == '\0') {
        popup = book_popup_get();

        if (popup != NULL) {
            popup_destroy(popup);
        } else {
            book_state_clear();
        }
        return true;
    }

    /* Calculate the line numbers. */
    box.w = BOOK_TEXT_WIDTH;
    box.h = BOOK_TEXT_HEIGHT - (book_signer[0] ? 36 : 0);
    text_show(NULL,
              FONT_ARIAL11,
              book_content,
              BOOK_TEXT_STARTX,
              BOOK_TEXT_STARTY,
              COLOR_WHITE,
              TEXT_WORD_WRAP | (book_sensitive ? 0 : TEXT_MARKUP) | TEXT_LINES_CALC,
              &box);
    book_lines = box.h;
    book_scroll_lines = box.y;

    /* Create the book popup if it doesn't exist yet. */
    popup = book_popup_get();
    if (popup == NULL) {
        popup = popup_create(texture_get(TEXTURE_TYPE_CLIENT, "book"));
        if (popup == NULL) {
            book_state_clear();
            return false;
        }
        popup->draw_func = popup_draw_func;
        popup->draw_post_func = popup_draw_post_func;
        popup->event_func = popup_event_func;
        popup->destroy_callback_func = popup_destroy_callback;
        popup->clipboard_copy_func = popup_clipboard_copy_func;
        popup->disable_texture_drawing = 1;

        popup->button_left.x = 25;
        popup->button_left.y = 25;

        if (book_help_history_enabled) {
            popup->button_left.event_func = popup_button_event_func;
            popup_button_set_text(&popup->button_left, "<");
        }

        popup->button_right.x = 649;
        popup->button_right.y = 25;
    }

    scrollbar_create(&scrollbar,
                     BOOK_SCROLLBAR_WIDTH,
                     BOOK_SCROLLBAR_HEIGHT,
                     &book_scroll,
                     &book_lines,
                     book_scroll_lines);
    scrollbar.redraw = &popup->redraw;

    popup->redraw = 1;
    return true;
}

bool book_load(const char *data, int len) {
    book_signer[0] = book_signed_date[0] = '\0';
#ifdef ATRINIK_WIDGET_TESTS
    book_signature_rendered = false;
#endif
    return book_load_internal(data, len, false, "Book");
}

bool book_load_signed(const char *data, int len, const char *signer, const char *date) {
#ifdef ATRINIK_WIDGET_TESTS
    book_signature_rendered = false;
#endif
    SDL_utf8strlcpy(book_signer, signer, sizeof(book_signer));
    SDL_utf8strlcpy(book_signed_date, date, sizeof(book_signed_date));
    return book_load_internal(data, len, false, "Book");
}

bool book_load_sensitive(const char *data, int len, const char *title) {
    if (data == NULL || title == NULL) {
        return false;
    }
    book_signer[0] = book_signed_date[0] = '\0';
#ifdef ATRINIK_WIDGET_TESTS
    book_signature_rendered = false;
#endif
    return book_load_internal(data, len, true, title);
}

void book_sensitive_clear(void) {
    if (!book_sensitive) {
        return;
    }

    popup_struct *popup = book_popup_get();
    if (popup != NULL) {
        popup_destroy(popup);
    } else {
        book_state_clear();
    }
}

bool book_sensitive_active(void) {
    return book_sensitive;
}

bool book_sensitive_visible(void) {
    return book_sensitive && book_popup_get() != NULL;
}

#ifdef ATRINIK_WIDGET_TESTS
bool book_test_content_retained(void) {
    return book_content != NULL;
}
bool book_test_signature_rendered(void) {
    return book_signature_rendered;
}

bool book_test_state_seed(const char *content, bool sensitive) {
    if (content == NULL || *content == '\0') {
        return false;
    }
    book_state_clear();
    book_content = xstrdup(content);
    book_sensitive = sensitive;
    if (sensitive) {
        capture_privacy_block();
    }
    book_test_clear_observed = false;
    return true;
}

bool book_test_clear_was_observed(void) {
    return book_test_clear_observed;
}

void book_test_state_discard(void) {
    book_state_clear();
}
#endif

/**
 * Redraw the book GUI.
 */
void book_redraw(void) {
    popup_struct *popup = book_popup_get();

    if (popup != NULL) {
        popup->redraw = 1;
    }
}

/**
 * Enable book help history.
 */
void book_add_help_history(const char *name) {
    if (!book_help_history_enabled) {
        book_help_history_enabled = 1;
        utarray_new(book_help_history, &ut_str_icd);
    }

    utarray_push_back(book_help_history, &name);
}
