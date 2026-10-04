/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <gpu_renderer.h>
#include <video_recording.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

#define WIRE_CAPACITY 4096U

typedef struct fake_process_state {
    SDL_IOStream *input;
    SDL_IOStream *output;
    unsigned char wire[WIRE_CAPACITY];
    size_t wire_size;
    unsigned int response;
    bool malformed_ready;
    bool create_failed;
    bool killed;
    bool destroyed;
    char executable[4096];
    char mode[64];
    char path[4096];
    char library_path[4096];
    bool environment_isolated;
    Sint64 stdin_mode;
    Sint64 stdout_mode;
    Sint64 stderr_mode;
} fake_process_state_t;

static fake_process_state_t process_state;
static int output_width = 2;
static int output_height = 1;
static bool output_size_ok = true;
static bool readback_accept = true;
static unsigned int readback_requests;
static gpu_renderer_readback_callback_t readback_complete;
static gpu_renderer_readback_cancel_callback_t readback_cancel;
static void *readback_userdata;
static SDL_AtomicInt input_blocked;
static SDL_AtomicInt input_gate;

static size_t SDLCALL fake_input_write(void *userdata,
                                       const void *ptr,
                                       size_t size,
                                       SDL_IOStatus *status) {
    fake_process_state_t *state = userdata;
    if (state->wire_size >= 4U && SDL_GetAtomicInt(&input_gate) != 0) {
        SDL_SetAtomicInt(&input_blocked, 1);
        *status = SDL_IO_STATUS_NOT_READY;
        return 0;
    }
    if (size > sizeof(state->wire) - state->wire_size) {
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    memcpy(state->wire + state->wire_size, ptr, size);
    state->wire_size += size;
    return size;
}

static size_t SDLCALL fake_output_read(void *userdata,
                                       void *ptr,
                                       size_t size,
                                       SDL_IOStatus *status) {
    fake_process_state_t *state = userdata;
    const char *response;
    if (state->response++ == 0U) {
        response = state->malformed_ready ? "BROKE\n" : "READY\n";
    } else {
        response = "SAVED\n";
    }
    if (size > 6U) {
        size = 6U;
    }
    memcpy(ptr, response, size);
    (void)status;
    return size;
}

static bool SDLCALL fake_io_close(void *userdata) {
    (void)userdata;
    return true;
}

static SDL_IOStream *open_fake_stream(bool input) {
    SDL_IOStreamInterface interface;
    SDL_INIT_INTERFACE(&interface);
    interface.write = input ? fake_input_write : NULL;
    interface.read = input ? NULL : fake_output_read;
    interface.close = fake_io_close;
    return SDL_OpenIO(&interface, &process_state);
}

SDL_Process *SDL_CreateProcessWithProperties(SDL_PropertiesID properties) {
    if (process_state.create_failed) {
        return NULL;
    }
    const char *const *args =
        SDL_GetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, NULL);
    TEST_CHECK(args != NULL);
    TEST_CHECK(args[0] != NULL && args[1] != NULL && args[2] != NULL && args[3] == NULL);
    SDL_strlcpy(process_state.executable, args[0], sizeof(process_state.executable));
    SDL_strlcpy(process_state.mode, args[1], sizeof(process_state.mode));
    SDL_strlcpy(process_state.path, args[2], sizeof(process_state.path));
    SDL_Environment *environment =
        SDL_GetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, NULL);
    TEST_CHECK(environment != NULL);
    const char *library_path = SDL_GetEnvironmentVariable(environment, "LD_LIBRARY_PATH");
    SDL_strlcpy(process_state.library_path,
                library_path != NULL ? library_path : "",
                sizeof(process_state.library_path));
    process_state.environment_isolated =
        SDL_GetEnvironmentVariable(environment, "HOME") == NULL &&
        SDL_GetEnvironmentVariable(environment, "ATRINIK_RECORDING_TEST_SECRET") == NULL;
    process_state.stdin_mode =
        SDL_GetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, -1);
    process_state.stdout_mode =
        SDL_GetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, -1);
    process_state.stderr_mode =
        SDL_GetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, -1);
    process_state.input = open_fake_stream(true);
    process_state.output = open_fake_stream(false);
    TEST_CHECK(process_state.input != NULL && process_state.output != NULL);
    return (SDL_Process *)&process_state;
}

SDL_IOStream *SDL_GetProcessInput(SDL_Process *process) {
    TEST_CHECK(process == (SDL_Process *)&process_state);
    return process_state.input;
}

SDL_IOStream *SDL_GetProcessOutput(SDL_Process *process) {
    TEST_CHECK(process == (SDL_Process *)&process_state);
    return process_state.output;
}

bool SDL_WaitProcess(SDL_Process *process, bool block, int *exit_code) {
    TEST_CHECK(process == (SDL_Process *)&process_state);
    (void)block;
    bool stopped = process_state.wire_size >= 8U &&
                   memcmp(process_state.wire + process_state.wire_size - 8U, "STOP", 4U) == 0;
    if ((stopped || process_state.killed) && exit_code != NULL) {
        *exit_code = 0;
    }
    return stopped || process_state.killed;
}

bool SDL_KillProcess(SDL_Process *process, bool force) {
    TEST_CHECK(process == (SDL_Process *)&process_state);
    TEST_CHECK(force);
    process_state.killed = true;
    return true;
}

void SDL_DestroyProcess(SDL_Process *process) {
    TEST_CHECK(process == (SDL_Process *)&process_state);
    SDL_CloseIO(process_state.input);
    SDL_CloseIO(process_state.output);
    process_state.input = NULL;
    process_state.output = NULL;
    process_state.destroyed = true;
}

bool gpu_renderer_output_size(int *width, int *height) {
    *width = output_width;
    *height = output_height;
    return output_size_ok;
}

bool gpu_renderer_readback_async(const SDL_Rect *rect,
                                 gpu_renderer_readback_callback_t callback,
                                 gpu_renderer_readback_cancel_callback_t cancel_callback,
                                 void *userdata) {
    TEST_CHECK(rect == NULL);
    TEST_CHECK(readback_complete == NULL && readback_cancel == NULL);
    readback_requests++;
    if (!readback_accept) {
        return false;
    }
    readback_complete = callback;
    readback_cancel = cancel_callback;
    readback_userdata = userdata;
    return true;
}

static uint32_t get_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U | (uint32_t)bytes[2] << 16U |
           (uint32_t)bytes[3] << 24U;
}

static void reset_fakes(void) {
    bool malformed_ready = process_state.malformed_ready;
    bool create_failed = process_state.create_failed;
    memset(&process_state, 0, sizeof(process_state));
    process_state.malformed_ready = malformed_ready;
    process_state.create_failed = create_failed;
    output_width = 2;
    output_height = 1;
    output_size_ok = true;
    readback_accept = true;
    readback_requests = 0;
    readback_complete = NULL;
    readback_cancel = NULL;
    readback_userdata = NULL;
    SDL_SetAtomicInt(&input_blocked, 0);
    SDL_SetAtomicInt(&input_gate, 0);
}

static void consume_message(const char *fragment, bool expected_failed) {
    char message[4600];
    bool failed = !expected_failed;
    TEST_CHECK(video_recording_message(message, sizeof(message), &failed));
    TEST_CHECK(strstr(message, fragment) != NULL);
    TEST_CHECK(failed == expected_failed);
}

static void wait_for_readback(unsigned int expected, uint64_t now_ms) {
    for (unsigned int i = 0; i < 1000U && readback_requests < expected; i++) {
        video_recording_frame(true, true, now_ms);
        SDL_Delay(1);
    }
    TEST_CHECK(readback_requests == expected);
}

static void wait_for_blocked_input(void) {
    for (unsigned int i = 0; i < 1000U && SDL_GetAtomicInt(&input_blocked) == 0; i++) {
        SDL_Delay(1);
    }
    TEST_CHECK(SDL_GetAtomicInt(&input_blocked) != 0);
}

static void finish_readback(unsigned char seed) {
    TEST_CHECK(readback_complete != NULL);
    gpu_renderer_readback_callback_t callback = readback_complete;
    void *userdata = readback_userdata;
    readback_complete = NULL;
    readback_cancel = NULL;
    readback_userdata = NULL;
    SDL_Surface *surface = SDL_CreateSurface(output_width, output_height, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    TEST_CHECK((size_t)surface->pitch >= (size_t)surface->w * 4U);
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w * 4; x++) {
            ((unsigned char *)surface->pixels)[(size_t)y * surface->pitch + (size_t)x] =
                (unsigned char)(seed + x);
        }
    }
    callback(surface, userdata);
}

static void cancel_readback(void) {
    TEST_CHECK(readback_cancel != NULL);
    gpu_renderer_readback_cancel_callback_t callback = readback_cancel;
    void *userdata = readback_userdata;
    readback_complete = NULL;
    readback_cancel = NULL;
    readback_userdata = NULL;
    callback(userdata);
}

static void test_validation_and_cancel(void) {
    TEST_CHECK(!video_recording_failed());
    TEST_CHECK(!video_recording_start("relative.avi"));
    TEST_CHECK(strstr(SDL_GetError(), "absolute") != NULL);
    TEST_CHECK(!video_recording_start("/tmp/bad\nname.avi"));
    TEST_CHECK(strstr(SDL_GetError(), "control") != NULL);
    TEST_CHECK(video_recording_start("/tmp/armed.avi"));
    consume_message("Recording armed", false);
    TEST_CHECK(!video_recording_start("/tmp/second.avi"));
    video_recording_frame(false, true, SDL_GetTicks());
    TEST_CHECK(readback_requests == 0U);
    video_recording_stop();
    consume_message("Recording canceled", false);
}

static void test_protocol_timestamps_and_isolation(void) {
    reset_fakes();
    TEST_CHECK(
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "LD_LIBRARY_PATH", "/test/lib", true));
    TEST_CHECK(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),
                                          "ATRINIK_RECORDING_TEST_SECRET",
                                          "do-not-copy",
                                          true));
    TEST_CHECK(video_recording_start("/tmp/capture.avi"));
    consume_message("Recording armed", false);
    uint64_t epoch = SDL_GetTicks();
    wait_for_readback(1U, epoch);
    finish_readback(0x10U);
    video_recording_frame(true, true, epoch + 49U);
    TEST_CHECK(readback_requests == 1U);
    wait_for_readback(2U, epoch + 50U);
    finish_readback(0x20U);
    video_recording_stop();
    video_recording_shutdown();

    TEST_CHECK(process_state.destroyed && !process_state.killed);
    size_t executable_length = strlen(process_state.executable);
    TEST_CHECK(executable_length >= strlen("atrinik"));
    TEST_CHECK(
        strcmp(process_state.executable + executable_length - strlen("atrinik"), "atrinik") == 0);
    TEST_CHECK(strcmp(process_state.mode, "--video-encoder") == 0);
    TEST_CHECK(strcmp(process_state.path, "/tmp/capture.avi") == 0);
    TEST_CHECK(strcmp(process_state.library_path, "/test/lib") == 0);
    TEST_CHECK(process_state.environment_isolated);
    TEST_CHECK(process_state.stdin_mode == SDL_PROCESS_STDIO_APP);
    TEST_CHECK(process_state.stdout_mode == SDL_PROCESS_STDIO_APP);
    TEST_CHECK(process_state.stderr_mode == SDL_PROCESS_STDIO_NULL);

    const unsigned char *wire = process_state.wire;
    TEST_CHECK(process_state.wire_size == 4U + 28U + 28U + 8U);
    TEST_CHECK(memcmp(wire, "AVR1FRAM", 8U) == 0);
    TEST_CHECK(get_u32(wire + 8U) == 0U);
    TEST_CHECK(get_u32(wire + 12U) == 2U && get_u32(wire + 16U) == 1U);
    TEST_CHECK(get_u32(wire + 20U) == 8U);
    for (unsigned int i = 0; i < 8U; i++) {
        TEST_CHECK(wire[24U + i] == (unsigned char)(0x10U + i));
    }
    TEST_CHECK(memcmp(wire + 32U, "FRAM", 4U) == 0);
    TEST_CHECK(get_u32(wire + 36U) == 1U);
    TEST_CHECK(get_u32(wire + 40U) == 2U && get_u32(wire + 44U) == 1U);
    TEST_CHECK(get_u32(wire + 48U) == 8U);
    for (unsigned int i = 0; i < 8U; i++) {
        TEST_CHECK(wire[52U + i] == (unsigned char)(0x20U + i));
    }
    TEST_CHECK(memcmp(wire + 60U, "STOP", 4U) == 0);
    TEST_CHECK(get_u32(wire + 64U) >= 2U);
    consume_message("Recording saved", false);
}

static void test_bounded_pending_and_shutdown_callback(void) {
    reset_fakes();
    TEST_CHECK(video_recording_start("/tmp/pending.avi"));
    uint64_t epoch = SDL_GetTicks();
    wait_for_readback(1U, epoch);
    for (unsigned int i = 1; i <= 1000U; i++) {
        video_recording_frame(true, true, epoch + (uint64_t)i * 50U);
    }
    TEST_CHECK(readback_requests == 1U);
    video_recording_shutdown();
    TEST_CHECK(process_state.destroyed);
    TEST_CHECK(!video_recording_start("/tmp/still-pending.avi"));
    cancel_readback();
    TEST_CHECK(video_recording_start("/tmp/after-cancel.avi"));
    video_recording_stop();
    consume_message("Recording canceled", false);
}

static void test_encoder_backpressure_bounds_queue(void) {
    reset_fakes();
    SDL_SetAtomicInt(&input_gate, 1);
    TEST_CHECK(video_recording_start("/tmp/backpressure.avi"));
    uint64_t epoch = SDL_GetTicks();
    wait_for_readback(1U, epoch);
    finish_readback(0x30U);
    wait_for_blocked_input();

    wait_for_readback(2U, epoch + 100U);
    finish_readback(0x40U);
    wait_for_readback(3U, epoch + 250U);
    finish_readback(0x50U);
    for (unsigned int i = 6U; i <= 1000U; i++) {
        video_recording_frame(true, true, epoch + (uint64_t)i * 50U);
    }
    TEST_CHECK(readback_requests == 3U);

    SDL_SetAtomicInt(&input_gate, 0);
    video_recording_stop();
    video_recording_shutdown();
    TEST_CHECK(process_state.destroyed && !process_state.killed);
    TEST_CHECK(process_state.wire_size == 4U + 3U * 28U + 8U);
    TEST_CHECK(memcmp(process_state.wire, "AVR1FRAM", 8U) == 0);
    TEST_CHECK(get_u32(process_state.wire + 8U) == 0U);
    TEST_CHECK(memcmp(process_state.wire + 32U, "FRAM", 4U) == 0);
    TEST_CHECK(get_u32(process_state.wire + 36U) == 2U);
    TEST_CHECK(memcmp(process_state.wire + 60U, "FRAM", 4U) == 0);
    TEST_CHECK(get_u32(process_state.wire + 64U) == 5U);
    TEST_CHECK(memcmp(process_state.wire + 88U, "STOP", 4U) == 0);
    TEST_CHECK(get_u32(process_state.wire + 92U) >= 6U);
    consume_message("Recording saved", false);
}

static void test_capture_failures(void) {
    reset_fakes();
    process_state.create_failed = true;
    TEST_CHECK(video_recording_start("/tmp/process-create.avi"));
    uint64_t now = SDL_GetTicks();
    for (unsigned int i = 0; i < 100U; i++) {
        video_recording_frame(true, true, now);
        SDL_Delay(1);
    }
    video_recording_shutdown();
    consume_message("Encoder could not", true);
    process_state.create_failed = false;

    reset_fakes();
    output_width = 4097;
    TEST_CHECK(video_recording_start("/tmp/oversize.avi"));
    now = SDL_GetTicks();
    for (unsigned int i = 0; i < 100U; i++) {
        video_recording_frame(true, true, now);
        SDL_Delay(1);
    }
    video_recording_shutdown();
    consume_message("exceeds 4096", true);
    TEST_CHECK(video_recording_failed());

    reset_fakes();
    readback_accept = false;
    TEST_CHECK(video_recording_start("/tmp/readback.avi"));
    wait_for_readback(1U, SDL_GetTicks());
    video_recording_shutdown();
    consume_message("readback failed", true);

    reset_fakes();
    process_state.malformed_ready = true;
    TEST_CHECK(video_recording_start("/tmp/not-ready.avi"));
    now = SDL_GetTicks();
    for (unsigned int i = 0; i < 100U; i++) {
        video_recording_frame(true, true, now);
        SDL_Delay(1);
    }
    video_recording_shutdown();
    consume_message("Encoder could not", true);
    TEST_CHECK(video_recording_failed());
    process_state.malformed_ready = false;
}

int main(void) {
    TEST_CHECK(SDL_Init(0));
    TEST_CHECK(video_recording_initialize("/untrusted/argv0"));
    test_validation_and_cancel();
    test_protocol_timestamps_and_isolation();
    test_bounded_pending_and_shutdown_callback();
    test_encoder_backpressure_bounds_queue();
    test_capture_failures();
    video_recording_shutdown();
    SDL_Quit();
    return 0;
}
