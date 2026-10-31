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

/* Mirror Event_PollInputDevice: Classic marks keys before the pilot filter. */
static bool route_event(SDL_Event *event) {
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
        event->key.scancode > SDL_SCANCODE_UNKNOWN && event->key.scancode < SDL_SCANCODE_COUNT) {
        keys[event->key.scancode].pressed = event->type == SDL_EVENT_KEY_DOWN;
    }
    return shared_chat_pilot_event(event);
}

static int key_pair(SDL_Scancode physical, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event = key_event(SDL_EVENT_KEY_DOWN, physical, key);
    event.key.mod = modifiers;
    CHECK(route_event(&event));
    event.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&event));
    return 0;
}

static int push_key_pair(SDL_Scancode physical, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event = key_event(SDL_EVENT_KEY_DOWN, physical, key);
    event.key.mod = modifiers;
    CHECK(SDL_PushEvent(&event));
    event.type = SDL_EVENT_KEY_UP;
    CHECK(SDL_PushEvent(&event));
    return 0;
}

static int push_text(const char *bytes, bool preedit) {
    SDL_Event event = {0};
    event.type = preedit ? SDL_EVENT_TEXT_EDITING : SDL_EVENT_TEXT_INPUT;
    if (preedit)
        event.edit.text = bytes;
    else
        event.text.text = bytes;
    event.common.timestamp = SDL_GetTicksNS();
    CHECK(SDL_PushEvent(&event));
    return 0;
}

static unsigned int drain(void) {
    SDL_Event event;
    unsigned int count = 0;
    while (shared_chat_pilot_poll(&event)) {
        (void)route_event(&event);
        ++count;
    }
    return count;
}

static int expect_composer(const char *expected) {
    uint8_t bytes[PILOT_BYTES];
    uint64_t count = 0;
    CHECK(twui_copy_text(pilot.ui, TWUI_TEXT_COMPOSER, 0, bytes, sizeof(bytes), &count) == TWUI_OK);
    CHECK(count == strlen(expected) && memcmp(bytes, expected, count) == 0);
    return 0;
}

static void destroy_test_widget(void) {
    widgetdata *widget = pilot.widget;
    destroy(widget);
    cur_widget[LABEL_ID] = NULL;
    free(widget->name);
    free(widget);
}

static int test_ordinary_editing_drain(void) {
    CHECK(key_pair(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_CTRL) == 0);
    CHECK(key_pair(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0) == 0);
    CHECK(expect_composer("") == 0);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    CHECK(push_text("a", false) == 0);
    CHECK(push_key_pair(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0) == 0);
    CHECK(drain() == 3);
    CHECK(expect_composer("") == 0);

    CHECK(push_text("ab", false) == 0);
    SDL_Event home = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_HOME, SDLK_HOME);
    CHECK(SDL_PushEvent(&home));
    CHECK(drain() == 2);
    uint32_t allowed = 0;
    CHECK(twui_key_effect_allowed(pilot.ui, SDL_SCANCODE_HOME, &allowed) == TWUI_OK && allowed);
    /* The text edit invalidates old geometry. Navigation becomes effective only
     * after the same text has been displayed, even though the key is UI-owned. */
    draw(pilot.widget);
    shared_chat_pilot_presented(true);
    home.key.repeat = true;
    CHECK(route_event(&home));
    home.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&home));
    twui_state current;
    CHECK(state(&current) && current.caret == 0);
    CHECK(push_text("c", false) == 0);
    CHECK(push_key_pair(SDL_SCANCODE_DELETE, SDLK_DELETE, 0) == 0);
    CHECK(drain() == 3);
    CHECK(expect_composer("cb") == 0);

    CHECK(push_text("x", false) == 0);
    CHECK(push_key_pair(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_CTRL) == 0);
    CHECK(push_key_pair(SDL_SCANCODE_C, SDLK_C, SDL_KMOD_CTRL) == 0);
    CHECK(drain() >= 5); /* SDL may additionally report clipboard updates. */
    CHECK(expect_composer("cxb") == 0);
    char *copied = SDL_GetClipboardText();
    CHECK(copied != NULL && strcmp(copied, "cxb") == 0);
    SDL_free(copied);
    CHECK(SDL_SetClipboardText("paste"));
    CHECK(push_text("!", false) == 0);
    CHECK(push_key_pair(SDL_SCANCODE_V, SDLK_V, SDL_KMOD_CTRL) == 0);
    CHECK(drain() >= 3);
    CHECK(expect_composer("!paste") == 0);

    /* Actual composition still owns non-Enter editing/helper keys, including
     * platforms that report the composition after its triggering key. */
    CHECK(push_key_pair(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0) == 0);
    CHECK(push_text("preedit", true) == 0);
    SDL_Event left = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LEFT, SDLK_LEFT);
    CHECK(SDL_PushEvent(&left));
    CHECK(drain() == 4);
    CHECK(expect_composer("!paste") == 0);
    CHECK(twui_key_effect_allowed(pilot.ui, SDL_SCANCODE_LEFT, &allowed) == TWUI_OK && !allowed);
    CHECK(push_text("", true) == 0);
    CHECK(drain() == 1);
    left.key.repeat = true;
    CHECK(route_event(&left));
    CHECK(twui_key_effect_allowed(pilot.ui, SDL_SCANCODE_LEFT, &allowed) == TWUI_OK && !allowed);
    left.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&left));
    CHECK(!pilot.native_composing);
    return 0;
}

static int test_retired_keys(void) {
    shared_chat_pilot_create();
    CHECK(pilot.widget != NULL && !pilot.failed);
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    SDL_Event held = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F2, SDLK_F2);
    CHECK(route_event(&held));
    CHECK(key_pair(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0) == 0);
    CHECK(!shared_chat_pilot_text_owner());
    destroy_test_widget();
    held.key.repeat = true;
    CHECK(route_event(&held));
    CHECK(!keys[SDL_SCANCODE_F2].pressed);
    shared_chat_pilot_create();
    CHECK(pilot.widget != NULL && !pilot.failed);
    CHECK(route_event(&held));
    CHECK(!keys[SDL_SCANCODE_F2].pressed);
    held.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&held));
    held.type = SDL_EVENT_KEY_DOWN;
    held.key.repeat = false;
    CHECK(!route_event(&held));
    CHECK(keys[SDL_SCANCODE_F2].pressed);
    held.type = SDL_EVENT_KEY_UP;
    CHECK(!route_event(&held));

    /* A reset during dispatch must finish the copied drain, including the
     * release that ends host quarantine after the widget has disappeared. */
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    held = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F3, SDLK_F3);
    CHECK(route_event(&held));
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    held.key.repeat = true;
    CHECK(SDL_PushEvent(&held));
    held.type = SDL_EVENT_KEY_UP;
    CHECK(SDL_PushEvent(&held));
    CHECK(shared_chat_pilot_poll(&held) && held.type == SDL_EVENT_KEY_DOWN);
    CHECK(route_event(&held));
    destroy_test_widget();
    CHECK(shared_chat_pilot_poll(&held) && held.type == SDL_EVENT_KEY_UP &&
          held.key.scancode == SDL_SCANCODE_F3);
    CHECK(route_event(&held));
    CHECK(!retired_keys[SDL_SCANCODE_F3]);
    held.type = SDL_EVENT_KEY_DOWN;
    held.key.repeat = false;
    CHECK(!route_event(&held));
    held.type = SDL_EVENT_KEY_UP;
    CHECK(!route_event(&held));
    return 0;
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
    CHECK(!route_event(&key));
    unsigned int before = cancelled;
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    CHECK(cancelled > before && !keys[SDL_SCANCODE_W].pressed);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&key));
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
    CHECK(route_event(&key));
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER);
    CHECK(route_event(&key));
    twui_action action = {.struct_size = sizeof(action)};
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&key));
    pilot.batch_ime = false;
    key.type = SDL_EVENT_KEY_DOWN;
    CHECK(route_event(&key));
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY); /* Return remains fenced. */
    key.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&key));
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    CHECK(route_event(&key));
    key = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER);
    CHECK(route_event(&key));
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
    CHECK(!route_event(&popup_key));
    popup_key.type = SDL_EVENT_KEY_UP;
    CHECK(!route_event(&popup_key));
    popup_open = false;
    shared_chat_pilot_sync();
    CHECK(cancelled > before);
    key.type = SDL_EVENT_KEY_UP;
    CHECK(route_event(&key));
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    (void)route_event(&key);
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    /* Focus handoff retires late native text even if it reaches the next owner. */
    classic_input = true;
    shared_chat_pilot_sync();
    CHECK(!shared_chat_pilot_text_owner());
    SDL_Event late = {0};
    late.type = SDL_EVENT_TEXT_INPUT;
    late.text.text = "late";
    late.text.timestamp = pilot.session_start - 1;
    CHECK(route_event(&late));
    classic_input = false;
    shared_chat_pilot_sync();
    CHECK(apply((twui_input){.kind = TWUI_SET_FOCUS, .value = 1}, NULL) == TWUI_OK);
    SDL_Event focus = {0};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    before = cancelled;
    CHECK(!route_event(&focus));
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    CHECK(!route_event(&focus));
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
        (void)route_event(&key);
        ++drained;
    }
    CHECK(drained == 2 && pilot.batch_ime == false);
    CHECK(twui_peek_action(pilot.ui, &action) == TWUI_EMPTY);
    key = key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDLK_RETURN);
    CHECK(route_event(&key));
    CHECK(test_ordinary_editing_drain() == 0);
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
    destroy_test_widget();
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(false);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(true);
    CHECK(capture_privacy_allowed(false));
    CHECK(test_retired_keys() == 0);
    SDL_DestroyWindow(ScreenWindow);
    SDL_Quit();
    return 0;
}
