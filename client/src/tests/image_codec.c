/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Deterministic priority regressions for the SDL_image codec gate. */

#include <global.h>
#include <image_codec.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

typedef struct test_context {
    SDL_Mutex *mutex;
    bool order[2];
    size_t count;
} test_context_t;

typedef struct test_caller {
    test_context_t *context;
    bool background;
} test_caller_t;

static int test_codec_caller(void *data) {
    test_caller_t *caller = data;
    TEST_CHECK(image_codec_test_enter(caller->background));

    SDL_LockMutex(caller->context->mutex);
    TEST_CHECK(caller->context->count < arraysize(caller->context->order));
    caller->context->order[caller->context->count++] = caller->background;
    SDL_UnlockMutex(caller->context->mutex);

    image_codec_test_leave(caller->background);
    return 0;
}

int main(void) {
    TEST_CHECK(SDL_Init(0));
    TEST_CHECK(image_codec_parallel_start());

    test_context_t context = {.mutex = SDL_CreateMutex()};
    TEST_CHECK(context.mutex != NULL);
    TEST_CHECK(image_codec_test_enter(true));

    test_caller_t main_caller = {.context = &context};
    SDL_Thread *main_thread = SDL_CreateThread(test_codec_caller, "codec-main", &main_caller);
    TEST_CHECK(main_thread != NULL);
    image_codec_test_wait_for_waiters(1, 0);

    test_caller_t background_caller = {.context = &context, .background = true};
    SDL_Thread *background_thread =
        SDL_CreateThread(test_codec_caller, "codec-background", &background_caller);
    TEST_CHECK(background_thread != NULL);
    image_codec_test_wait_for_waiters(1, 1);

    image_codec_test_leave(true);
    SDL_WaitThread(main_thread, NULL);
    SDL_WaitThread(background_thread, NULL);

    TEST_CHECK(context.count == arraysize(context.order));
    TEST_CHECK(!context.order[0]);
    TEST_CHECK(context.order[1]);

    SDL_DestroyMutex(context.mutex);
    image_codec_parallel_stop();
    SDL_Quit();
    return 0;
}
