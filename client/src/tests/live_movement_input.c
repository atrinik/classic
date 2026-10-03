/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <live_movement_input.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                        \
        }                                                                   \
    } while (0)

int main(void) {
    /* Held keys, run/fire toggles, click-to-move and text commands must never
     * acquire the movement stream owned by a live route. */
    const Uint32 gameplay[] = {SDL_EVENT_KEY_DOWN,
                               SDL_EVENT_KEY_UP,
                               SDL_EVENT_MOUSE_BUTTON_DOWN,
                               SDL_EVENT_MOUSE_BUTTON_UP,
                               SDL_EVENT_MOUSE_MOTION,
                               SDL_EVENT_MOUSE_WHEEL,
                               SDL_EVENT_TEXT_INPUT,
                               SDL_EVENT_TEXT_EDITING};
    for (size_t i = 0; i < sizeof(gameplay) / sizeof(gameplay[0]); i++) {
        CHECK(live_movement_input_is_gameplay(gameplay[i]));
    }
    /* Window ownership and shutdown continue through the normal event path. */
    const Uint32 lifecycle[] = {SDL_EVENT_QUIT,
                                SDL_EVENT_WINDOW_RESIZED,
                                SDL_EVENT_WINDOW_EXPOSED,
                                SDL_EVENT_WINDOW_FOCUS_LOST,
                                SDL_EVENT_WINDOW_RESTORED,
                                SDL_EVENT_USER};
    for (size_t i = 0; i < sizeof(lifecycle) / sizeof(lifecycle[0]); i++) {
        CHECK(!live_movement_input_is_gameplay(lifecycle[i]));
    }
    return 0;
}
