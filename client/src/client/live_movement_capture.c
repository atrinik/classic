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

#include <live_movement_capture.h>

#include <gpu_renderer.h>
#include <image_codec.h>

#include <SDL3/SDL.h>
#include <openssl/evp.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIVE_MOVEMENT_CAPTURE_PATH_MAX 4095U
#define LIVE_MOVEMENT_CAPTURE_DIMENSION_MAX 4096U
#define LIVE_MOVEMENT_CAPTURE_PNG_MAX (64U * 1024U * 1024U)
#define LIVE_MOVEMENT_CAPTURE_PNG_OVERHEAD 65536U
#define LIVE_MOVEMENT_CAPTURE_BYTES_PER_PIXEL 8U

struct live_movement_capture {
    live_movement_capture_result_t result;
    char path[LIVE_MOVEMENT_CAPTURE_PATH_MAX + 1U];
    FILE *stream;
    bool release_requested;
};

static void capture_text(char *destination, size_t destination_size, const char *format, ...) {
    if (destination == NULL || destination_size == 0U) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(destination, destination_size, format, args);
    va_end(args);
}

static bool capture_absolute_path(const char *path) {
    if (path == NULL || *path == '\0') {
        return false;
    }
#ifdef WIN32
    unsigned char first = (unsigned char)path[0];
    return (isalpha(first) && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
           (path[0] == '\\' && path[1] == '\\');
#else
    return path[0] == '/';
#endif
}

static bool capture_close(live_movement_capture_t *capture) {
    if (capture->stream == NULL) {
        return true;
    }
    FILE *stream = capture->stream;
    capture->stream = NULL;
    return fclose(stream) == 0;
}

static void capture_fail(live_movement_capture_t *capture, const char *reason) {
    capture->result.status = LIVE_MOVEMENT_CAPTURE_FAILED;
    capture_text(capture->result.error, sizeof(capture->result.error), "%s", reason);
    capture_close(capture);
}

static bool capture_dimensions(SDL_Surface *surface, size_t *capacity) {
    if (surface == NULL || surface->w < 1 || surface->h < 1 ||
        surface->w > (int)LIVE_MOVEMENT_CAPTURE_DIMENSION_MAX ||
        surface->h > (int)LIVE_MOVEMENT_CAPTURE_DIMENSION_MAX) {
        return false;
    }
    size_t width = (size_t)surface->w;
    size_t height = (size_t)surface->h;
    if (height > SIZE_MAX / width) {
        return false;
    }
    size_t pixels = width * height;
    if (pixels > (LIVE_MOVEMENT_CAPTURE_PNG_MAX - LIVE_MOVEMENT_CAPTURE_PNG_OVERHEAD) /
                     LIVE_MOVEMENT_CAPTURE_BYTES_PER_PIXEL) {
        return false;
    }
    *capacity = pixels * LIVE_MOVEMENT_CAPTURE_BYTES_PER_PIXEL + LIVE_MOVEMENT_CAPTURE_PNG_OVERHEAD;
    return *capacity <= LIVE_MOVEMENT_CAPTURE_PNG_MAX;
}

static bool capture_sha256(const unsigned char *bytes, size_t size, char output[65]) {
    unsigned char digest[32];
    unsigned int digest_size = 0;
    if (EVP_Digest(bytes, size, digest, &digest_size, EVP_sha256(), NULL) != 1 ||
        digest_size != sizeof(digest)) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        output[i * 2U] = hex[digest[i] >> 4U];
        output[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    output[64] = '\0';
    return true;
}

static void capture_complete(SDL_Surface *surface, void *userdata) {
    live_movement_capture_t *capture = userdata;
    if (capture->release_requested) {
        SDL_DestroySurface(surface);
        free(capture);
        return;
    }
    size_t capacity = 0;
    if (surface == NULL) {
        capture_fail(capture, "GPU readback completed without a surface");
        return;
    }
    if (!capture_dimensions(surface, &capacity)) {
        capture_fail(capture, "GPU readback dimensions exceed capture limits");
        SDL_DestroySurface(surface);
        return;
    }
    capture->result.width = (uint32_t)surface->w;
    capture->result.height = (uint32_t)surface->h;

    unsigned char *encoded = malloc(capacity);
    SDL_IOStream *io = encoded != NULL ? SDL_IOFromMem(encoded, capacity) : NULL;
    bool saved = io != NULL && image_codec_save_png_io(surface, io, false);
    Sint64 encoded_size = io != NULL ? SDL_TellIO(io) : -1;
    bool io_closed = io == NULL || SDL_CloseIO(io);
    SDL_DestroySurface(surface);
    if (!saved || !io_closed || encoded_size <= 0 || (uint64_t)encoded_size > capacity) {
        free(encoded);
        capture_fail(capture, "PNG encoding failed or exceeded its bounded buffer");
        return;
    }

    char digest[65];
    size_t size = (size_t)encoded_size;
    if (!capture_sha256(encoded, size, digest)) {
        free(encoded);
        capture_fail(capture, "PNG SHA-256 computation failed");
        return;
    }
    bool written = fwrite(encoded, 1, size, capture->stream) == size;
    free(encoded);
    written = written && fflush(capture->stream) == 0;
    if (!capture_close(capture)) {
        written = false;
    }
    if (!written) {
        capture_fail(capture, "PNG output write, flush, or close failed");
        return;
    }
    capture->result.size_bytes = size;
    memcpy(capture->result.sha256, digest, sizeof(digest));
    capture->result.error[0] = '\0';
    capture->result.status = LIVE_MOVEMENT_CAPTURE_COMPLETE;
}

static void capture_cancel(void *userdata) {
    live_movement_capture_t *capture = userdata;
    if (capture->release_requested) {
        free(capture);
        return;
    }
    capture_fail(capture, "GPU readback was canceled");
}

live_movement_capture_t *
live_movement_capture_create(const char *absolute_path, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (!capture_absolute_path(absolute_path)) {
        capture_text(error, error_size, "capture path must be absolute");
        return NULL;
    }
    size_t path_size = strlen(absolute_path);
    if (path_size > LIVE_MOVEMENT_CAPTURE_PATH_MAX) {
        capture_text(error,
                     error_size,
                     "capture path exceeds %u bytes",
                     LIVE_MOVEMENT_CAPTURE_PATH_MAX);
        return NULL;
    }
    live_movement_capture_t *capture = calloc(1, sizeof(*capture));
    if (capture == NULL) {
        capture_text(error, error_size, "could not allocate capture job");
        return NULL;
    }
    memcpy(capture->path, absolute_path, path_size + 1U);
    capture->result.path = capture->path;
    capture->result.status = LIVE_MOVEMENT_CAPTURE_READY;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef WIN32
    flags |= O_BINARY;
#endif
    int descriptor = open(capture->path, flags, 0600);
    if (descriptor < 0) {
        capture_text(error, error_size, "cannot exclusively create capture: %s", strerror(errno));
        free(capture);
        return NULL;
    }
    capture->stream = fdopen(descriptor, "wb");
    if (capture->stream == NULL) {
        capture_text(error, error_size, "cannot open capture stream: %s", strerror(errno));
        close(descriptor);
        free(capture);
        return NULL;
    }
    return capture;
}

bool live_movement_capture_request(live_movement_capture_t *capture) {
    if (capture == NULL || capture->result.status != LIVE_MOVEMENT_CAPTURE_READY ||
        capture->stream == NULL) {
        return false;
    }
    capture->result.status = LIVE_MOVEMENT_CAPTURE_PENDING;
    if (!gpu_renderer_readback_async(NULL, capture_complete, capture_cancel, capture)) {
        capture_fail(capture, "could not queue GPU readback");
        return false;
    }
    return true;
}

const live_movement_capture_result_t *
live_movement_capture_result(const live_movement_capture_t *capture) {
    return capture != NULL ? &capture->result : NULL;
}

void live_movement_capture_destroy(live_movement_capture_t *capture) {
    if (capture == NULL) {
        return;
    }
    if (capture->result.status == LIVE_MOVEMENT_CAPTURE_PENDING) {
        capture->release_requested = true;
        capture_close(capture);
        return;
    }
    capture_close(capture);
    free(capture);
}

#ifdef ATRINIK_LIVE_MOVEMENT_CAPTURE_TESTING
bool live_movement_capture_test_close_descriptor(live_movement_capture_t *capture) {
    return capture != NULL && capture->stream != NULL && close(fileno(capture->stream)) == 0;
}
#endif
