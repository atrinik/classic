/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIVE_MOVEMENT_INPUT_H
#define LIVE_MOVEMENT_INPUT_H
#include <stdbool.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

/** A visible diagnostic owns its cadence without acquiring keyboard focus. */
static inline bool live_movement_window_is_active(SDL_WindowFlags flags, bool diagnostic) {
    return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0 &&
           (diagnostic || (flags & SDL_WINDOW_INPUT_FOCUS) != 0);
}

/** Preserve lifecycle events while a dedicated route owns all gameplay input. */
static inline bool live_movement_input_is_gameplay(Uint32 type) {
    switch (type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return true;
        default:
            return false;
    }
}
#endif
