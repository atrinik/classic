/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <video_recording.h>

#include <gpu_renderer.h>
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_FPS 20U
#define RECORD_MAX_FRAMES 72000U
#define RECORD_MAX_PIXELS (8U * 1024U * 1024U)
#define RECORD_QUEUE 2U
#define RECORD_TIMEOUT_MS 5000U

typedef struct recording_frame {
    SDL_Surface *surface;
    uint32_t index;
} recording_frame_t;

/* The permanent address also survives renderer cancellation during shutdown. */
static struct {
    char executable[4096];
    char path[4096];
    char notice[4352];
    bool notice_failed;
    bool ever_failed;
    bool armed;
    bool pending;
    bool epoch_set;
    uint64_t epoch;
    uint32_t requested_index;
    uint32_t pending_index;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    /* Everything below is protected by mutex while the worker exists. */
    recording_frame_t queue[RECORD_QUEUE];
    unsigned int count;
    bool ready;
    bool stopping;
    bool finished;
    uint64_t stop_tick;
    uint32_t final_count;
    uint32_t submitted;
    uint32_t last_submitted_index;
    uint32_t dropped;
    char error[192];
} recording;

static void notice(const char *text, bool failed) {
    snprintf(recording.notice, sizeof(recording.notice), "%s: %s", text, recording.path);
    recording.notice_failed = failed;
    recording.ever_failed |= failed;
}

static void fail_locked(const char *text) {
    if (recording.error[0] == '\0') {
        SDL_strlcpy(recording.error, text, sizeof(recording.error));
    }
    if (!recording.stopping) {
        recording.stop_tick = SDL_GetTicks();
    }
    recording.stopping = true;
    SDL_SignalCondition(recording.condition);
}

static bool stop_expired(void) {
    SDL_LockMutex(recording.mutex);
    bool expired = recording.stopping &&
                   SDL_GetTicks() - recording.stop_tick >= RECORD_TIMEOUT_MS;
    SDL_UnlockMutex(recording.mutex);
    return expired;
}

/* SDL process pipes are nonblocking. Only this worker polls or waits on child. */
static bool pipe_transfer(SDL_IOStream *pipe, void *bytes, size_t size, bool write) {
    uint64_t progress = SDL_GetTicks();
    unsigned char *data = bytes;
    while (size != 0U) {
        if (stop_expired() || SDL_GetTicks() - progress >= RECORD_TIMEOUT_MS) {
            return false;
        }
        size_t n = write ? SDL_WriteIO(pipe, data, size) : SDL_ReadIO(pipe, data, size);
        if (n != 0U) {
            data += n;
            size -= n;
            progress = SDL_GetTicks();
        } else if (SDL_GetIOStatus(pipe) == SDL_IO_STATUS_NOT_READY) {
            SDL_Delay(2);
        } else {
            return false;
        }
    }
    return true;
}

static void put_u32(unsigned char *buffer, uint32_t value) {
    for (unsigned int i = 0; i < 4U; i++) {
        buffer[i] = (unsigned char)(value >> (8U * i));
    }
}

static bool send_frame(SDL_IOStream *input, const recording_frame_t *frame) {
    SDL_Surface *surface = frame->surface;
    unsigned char header[20] = {'F', 'R', 'A', 'M'};
    put_u32(header + 4, frame->index);
    put_u32(header + 8, (uint32_t)surface->w);
    put_u32(header + 12, (uint32_t)surface->h);
    put_u32(header + 16, (uint32_t)surface->w * (uint32_t)surface->h * 4U);
    if (!pipe_transfer(input, header, sizeof(header), true)) {
        return false;
    }
    for (int row = 0; row < surface->h; row++) {
        if (!pipe_transfer(input,
                           (unsigned char *)surface->pixels + (size_t)row * surface->pitch,
                           (size_t)surface->w * 4U,
                           true)) {
            return false;
        }
    }
    return true;
}

static int recording_worker(void *unused) {
    (void)unused;
    const char *args[] = {recording.executable, "--video-encoder", recording.path, NULL};
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_Environment *environment = SDL_CreateEnvironment(false);
    /* Preserve only the loader's explicit library search path. In particular,
     * no HOME, client configuration, scenario variables or preload is passed. */
    const char *library_path = SDL_getenv("LD_LIBRARY_PATH");
    bool properties_ok = properties != 0 && environment != NULL &&
        (library_path == NULL ||
         SDL_SetEnvironmentVariable(environment, "LD_LIBRARY_PATH", library_path, true)) &&
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment) &&
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *process = properties_ok ? SDL_CreateProcessWithProperties(properties) : NULL;
    SDL_DestroyProperties(properties);
    SDL_DestroyEnvironment(environment);
    SDL_IOStream *input = process != NULL ? SDL_GetProcessInput(process) : NULL;
    SDL_IOStream *output = process != NULL ? SDL_GetProcessOutput(process) : NULL;
    char response[6];
    bool ok = input != NULL && output != NULL &&
              pipe_transfer(output, response, sizeof(response), false) &&
              memcmp(response, "READY\n", sizeof(response)) == 0 &&
              pipe_transfer(input, (void *)"AVR1", 4U, true);
    SDL_LockMutex(recording.mutex);
    recording.ready = ok;
    if (!ok) {
        fail_locked("Encoder could not open a new output or initialize");
    }
    SDL_UnlockMutex(recording.mutex);

    while (ok) {
        SDL_LockMutex(recording.mutex);
        while (recording.count == 0U && !recording.stopping) {
            SDL_WaitConditionTimeout(recording.condition, recording.mutex, 100);
            /* Detect a failed encoder even while gameplay is idle. */
            if (SDL_WaitProcess(process, false, NULL)) {
                fail_locked("Encoder exited before recording finished");
                ok = false;
            }
        }
        if (!ok || (recording.count == 0U && recording.stopping)) {
            SDL_UnlockMutex(recording.mutex);
            break;
        }
        recording_frame_t frame = recording.queue[0];
        recording.count--;
        if (recording.count != 0U) {
            recording.queue[0] = recording.queue[1];
        }
        SDL_UnlockMutex(recording.mutex);
        ok = send_frame(input, &frame);
        SDL_DestroySurface(frame.surface);
        if (!ok) {
            SDL_LockMutex(recording.mutex);
            fail_locked("Encoder stopped accepting frames or exceeded its deadline");
            SDL_UnlockMutex(recording.mutex);
        }
    }

    if (ok) {
        unsigned char end[8] = {'S', 'T', 'O', 'P'};
        SDL_LockMutex(recording.mutex);
        put_u32(end + 4, recording.final_count);
        SDL_UnlockMutex(recording.mutex);
        ok = pipe_transfer(input, end, sizeof(end), true) &&
             pipe_transfer(output, response, sizeof(response), false) &&
             memcmp(response, "SAVED\n", sizeof(response)) == 0;
    }
    int exit_code = -1;
    if (process != NULL) {
        uint64_t waiting = SDL_GetTicks();
        while (ok && !SDL_WaitProcess(process, false, &exit_code)) {
            if (stop_expired() || SDL_GetTicks() - waiting >= RECORD_TIMEOUT_MS) {
                ok = false;
                break;
            }
            SDL_Delay(2);
        }
        if (!ok) {
            bool killed = SDL_KillProcess(process, true);
            uint64_t reap_started = SDL_GetTicks();
            bool reaped = SDL_WaitProcess(process, false, &exit_code);
            while (killed && !reaped && SDL_GetTicks() - reap_started < 1000U) {
                SDL_Delay(2);
                reaped = SDL_WaitProcess(process, false, &exit_code);
            }
            if (!reaped) {
                SDL_LockMutex(recording.mutex);
                SDL_strlcpy(recording.error, "Encoder termination could not be confirmed",
                            sizeof(recording.error));
                SDL_UnlockMutex(recording.mutex);
            }
        }
        SDL_DestroyProcess(process);
    }
    SDL_LockMutex(recording.mutex);
    if (!ok || exit_code != 0) {
        fail_locked("Recording failed; output may be incomplete (encoder or file limit)");
    }
    recording.finished = true;
    SDL_UnlockMutex(recording.mutex);
    return 0;
}

bool video_recording_initialize(const char *executable_name) {
    (void)executable_name;
#ifndef __linux__
    return SDL_SetError("Video recording currently requires Linux process isolation");
#else
    const char *base = SDL_GetBasePath();
    /* The installed CMake target has a fixed name. Never execute argv[0] or
     * search PATH: callers can supply either independently of this binary. */
    if (base == NULL ||
        snprintf(recording.executable, sizeof(recording.executable), "%satrinik", base) >=
            (int)sizeof(recording.executable)) {
        return SDL_SetError("Could not locate the recording encoder executable");
    }
    return true;
#endif
}

bool video_recording_start(const char *path) {
    if (recording.armed || recording.thread != NULL || recording.pending) {
        return SDL_SetError("A recording is already armed, running or finalizing");
    }
    bool absolute = path != NULL && path[0] == '/';
#ifdef WIN32
    absolute = path != NULL &&
               ((isalpha((unsigned char)path[0]) && path[1] == ':' &&
                 (path[2] == '/' || path[2] == '\\')) ||
                (path[0] == '\\' && path[1] == '\\'));
#endif
    if (!absolute || strlen(path) >= sizeof(recording.path)) {
        return SDL_SetError("Recording requires an absolute new output path (maximum 4095 bytes)");
    }
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        if (*p < 32U || *p == 127U) {
            return SDL_SetError("Recording output path contains a control character");
        }
    }
    if (recording.executable[0] == '\0') {
        return SDL_SetError("Recording encoder unavailable (Linux process isolation required)");
    }
    SDL_strlcpy(recording.path, path, sizeof(recording.path));
    recording.armed = true;
    notice("Recording armed; starts with gameplay", false);
    return true;
}

void video_recording_stop(void) {
    if (recording.armed) {
        recording.armed = false;
        notice("Recording canceled before gameplay", false);
    }
    if (recording.thread == NULL) {
        return;
    }
    SDL_LockMutex(recording.mutex);
    if (!recording.stopping) {
        recording.stop_tick = SDL_GetTicks();
        recording.final_count = recording.epoch_set
                                    ? (uint32_t)((recording.stop_tick - recording.epoch) * RECORD_FPS /
                                                 1000U) + 1U
                                    : 0U;
        if (recording.final_count > RECORD_MAX_FRAMES) {
            recording.final_count = RECORD_MAX_FRAMES;
        }
        if (recording.submitted != 0U && recording.final_count <= recording.last_submitted_index) {
            recording.final_count = recording.last_submitted_index + 1U;
        }
        recording.stopping = true;
        SDL_SignalCondition(recording.condition);
        notice("Finalizing recording", false);
    }
    SDL_UnlockMutex(recording.mutex);
}

static void capture_complete(SDL_Surface *surface, void *userdata) {
    (void)userdata;
    recording.pending = false;
    if (recording.thread == NULL) {
        SDL_DestroySurface(surface);
        return;
    }
    SDL_LockMutex(recording.mutex);
    if (!recording.stopping) {
        if (surface == NULL || surface->format != SDL_PIXELFORMAT_RGBA32 ||
            surface->w <= 0 || surface->h <= 0 || surface->w > 4096 || surface->h > 4096 ||
            (uint64_t)surface->w * surface->h > RECORD_MAX_PIXELS) {
            fail_locked("Recording frame readback failed or exceeded dimension limits");
        } else if (recording.count < RECORD_QUEUE) {
            recording.queue[recording.count++] =
                (recording_frame_t){surface, recording.pending_index};
            recording.submitted++;
            recording.last_submitted_index = recording.pending_index;
            surface = NULL;
            SDL_SignalCondition(recording.condition);
        } else {
            recording.dropped++;
        }
    }
    SDL_UnlockMutex(recording.mutex);
    SDL_DestroySurface(surface);
}

static void capture_cancel(void *userdata) {
    capture_complete(NULL, userdata);
}

void video_recording_frame(bool playing, bool presented, uint64_t now_ms) {
    if (recording.armed && playing && presented) {
        recording.armed = false;
        recording.epoch_set = false;
        recording.count = 0;
        recording.ready = recording.stopping = recording.finished = false;
        recording.submitted = recording.dropped = recording.final_count = 0;
        recording.last_submitted_index = 0;
        recording.error[0] = '\0';
        recording.mutex = SDL_CreateMutex();
        recording.condition = SDL_CreateCondition();
        if (recording.mutex != NULL && recording.condition != NULL) {
            recording.thread = SDL_CreateThread(recording_worker, "video-recording", NULL);
        }
        if (recording.thread == NULL) {
            SDL_DestroyCondition(recording.condition);
            SDL_DestroyMutex(recording.mutex);
            recording.condition = NULL;
            recording.mutex = NULL;
            notice("Could not start recording worker", true);
        } else {
            notice("Recording active (20 fps Motion JPEG AVI, no audio)", false);
        }
    }
    if (recording.thread == NULL) {
        return;
    }
    if (!playing) {
        video_recording_stop();
    }
    SDL_LockMutex(recording.mutex);
    bool finished = recording.finished;
    bool capture = recording.ready && !recording.stopping && recording.count < RECORD_QUEUE;
    SDL_UnlockMutex(recording.mutex);
    if (finished && !recording.pending) {
        SDL_WaitThread(recording.thread, NULL);
        recording.thread = NULL;
        for (unsigned int i = 0; i < recording.count; i++) {
            SDL_DestroySurface(recording.queue[i].surface);
        }
        SDL_DestroyCondition(recording.condition);
        SDL_DestroyMutex(recording.mutex);
        recording.condition = NULL;
        recording.mutex = NULL;
        if (recording.error[0] != '\0') {
            notice(recording.error, true);
        } else {
            char status[192];
            uint32_t repeats = recording.final_count > recording.submitted
                                   ? recording.final_count - recording.submitted
                                   : 0U;
            snprintf(status, sizeof(status), "Recording saved (%u captured frames, %u repeated slots)",
                     recording.submitted, repeats);
            notice(status, false);
        }
        return;
    }
    if (!capture || !playing || !presented || recording.pending) {
        return;
    }
    uint64_t index = recording.epoch_set ? (now_ms - recording.epoch) * RECORD_FPS / 1000U : 0U;
    if (index >= RECORD_MAX_FRAMES) {
        video_recording_stop();
        return;
    }
    if (recording.epoch_set && index <= recording.requested_index) {
        return;
    }
    int width, height;
    if (!gpu_renderer_output_size(&width, &height) || width < 1 || height < 1 ||
        width > 4096 || height > 4096 || (uint64_t)width * height > RECORD_MAX_PIXELS) {
        SDL_LockMutex(recording.mutex);
        fail_locked("Recording window exceeds 4096 pixels per side or 8 megapixels");
        SDL_UnlockMutex(recording.mutex);
        return;
    }
    if (!recording.epoch_set) {
        recording.epoch = now_ms;
        recording.epoch_set = true;
    }
    recording.requested_index = (uint32_t)index;
    recording.pending_index = (uint32_t)index;
    recording.pending = true;
    if (!gpu_renderer_readback_async(NULL, capture_complete, capture_cancel, NULL)) {
        capture_complete(NULL, NULL);
    }
}

bool video_recording_message(char *message, size_t capacity, bool *failed) {
    if (recording.notice[0] == '\0') {
        return false;
    }
    SDL_strlcpy(message, recording.notice, capacity);
    *failed = recording.notice_failed;
    recording.notice[0] = '\0';
    return true;
}

bool video_recording_failed(void) {
    return recording.ever_failed;
}

void video_recording_shutdown(void) {
    video_recording_stop();
    if (recording.thread != NULL) {
        SDL_WaitThread(recording.thread, NULL);
        recording.thread = NULL;
        for (unsigned int i = 0; i < recording.count; i++) {
            SDL_DestroySurface(recording.queue[i].surface);
        }
        SDL_DestroyCondition(recording.condition);
        SDL_DestroyMutex(recording.mutex);
        recording.condition = NULL;
        recording.mutex = NULL;
        notice(recording.error[0] != '\0' ? recording.error : "Recording saved",
               recording.error[0] != '\0');
    }
}
