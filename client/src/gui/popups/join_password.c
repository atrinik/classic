/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2 of the License, or    *
 *   (at your option) any later version.                                  *
 ************************************************************************/

/** @file Session-only server join-password prompt. */

#include <openssl/crypto.h>
#include <button.h>
#include <client.h>
#include <main.h>
#include <popup.h>
#include <text.h>
#include <text_input.h>
#include <textwin.h>
#include <join_credentials.h>
#include <toolkit/toolkit.h>
#include <widget.h>
#include <toolkit/path.h>
#include <toolkit/rendezvous.h>
#include <toolkit/string.h>

static button_struct button_connect;
static popup_struct *join_password_popup;
static server_struct *join_password_server;
static text_input_struct password_input;
static text_input_struct invite_input;

static bool invite_required(void) {
    return join_password_server != NULL &&
           (join_password_server->hostname == NULL || join_password_server->port == 0);
}

static bool invite_load_file(void) {
    if (clioption_settings.rendezvous_invite_file == NULL) {
        return false;
    }
    char text[RENDEZVOUS_INVITE_TEXT_SIZE];
    bool permissive_mode = false;
    path_secret_error_t error =
        path_read_secret(clioption_settings.rendezvous_invite_file, VS(text), &permissive_mode);
    bool ok = error == PATH_SECRET_OK && !permissive_mode;
    if (ok) {
        text_input_set(&invite_input, text);
    } else {
        draw_info_format(COLOR_RED,
                         "Cannot use rendezvous invite file %s: %s%s",
                         clioption_settings.rendezvous_invite_file,
                         path_secret_error_string(error),
                         permissive_mode ? " (mode 0600 is required)" : "");
    }
    OPENSSL_cleanse(text, sizeof(text));
    return ok;
}

static bool invite_store(void) {
    rendezvous_invite_t parsed;
    if (join_password_server == NULL || !rendezvous_invite_parse(invite_input.str, &parsed) ||
        !rendezvous_invite_valid_at(&parsed,
                                    join_password_server->server_id,
                                    (uint64_t)time(NULL))) {
        rendezvous_invite_cleanse(&parsed);
        draw_info(COLOR_RED, "The rendezvous invite is invalid, expired, or for another server.");
        return false;
    }
    if (join_password_server->rendezvous_invite == NULL) {
        join_password_server->rendezvous_invite = xmalloc(sizeof(parsed));
    } else {
        rendezvous_invite_cleanse(join_password_server->rendezvous_invite);
    }
    *join_password_server->rendezvous_invite = parsed;
    rendezvous_invite_cleanse(&parsed);
    return true;
}

static int popup_draw(popup_struct *popup) {
    SDL_Rect box = {0, 0, popup->surface->w, 38};
    text_show(popup->surface,
              FONT_SERIF16,
              "Server password",
              0,
              0,
              COLOR_HGOLD,
              TEXT_ALIGN_CENTER | TEXT_VALIGN_CENTER,
              &box);

    box.x = 18;
    box.y = 48;
    box.w = popup->surface->w - 36;
    box.h = 45;
    bool needs_invite = invite_required();
    text_show(popup->surface,
              FONT_ARIAL11,
              needs_invite
                  ? "Paste the server invite, then enter its in-game join password. "
                    "Both are kept only for this connection."
                  : "This public endpoint requires an in-game join password. It will be kept "
                    "only for this connection.",
              box.x,
              box.y,
              COLOR_WHITE,
              TEXT_WORD_WRAP,
              &box);

    if (needs_invite) {
        text_show(popup->surface,
                  FONT_ARIAL11,
                  "[b]Invite (paste only):[/b]",
                  18,
                  91,
                  COLOR_WHITE,
                  TEXT_MARKUP,
                  NULL);
        text_input_set_parent(&invite_input, popup->x, popup->y);
        text_input_show(&invite_input, popup->surface, 150, 91);
    }

    text_show(popup->surface,
              FONT_ARIAL11,
              "[b]Join password:[/b]",
              30,
              needs_invite ? 130 : 110,
              COLOR_WHITE,
              TEXT_MARKUP,
              NULL);
    text_input_set_parent(&password_input, popup->x, popup->y);
    text_input_show(&password_input, popup->surface, 130, needs_invite ? 130 : 110);

    button_set_parent(&button_connect, popup->x, popup->y);
    button_connect.x = 190;
    button_connect.y = 170;
    button_connect.surface = popup->surface;
    button_show(&button_connect, "Connect");
    return 1;
}

static int popup_event(popup_struct *popup, SDL_Event *event) {
    if (button_event(&button_connect, event) ||
        (event->type == SDL_EVENT_KEY_DOWN && IS_ENTER(event->key.key))) {
        if (join_password_server == NULL ||
            client_join_password_missing(password_input.str,
                                         clioption_settings.join_password,
                                         join_password_server->join_password) ||
            (invite_required() && !invite_store())) {
            return -1;
        }

        if (password_input.str[0] != '\0') {
            client_join_credentials_clear(&join_password_server->join_password, NULL);
            join_password_server->join_password = xstrdup(password_input.str);
        }
        popup_destroy(popup);
        login_start();
        return 1;
    }

    if (invite_required() && invite_input.focus &&
        (event->type == SDL_EVENT_TEXT_INPUT || event->type == SDL_EVENT_TEXT_EDITING)) {
        return 1;
    }
    if ((invite_required() && text_input_event(&invite_input, event)) ||
        text_input_event(&password_input, event)) {
        return 1;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        if (invite_required() &&
            text_input_mouse_over(&invite_input, event->button.x, event->button.y)) {
            invite_input.focus = 1;
            password_input.focus = 0;
            return 1;
        }
        if (text_input_mouse_over(&password_input, event->button.x, event->button.y)) {
            password_input.focus = 1;
            invite_input.focus = 0;
            return 1;
        }
    }
    return -1;
}

static int popup_destroy_callback(popup_struct *popup) {
    (void)popup;
    OPENSSL_cleanse(password_input.str, sizeof(password_input.str));
    OPENSSL_cleanse(invite_input.str, sizeof(invite_input.str));
    text_input_destroy(&password_input);
    text_input_destroy(&invite_input);
    button_destroy(&button_connect);
    join_password_popup = NULL;
    join_password_server = NULL;
    return 1;
}

void join_password_open(server_struct *server) {
    HARD_ASSERT(server != NULL);

    join_password_server = server;
    join_password_popup = popup_create(texture_get(TEXTURE_TYPE_CLIENT, "popup"));
    if (join_password_popup == NULL) {
        join_password_server = NULL;
        return;
    }
    join_password_popup->draw_func = popup_draw;
    join_password_popup->event_func = popup_event;
    join_password_popup->destroy_callback_func = popup_destroy_callback;

    text_input_create(&password_input);
    text_input_create(&invite_input);
    password_input.coords.w = 210;
    password_input.max = MAX_BUF - 1;
    password_input.show_edit_func = text_input_show_edit_password;
    password_input.focus = 0;
    invite_input.coords.w = 190;
    invite_input.max = RENDEZVOUS_INVITE_TEXT_SIZE - 1U;
    invite_input.show_edit_func = text_input_show_edit_password;
    invite_input.focus = invite_required();
    password_input.focus = !invite_input.focus;
    if (invite_required()) {
        invite_load_file();
    }
    button_create(&button_connect);
}
