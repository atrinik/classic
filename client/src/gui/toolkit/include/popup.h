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
 * Header file for popup API.
 */

#ifndef POPUP_H
#define POPUP_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <button.h>
#include <text_input.h>

typedef struct hfile_struct hfile_struct;
typedef struct server_struct server_struct;

/**
 * A single "generic" button in a popup.
 */
typedef struct popup_button {
    /** X position in the popup of the button. */
    int x;

    /** Y position in the popup of the button. */
    int y;

    /** Text to show in the button. */
    char *text;

    /** The button. */
    button_struct button;

    /**
     * Callback function to call when the button is clicked.
     * @param button
     * The clicked button.
     * @retval 1 Handled the event, should not do generic handling.
     * @retval 0 Did not handle the event.
     */
    int (*event_func)(struct popup_button *button);
} popup_button;

/** A single popup. */
typedef struct popup_struct {
    /**
     * Surface the popup uses for immutable content construction. It is then
     * submitted as a retained GPU texture.
     */
    SDL_Surface *surface;

    /**
     * Texture to use.
     */
    texture_struct *texture;

    /**
     * Disable automatically drawing the texture on the popup surface?
     */
    uint8_t disable_texture_drawing;

    /** Custom data. */
    void *custom_data;

    /** Optional character pointer. */
    char *buf;

    /** Optional integers. */
    int64_t i[3];

    /** X position of the popup. */
    int x;

    /** Y position of the popup. */
    int y;

    /** The left button, generally the help button. */
    popup_button button_left;

    /** The right button, generally the close button. */
    popup_button button_right;

    /** Next popup in a doubly-linked list. */
    struct popup_struct *next;

    /** Previous popup in a doubly-linked list. */
    struct popup_struct *prev;

    /** Start of selection. */
    int64_t selection_start;

    /** End of selection. */
    int64_t selection_end;

    /** Whether the selection has started. */
    uint8_t selection_started;

    /** Whether redrawing is in order. */
    uint8_t redraw;

    uint8_t modal;

    uint8_t destroy_on_switch;

    /**
     * Function used for drawing on the popup's surface.
     * @param popup
     * The popup.
     * @return
     * 0 to destroy the popup, 1 otherwise.
     */
    int (*draw_func)(struct popup_struct *popup);

    /**
     * Function used for drawing after drawing the popup's surface on
     * the main surface.
     * @param popup
     * The popup.
     * @return
     * 0 to destroy the popup, 1 otherwise.
     */
    int (*draw_post_func)(struct popup_struct *popup);

    /**
     * Function used for handling mouse/key events when popup is visible.
     * @param event
     * SDL event.
     * @retval -1 Did not handle the event.
     * @retval 0 Did not handle the event, but allow other keyboard
     * events.
     * @retval 1 Handled the event.
     */
    int (*event_func)(struct popup_struct *popup, SDL_Event *event);

    /**
     * Function used right before the visible popup is destroyed using
     * popup_destroy_visible().
     * @param popup
     * The popup.
     * @return
     * 1 to proceed with the destruction of the popup, 0
     * otherwise.
     */
    int (*destroy_callback_func)(struct popup_struct *popup);

    /**
     * Function used to get contents for clipboard copy operation.
     * @param popup
     * Popup.
     * @return
     * Contents to copy.
     */
    const char *(*clipboard_copy_func)(struct popup_struct *popup);
} popup_struct;

/** Public API implemented in src/gui/misc/game_news.c. */

extern void game_news_open(const char *title);

/** Public API implemented in src/gui/misc/intro.c. */

extern void intro_deinit(void);
#ifdef ATRINIK_WIDGET_TESTS
extern void intro_test_begin(void);
#endif

extern void intro_show(void);

extern int intro_event(SDL_Event *event);

/** Public API implemented in src/gui/popups/characters.c. */

/** Open a fully initialized character chooser, or return false on canvas failure. */
extern bool characters_open(void);

extern void socket_command_characters(uint8_t *data, size_t len, size_t pos);

/** Public API implemented in src/gui/popups/credits.c. */

extern void credits_show(void);
#ifdef ATRINIK_WIDGET_TESTS
extern void credits_test_show(const char *message);
#endif

/** Public API implemented in src/gui/popups/help.c. */

extern void hfiles_deinit(void);

extern void hfiles_init(void);

extern hfile_struct *help_find(const char *name);

extern void help_show(const char *name);

extern void help_handle_tabulator(text_input_struct *text_input);

/** Public API implemented in src/gui/popups/join_password.c. */

extern void join_password_open(server_struct *server);

/** Public API implemented in src/gui/popups/login.c. */

extern void login_start(void);
#ifdef ATRINIK_WIDGET_TESTS
extern bool login_test_form_rendered(void);
#endif

/** Public API implemented in src/gui/popups/painting.c. */

void socket_command_painting(uint8_t *data, size_t len, size_t pos);
#ifdef ATRINIK_WIDGET_TESTS
extern bool popup_painting_test_viewport_rendered(void);
#endif

/** Public API implemented in src/gui/popups/server_add.c. */

extern void server_add_open(void);

/** Public API implemented in src/gui/popups/connection_preference.c. */

extern void connection_preference_open(server_struct *server);

/** Release active or pending connection-preference popup state. */
extern void connection_preference_popup_deinit(void);

/** Recreate a pending connection-preference popup after GPU recovery. */
extern bool connection_preference_recover(void);
#ifdef ATRINIK_WIDGET_TESTS
extern bool connection_preference_test_pending(void);
extern bool connection_preference_test_active(void);
#endif

/** Public API implemented in src/gui/toolkit/popup.c. */

/** Create a popup, or return NULL when its retained GPU canvas cannot be created. */
extern popup_struct *popup_create(texture_struct *texture);
#ifdef ATRINIK_WIDGET_TESTS
/** Make the next production popup take its surface-allocation failure path. */
extern void popup_test_surface_allocation_fail_once(void);
#endif

extern void popup_destroy(popup_struct *popup);

extern void popup_destroy_all(void);

extern void popup_render(popup_struct *popup);

extern void popup_render_all(void);

/**
 * Check whether a screen point is covered by a visible popup.
 * @param x
 * Screen X coordinate.
 * @param y
 * Screen Y coordinate.
 * @return
 * 1 if a popup covers the point, 0 otherwise.
 */
extern bool popup_covers_point(int x, int y);

extern int popup_handle_event(SDL_Event *event);

extern popup_struct *popup_get_head(void);

extern void popup_button_set_text(popup_button *button, const char *text);

extern int popup_need_redraw(void);

/** Mark every popup canvas dirty after GPU resource reconstruction. */
extern void popup_redraw_all(void);

#endif
