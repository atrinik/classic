/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHARED_CHAT_PILOT_H
#define SHARED_CHAT_PILOT_H
#include <stdbool.h>
#include <SDL3/SDL.h>
#ifdef ATRINIK_SHARED_CHAT_PILOT
void shared_chat_pilot_create(void);
void shared_chat_pilot_sync(void);
void shared_chat_pilot_invalidate(void);
void shared_chat_pilot_presented(bool presented);
bool shared_chat_pilot_event(SDL_Event *event);
bool shared_chat_pilot_poll(SDL_Event *event);
bool shared_chat_pilot_text_owner(void);
#else
static inline void shared_chat_pilot_create(void) {}
static inline void shared_chat_pilot_sync(void) {}
static inline void shared_chat_pilot_invalidate(void) {}
static inline void shared_chat_pilot_presented(bool presented) {
    (void)presented;
}
static inline bool shared_chat_pilot_event(SDL_Event *event) {
    (void)event;
    return false;
}
static inline bool shared_chat_pilot_poll(SDL_Event *event) {
    return SDL_PollEvent(event);
}
static inline bool shared_chat_pilot_text_owner(void) {
    return false;
}
#endif
#endif
