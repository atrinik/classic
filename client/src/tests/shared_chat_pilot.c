/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define ATRINIK_SHARED_CHAT_PILOT_TEST
#include "../client/shared_chat_pilot.c"
#define CHECK(test)                                            \
    do {                                                       \
        if (!(test)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #test); \
            return 1;                                          \
        }                                                      \
    } while (0)

static SDL_Event key_event(SDL_EventType type, SDL_Scancode physical, SDL_Keycode key) {
    SDL_Event e = {0};
    e.type = type;
    e.key.scancode = physical;
    e.key.key = key;
    return e;
}

int main(void) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    ScreenWindow = SDL_CreateWindow("shared chat adapter test", 800, 600, 0);
    CHECK(ScreenWindow != NULL);
    CHECK(SDL_setenv_unsafe("ATRINIK_SHARED_CHAT_PILOT", "0", 1) == 0);
    shared_chat_pilot_create();
    CHECK(pilot.widget == NULL);
    CHECK(SDL_setenv_unsafe("ATRINIK_SHARED_CHAT_PILOT", "1", 1) == 0);
    shared_chat_pilot_create();
    CHECK(pilot.widget && !pilot.failed);
    CHECK(!capture_privacy_allowed(false));
    SDL_Event key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W, SDLK_W);
    keys[SDL_SCANCODE_W].pressed = 1;
    CHECK(!shared_chat_pilot_event(&key));
    unsigned int before = cancelled;
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    CHECK(cancelled > before && !keys[SDL_SCANCODE_W].pressed);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(shared_chat_pilot_event(&key));
    CHECK(text(TWUI_COMMIT, "private /local", 14, 0, 0) == TWUI_OK);
    draw(pilot.widget);
    CHECK(pilot.pending && drawn && !pilot.failed);
    twui_stamp stamp = {.struct_size = sizeof(stamp)};
    CHECK(twui_get_displayed(pilot.ui, &stamp) == TWUI_EMPTY);
    shared_chat_pilot_presented(false);
    CHECK(!pilot.pending);
    CHECK(twui_get_displayed(pilot.ui, &stamp) == TWUI_EMPTY);
    draw(pilot.widget);
    shared_chat_pilot_presented(true);
    CHECK(twui_get_displayed(pilot.ui, &stamp) == TWUI_OK);
    pilot.widget->x++;
    shared_chat_pilot_sync();
    CHECK(twui_get_displayed(pilot.ui, &stamp) == TWUI_EMPTY);
    draw(pilot.widget);
    shared_chat_pilot_presented(true);
    /* Same-drain text/IME fences both physical Enter keys independently. */
    pilot.batch_ime = true;
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDLK_RETURN);
    CHECK(shared_chat_pilot_event(&key));
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER);
    CHECK(shared_chat_pilot_event(&key));
    twui_action action = {.struct_size = sizeof(action)};
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(shared_chat_pilot_event(&key));
    pilot.batch_ime = false;
    key.type = SDL_EVENT_KEY_DOWN;
    CHECK(shared_chat_pilot_event(&key));
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY); /* Return remains fenced. */
    key.type = SDL_EVENT_KEY_UP;
    CHECK(shared_chat_pilot_event(&key));
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    CHECK(shared_chat_pilot_event(&key));
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER);
    CHECK(shared_chat_pilot_event(&key));
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_OK);
    accept_actions();
    uint64_t accepted = pilot.accepted;
    accept_actions();
    CHECK(accepted && accepted == pilot.accepted);
    twui_state current;
    CHECK(state(&current) && current.history_messages == 2);
    /* Modal enter+exit retains its epoch barrier even if final state allows world. */
    before = cancelled;
    popup_open = true;
    shared_chat_pilot_sync();
    SDL_Event popup_key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDLK_A);
    CHECK(!shared_chat_pilot_event(&popup_key));
    popup_key.type = SDL_EVENT_KEY_UP;
    CHECK(!shared_chat_pilot_event(&popup_key));
    popup_open = false;
    shared_chat_pilot_sync();
    CHECK(cancelled > before);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(shared_chat_pilot_event(&key));
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    (void)shared_chat_pilot_event(&key);
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    /* Focus handoff retires late native text even if it reaches the next owner. */
    classic_input = true;
    shared_chat_pilot_sync();
    CHECK(!shared_chat_pilot_text_owner());
    SDL_Event late = {0};
    late.type = SDL_EVENT_TEXT_INPUT;
    late.text.text = "late";
    late.text.timestamp = pilot.session_start - 1;
    CHECK(shared_chat_pilot_event(&late));
    classic_input = false;
    shared_chat_pilot_sync();
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    SDL_Event focus = {0};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    before = cancelled;
    CHECK(!shared_chat_pilot_event(&focus));
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    CHECK(!shared_chat_pilot_event(&focus));
    CHECK(cancelled > before && !shared_chat_pilot_text_owner());
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    /* Actual copied drain sees text even when Enter precedes the commit. */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDLK_RETURN);
    SDL_Event commit = {0};
    commit.type = SDL_EVENT_TEXT_INPUT;
    commit.text.text = "a";
    commit.text.timestamp = SDL_GetTicksNS();
    CHECK(SDL_PushEvent(&key));
    CHECK(SDL_PushEvent(&commit));
    unsigned int drained = 0;
    while (shared_chat_pilot_poll(&key)) {
        (void)shared_chat_pilot_event(&key);
        ++drained;
    }
    CHECK(drained == 2 && pilot.batch_ime == false);
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY);
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    CHECK(shared_chat_pilot_event(&key));
    /* Full output queue retains draft; cancellation remains available. */
    for (unsigned int i = 0; i < 8; ++i) {
        CHECK(text(TWUI_COMMIT, "x", 1, 0, 0) == TWUI_OK);
        CHECK(apply((twui_input){.kind = TWUI_SUBMIT}, NULL) == TWUI_OK);
    }
    CHECK(text(TWUI_COMMIT, "retained", 8, 0, 0) == TWUI_OK);
    CHECK(apply((twui_input){.kind = TWUI_SUBMIT}, NULL) == TWUI_QUEUE_FULL);
    CHECK(state(&current) && current.composer_bytes == 8);
    draw(pilot.widget);
    shared_chat_pilot_presented(true);
    drawing_ok = false;
    draw(pilot.widget);
    CHECK(!pilot.pending);
    CHECK(twui_get_displayed(pilot.ui, &stamp) == TWUI_EMPTY);
    drawing_ok = true;
    pilot.widget->show = 0;
    shared_chat_pilot_sync();
    CHECK(state(&current) && current.suspended && !current.focused);
    CHECK(!capture_privacy_allowed(false));
    widgetdata *widget = pilot.widget;
    destroy(widget);
    free(widget->name);
    free(widget);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(false);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(true);
    CHECK(capture_privacy_allowed(false));
    SDL_DestroyWindow(ScreenWindow);
    SDL_Quit();
    return 0;
}
