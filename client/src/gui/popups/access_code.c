/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2 of the License, or    *
 *   (at your option) any later version.                                  *
 ************************************************************************/

/** @file One-attempt server access-code prompt. */

#include <access_attempt.h>
#include <button.h>
#include <client.h>
#include <main.h>
#include <metaserver.h>
#include <popup.h>
#include <text.h>
#include <text_input.h>
#include <textwin.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>
#include <widget.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static button_struct button_connect;
static popup_struct *access_code_popup;
static server_struct *access_code_server;
static server_struct *access_code_existing_server;
static text_input_struct code_input;
static bool identity_confirmation;
static bool connecting;
static bool access_code_server_added;
typedef struct access_resolve_job {
    client_access_attempt_t attempt;
    server_struct *server;
    bool done;
} access_resolve_job_t;
static access_resolve_job_t *resolve_job;
static SDL_Thread *resolve_thread;
static SDL_Mutex *resolve_mutex;

static void access_code_input_clear(void) {
    access_code_clear(code_input.str, sizeof(code_input.str));
    access_code_clear(code_input.str_editing, sizeof(code_input.str_editing));
    access_code_clear(code_input.composition, sizeof(code_input.composition));
    code_input.pos = 0;
    code_input.num = 0;
}

static int resolve_worker(void *data) {
    access_resolve_job_t *job = data;
    server_struct *server = metaserver_access_resolve(job->attempt.code);
    if (server != NULL) {
        server->access_attempt = job->attempt;
        access_code_clear(&job->attempt, sizeof(job->attempt));
    } else {
        client_access_attempt_clear(&job->attempt);
    }
    SDL_LockMutex(resolve_mutex);
    job->server = server;
    job->done = true;
    SDL_UnlockMutex(resolve_mutex);
    return 0;
}

static bool resolve_finished(void) {
    if (resolve_job == NULL || resolve_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(resolve_mutex);
    bool done = resolve_job->done;
    SDL_UnlockMutex(resolve_mutex);
    if (!done) {
        return false;
    }
    SDL_WaitThread(resolve_thread, NULL);
    resolve_thread = NULL;
    SDL_DestroyMutex(resolve_mutex);
    resolve_mutex = NULL;
    server_struct *server = resolve_job->server;
    access_code_clear(resolve_job, sizeof(*resolve_job));
    free(resolve_job);
    resolve_job = NULL;
    if (server == NULL) {
        draw_info(COLOR_RED, "Access unavailable. Check the code and try again.");
        return true;
    }
    if (access_code_existing_server != NULL) {
        if (strcmp(server->server_id, access_code_existing_server->server_id) != 0) {
            metaserver_server_free(server);
            draw_info(COLOR_RED, "The resolved server identity changed; access was not sent.");
            return true;
        }
        free(access_code_existing_server->hostname);
        access_code_existing_server->hostname = server->hostname;
        server->hostname = NULL;
        access_code_existing_server->port = server->port;
        free(access_code_existing_server->rendezvous_origin);
        access_code_existing_server->rendezvous_origin = server->rendezvous_origin;
        server->rendezvous_origin = NULL;
        rendezvous_access_grant_clear(&access_code_existing_server->access_grant);
        access_code_existing_server->access_grant = server->access_grant;
        memset(&server->access_grant, 0, sizeof(server->access_grant));
        access_code_existing_server->access_attempt = server->access_attempt;
        client_access_attempt_clear(&server->access_attempt);
        metaserver_server_free(server);
        access_code_server = access_code_existing_server;
        access_code_server_added = true;
    } else {
        access_code_server = server;
        access_code_server_added = false;
    }
    identity_confirmation = true;
    return true;
}

static int popup_draw(popup_struct *popup) {
    if (resolve_finished()) {
        popup->redraw = 1;
    }
    SDL_Rect box = {0, 0, popup->surface->w, 38};
    text_show(popup->surface,
              FONT_SERIF16,
              "Server access code",
              0,
              0,
              COLOR_HGOLD,
              TEXT_ALIGN_CENTER | TEXT_VALIGN_CENTER,
              &box);

    box.x = 18;
    box.y = 48;
    box.w = popup->surface->w - 36;
    box.h = 50;
    if (resolve_job != NULL) {
        text_show(popup->surface,
                  FONT_ARIAL11,
                  "Resolving the private server through the trusted access service...",
                  box.x,
                  box.y,
                  COLOR_WHITE,
                  TEXT_WORD_WRAP,
                  &box);
    } else if (identity_confirmation) {
        char message[256];
        snprintf(message,
                 sizeof(message),
                 "Confirm the server identity before the code is sent:\n%s\n%s",
                 access_code_server->name,
                 access_code_server->server_id);
        text_show(popup->surface,
                  FONT_ARIAL11,
                  message,
                  box.x,
                  box.y,
                  COLOR_WHITE,
                  TEXT_WORD_WRAP,
                  &box);
    } else {
        text_show(popup->surface,
                  FONT_ARIAL11,
                  "Enter the 16-character access code. It is kept only for this connection "
                  "attempt and is never saved.",
                  box.x,
                  box.y,
                  COLOR_WHITE,
                  TEXT_WORD_WRAP,
                  &box);
    }

    if (!identity_confirmation && resolve_job == NULL) {
        text_show(popup->surface,
                  FONT_ARIAL11,
                  "[b]Access code:[/b]",
                  30,
                  115,
                  COLOR_WHITE,
                  TEXT_MARKUP,
                  NULL);
        text_input_set_parent(&code_input, popup->x, popup->y);
        text_input_show(&code_input, popup->surface, 135, 115);
    }

    button_set_parent(&button_connect, popup->x, popup->y);
    button_connect.x = 190;
    button_connect.y = 170;
    button_connect.surface = popup->surface;
    button_show(&button_connect,
                resolve_job != NULL ? "Please wait"
                                    : (identity_confirmation ? "Confirm" : "Connect"));
    return 1;
}

static int popup_event(popup_struct *popup, SDL_Event *event) {
    if (button_event(&button_connect, event) ||
        (event->type == SDL_EVENT_KEY_DOWN && IS_ENTER(event->key.key))) {
        if (resolve_job != NULL) {
            return 1;
        }
        if (identity_confirmation) {
            if (!access_code_server_added) {
                metaserver_server_add(access_code_server);
                access_code_server_added = true;
            }
            selected_server = access_code_server;
            connecting = true;
            popup_destroy(popup);
            login_start();
            return 1;
        }

        client_access_attempt_t attempt = {0};
        if (!client_access_attempt_set(&attempt, code_input.str, strlen(code_input.str))) {
            draw_info(COLOR_RED, "Enter a valid 16-character access code.");
            return 1;
        }

        if (access_code_server == NULL) {
            resolve_mutex = SDL_CreateMutex();
            resolve_job = xcalloc(1, sizeof(*resolve_job));
            resolve_job->attempt = attempt;
            access_code_clear(&attempt, sizeof(attempt));
            resolve_thread = resolve_mutex != NULL
                                 ? SDL_CreateThread(resolve_worker, "access-resolve", resolve_job)
                                 : NULL;
            if (resolve_thread == NULL) {
                client_access_attempt_clear(&resolve_job->attempt);
                free(resolve_job);
                resolve_job = NULL;
                if (resolve_mutex != NULL) {
                    SDL_DestroyMutex(resolve_mutex);
                    resolve_mutex = NULL;
                }
                draw_info(COLOR_RED, "Could not start private server resolution.");
                return 1;
            }
            access_code_input_clear();
            popup->redraw = 1;
            return 1;
        }

        access_code_server->access_attempt = attempt;
        access_code_clear(&attempt, sizeof(attempt));

        connecting = true;
        popup_destroy(popup);
        login_start();
        return 1;
    }

    if (!identity_confirmation && resolve_job == NULL && text_input_event(&code_input, event)) {
        return 1;
    }
    if (!identity_confirmation && resolve_job == NULL &&
        event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT &&
        text_input_mouse_over(&code_input, event->button.x, event->button.y)) {
        code_input.focus = 1;
        return 1;
    }
    return -1;
}

static int popup_destroy_callback(popup_struct *popup) {
    (void)popup;
    if (resolve_thread != NULL) {
        SDL_WaitThread(resolve_thread, NULL);
        resolve_thread = NULL;
    }
    if (resolve_mutex != NULL) {
        SDL_DestroyMutex(resolve_mutex);
        resolve_mutex = NULL;
    }
    if (resolve_job != NULL) {
        if (resolve_job->server != NULL) {
            metaserver_server_free(resolve_job->server);
        }
        client_access_attempt_clear(&resolve_job->attempt);
        free(resolve_job);
        resolve_job = NULL;
    }
    text_input_destroy(&code_input);
    access_code_clear(&code_input, sizeof(code_input));
    button_destroy(&button_connect);
    if (!connecting && access_code_server != NULL) {
        if (access_code_server_added) {
            client_access_attempt_clear(&access_code_server->access_attempt);
            if (access_code_server->private_access) {
                rendezvous_access_grant_clear(&access_code_server->access_grant);
            }
        } else {
            metaserver_server_free(access_code_server);
        }
    }
    access_code_popup = NULL;
    access_code_server = NULL;
    access_code_existing_server = NULL;
    access_code_server_added = false;
    identity_confirmation = false;
    connecting = false;
    return 1;
}

void access_code_open(server_struct *server) {
    access_code_existing_server = server != NULL && server->private_access ? server : NULL;
    access_code_server = access_code_existing_server == NULL ? server : NULL;
    access_code_server_added = server != NULL;
    identity_confirmation = false;
    connecting = false;
    access_code_popup = popup_create(texture_get(TEXTURE_TYPE_CLIENT, "popup"));
    if (access_code_popup == NULL) {
        access_code_server = NULL;
        return;
    }
    access_code_popup->draw_func = popup_draw;
    access_code_popup->event_func = popup_event;
    access_code_popup->destroy_callback_func = popup_destroy_callback;

    text_input_create(&code_input);
    code_input.coords.w = 205;
    code_input.max = ACCESS_CODE_LENGTH;
    code_input.show_edit_func = text_input_show_edit_password;
    code_input.focus = 1;
    button_create(&button_connect);
}
