/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <toolkit/porting.h>

#include <gpu_renderer.h>
#include <image_codec.h>
#include <live_movement_capture.h>

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fprintf(stderr, "%d\n", __LINE__); \
            return 1;                          \
        }                                      \
    } while (0)

#define TEST_PATH_MAX 4096U

static gpu_renderer_readback_callback_t queued_complete;
static gpu_renderer_readback_cancel_callback_t queued_cancel;
static void *queued_userdata;
static bool queue_success = true;
static bool encoder_success = true;
static const unsigned char encoded_png[] = "mock-png-data";

bool gpu_renderer_readback_async(const SDL_Rect *rect,
                                 gpu_renderer_readback_callback_t callback,
                                 gpu_renderer_readback_cancel_callback_t cancel_callback,
                                 void *userdata) {
    if (!queue_success || rect != NULL || callback == NULL || cancel_callback == NULL) {
        return false;
    }
    queued_complete = callback;
    queued_cancel = cancel_callback;
    queued_userdata = userdata;
    return true;
}

bool image_codec_save_png_io(SDL_Surface *surface, SDL_IOStream *stream, bool close_stream) {
    if (!encoder_success || surface == NULL || stream == NULL || close_stream) {
        return false;
    }
    return SDL_WriteIO(stream, encoded_png, sizeof(encoded_png) - 1U) == sizeof(encoded_png) - 1U;
}

static void mock_reset(void) {
    queued_complete = NULL;
    queued_cancel = NULL;
    queued_userdata = NULL;
    queue_success = true;
    encoder_success = true;
}

static void mock_complete(SDL_Surface *surface) {
    gpu_renderer_readback_callback_t callback = queued_complete;
    void *userdata = queued_userdata;
    queued_complete = NULL;
    queued_cancel = NULL;
    queued_userdata = NULL;
    callback(surface, userdata);
}

static void mock_cancel(void) {
    gpu_renderer_readback_cancel_callback_t callback = queued_cancel;
    void *userdata = queued_userdata;
    queued_complete = NULL;
    queued_cancel = NULL;
    queued_userdata = NULL;
    callback(userdata);
}

static bool test_path(char path[TEST_PATH_MAX], bool preserve) {
    char working[TEST_PATH_MAX];
    if (getcwd(working, sizeof(working)) == NULL ||
        snprintf(path, TEST_PATH_MAX, "%s/live-movement-capture-XXXXXX", working) >=
            (int)TEST_PATH_MAX) {
        return false;
    }
    int descriptor = mkstemp(path);
    if (descriptor < 0 || close(descriptor) != 0) {
        return false;
    }
    return preserve || unlink(path) == 0;
}

static live_movement_capture_t *create_capture(char path[TEST_PATH_MAX]) {
    if (!test_path(path, false)) {
        return NULL;
    }
    char error[256];
    return live_movement_capture_create(path, error, sizeof(error));
}

static int test_create_exclusive(void) {
    char error[256];
    REQUIRE(live_movement_capture_create("relative.png", error, sizeof(error)) == NULL);
    REQUIRE(strstr(error, "absolute") != NULL);

    char path[TEST_PATH_MAX];
    REQUIRE(test_path(path, true));
    FILE *existing = fopen(path, "wb");
    REQUIRE(existing != NULL);
    REQUIRE(fwrite("keep", 1, 4, existing) == 4U);
    REQUIRE(fclose(existing) == 0);
    REQUIRE(live_movement_capture_create(path, error, sizeof(error)) == NULL);
    existing = fopen(path, "rb");
    REQUIRE(existing != NULL);
    char contents[5] = {0};
    REQUIRE(fread(contents, 1, 4, existing) == 4U);
    REQUIRE(fclose(existing) == 0);
    REQUIRE(strcmp(contents, "keep") == 0);
    REQUIRE(unlink(path) == 0);

    REQUIRE(test_path(path, false));
#ifndef WIN32
    mode_t previous = umask(0002);
#endif
    live_movement_capture_t *capture = live_movement_capture_create(path, error, sizeof(error));
#ifndef WIN32
    umask(previous);
#endif
    REQUIRE(capture != NULL);
    const live_movement_capture_result_t *result = live_movement_capture_result(capture);
    REQUIRE(result != NULL && result->status == LIVE_MOVEMENT_CAPTURE_READY);
    REQUIRE(strcmp(result->path, path) == 0);
#ifndef WIN32
    struct stat attributes;
    REQUIRE(stat(path, &attributes) == 0);
    REQUIRE((attributes.st_mode & 0777) == 0600);
#endif
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);
    return 0;
}

static int test_success(void) {
    mock_reset();
    char path[TEST_PATH_MAX];
    live_movement_capture_t *capture = create_capture(path);
    REQUIRE(capture != NULL);
    REQUIRE(live_movement_capture_request(capture));
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_PENDING);
    REQUIRE(!live_movement_capture_request(capture));
    SDL_Surface *surface = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    const live_movement_capture_result_t *result = live_movement_capture_result(capture);
    REQUIRE(result->status == LIVE_MOVEMENT_CAPTURE_COMPLETE);
    REQUIRE(result->width == 2U && result->height == 2U);
    REQUIRE(result->size_bytes == sizeof(encoded_png) - 1U);
    REQUIRE(strcmp(result->sha256,
                   "299dbbe8bea67192ea4eba822553df9ba00b97ba42e5e36a8dad0e7aefee9dac") == 0);
    REQUIRE(result->error[0] == '\0');
    FILE *stream = fopen(path, "rb");
    REQUIRE(stream != NULL);
    unsigned char contents[sizeof(encoded_png)] = {0};
    REQUIRE(fread(contents, 1, sizeof(encoded_png) - 1U, stream) == sizeof(encoded_png) - 1U);
    REQUIRE(fclose(stream) == 0);
    REQUIRE(memcmp(contents, encoded_png, sizeof(encoded_png) - 1U) == 0);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);
    return 0;
}

static int test_queue_and_callback_failures(void) {
    mock_reset();
    char path[TEST_PATH_MAX];
    live_movement_capture_t *capture = create_capture(path);
    REQUIRE(capture != NULL);
    queue_success = false;
    REQUIRE(!live_movement_capture_request(capture));
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    mock_cancel();
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    REQUIRE(strstr(live_movement_capture_result(capture)->error, "canceled") != NULL);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    mock_complete(NULL);
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    encoder_success = false;
    SDL_Surface *surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);
    return 0;
}

static int test_bounds_and_write_failure(void) {
    mock_reset();
    char path[TEST_PATH_MAX];
    live_movement_capture_t *capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    SDL_Surface *surface = SDL_CreateSurface(4097, 1, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    surface = SDL_CreateSurface(4096, 2048, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    REQUIRE(live_movement_capture_test_close_descriptor(capture));
    surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    REQUIRE(live_movement_capture_result(capture)->status == LIVE_MOVEMENT_CAPTURE_FAILED);
    REQUIRE(strstr(live_movement_capture_result(capture)->error, "write") != NULL);
    live_movement_capture_destroy(capture);
    REQUIRE(unlink(path) == 0);
    return 0;
}

static int test_pending_destroy(void) {
    mock_reset();
    char path[TEST_PATH_MAX];
    live_movement_capture_t *capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    live_movement_capture_destroy(capture);
    SDL_Surface *surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != NULL);
    mock_complete(surface);
    REQUIRE(unlink(path) == 0);

    mock_reset();
    capture = create_capture(path);
    REQUIRE(capture != NULL && live_movement_capture_request(capture));
    live_movement_capture_destroy(capture);
    mock_cancel();
    REQUIRE(unlink(path) == 0);
    return 0;
}

int main(void) {
    REQUIRE(SDL_Init(0));
    REQUIRE(test_create_exclusive() == 0);
    REQUIRE(test_success() == 0);
    REQUIRE(test_queue_and_callback_failures() == 0);
    REQUIRE(test_bounds_and_write_failure() == 0);
    REQUIRE(test_pending_destroy() == 0);
    SDL_Quit();
    return 0;
}
