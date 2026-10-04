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

#include <video_encoder.h>

#include <video_avi.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <signal.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#define VIDEO_ENCODER_PATH_MAX 4095U
#define VIDEO_ENCODER_DIMENSION_MAX 4096U
#define VIDEO_ENCODER_PIXELS_MAX 8388608U
#define VIDEO_ENCODER_FRAME_COUNT_MAX 72000U
#define VIDEO_ENCODER_BYTES_PER_PIXEL 4U
#define VIDEO_ENCODER_JPEG_OVERHEAD 65536U
#define VIDEO_ENCODER_FPS 20U

static bool encoder_close_inherited(void) {
#if defined(__linux__) && defined(SYS_close_range)
    /* Linux 5.9+ closes the entire descriptor space, including descriptors
     * above a lowered RLIMIT_NOFILE. A bounded scan cannot guarantee that. */
    return syscall(SYS_close_range, 3U, UINT_MAX, 0U) == 0;
#else
    /* Other platforms need their own qualified handle/descriptor allowlist. */
    return false;
#endif
}

typedef struct encoder_output {
    unsigned char *data;
    size_t capacity;
    size_t position;
    size_t size;
    bool failed;
} encoder_output_t;

/* libjpeg's SDL_image destination may ignore a short SDL_WriteIO. Remember any
 * overflow independently so truncated output can never become an AVI frame. */
static size_t SDLCALL encoder_output_write(void *userdata,
                                           const void *data,
                                           size_t size,
                                           SDL_IOStatus *status) {
    encoder_output_t *output = userdata;
    if (size > output->capacity - output->position) {
        output->failed = true;
        *status = SDL_IO_STATUS_ERROR;
        return 0U;
    }
    memcpy(output->data + output->position, data, size);
    output->position += size;
    if (output->position > output->size) {
        output->size = output->position;
    }
    return size;
}

static Sint64 SDLCALL encoder_output_seek(void *userdata, Sint64 offset, SDL_IOWhence whence) {
    encoder_output_t *output = userdata;
    Sint64 base = whence == SDL_IO_SEEK_SET   ? 0
                  : whence == SDL_IO_SEEK_CUR ? (Sint64)output->position
                  : whence == SDL_IO_SEEK_END ? (Sint64)output->size
                                              : -1;
    if (base < 0 || offset < -base || offset > (Sint64)output->capacity - base) {
        output->failed = true;
        return -1;
    }
    output->position = (size_t)(base + offset);
    return base + offset;
}

static bool encoder_prepare_process(void) {
    if (!encoder_close_inherited()) {
        return false;
    }
#ifndef WIN32
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return false;
    }
#endif
    return true;
}

static uint32_t encoder_u32_le(const unsigned char bytes[4]) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U | (uint32_t)bytes[2] << 16U |
           (uint32_t)bytes[3] << 24U;
}

static bool encoder_read(void *destination, size_t size) {
    return size == 0U || fread(destination, 1, size, stdin) == size;
}

static bool encoder_absolute_path(const char *path) {
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

static FILE *encoder_open_output(const char *path) {
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef WIN32
    flags |= O_BINARY | O_NOINHERIT;
    int descriptor = _open(path, flags, _S_IREAD | _S_IWRITE);
    if (descriptor < 0) {
        return NULL;
    }
    FILE *stream = _fdopen(descriptor, "wb");
    if (stream == NULL) {
        _close(descriptor);
    }
#else
    int descriptor = open(path, flags, 0600);
    if (descriptor < 0) {
        return NULL;
    }
    FILE *stream = fdopen(descriptor, "wb");
    if (stream == NULL) {
        close(descriptor);
    }
#endif
    return stream;
}

static bool encoder_status(const char *status) {
    return fputs(status, stdout) != EOF && fflush(stdout) == 0;
}

static int encoder_failed(FILE *output,
                          video_avi_t *writer,
                          unsigned char *pixels,
                          unsigned char *jpeg,
                          const char *reason) {
    if (writer != NULL) {
        (void)video_avi_finish(writer, video_avi_frames(writer));
        video_avi_free(writer);
    }
    free(jpeg);
    free(pixels);
    if (output != NULL) {
        (void)fclose(output);
    }
    fputs(reason, stderr);
    fputc('\n', stderr);
    (void)encoder_status("FAILED\n");
    return 1;
}

int video_encoder_main(const char *output_path) {
    if (!encoder_prepare_process()) {
        return encoder_failed(NULL, NULL, NULL, NULL, "descriptor isolation failed");
    }
    if (!encoder_absolute_path(output_path)) {
        return encoder_failed(NULL, NULL, NULL, NULL, "invalid output path");
    }
    size_t path_size = strlen(output_path);
    if (path_size > VIDEO_ENCODER_PATH_MAX) {
        return encoder_failed(NULL, NULL, NULL, NULL, "invalid output path");
    }
    for (size_t i = 0; i < path_size; i++) {
        unsigned char byte = (unsigned char)output_path[i];
        if (byte < 32U || byte == 127U) {
            return encoder_failed(NULL, NULL, NULL, NULL, "invalid output path");
        }
    }

#ifdef WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0) {
        return encoder_failed(NULL, NULL, NULL, NULL, "binary stream setup failed");
    }
#endif

    FILE *output = encoder_open_output(output_path);
    if (output == NULL) {
        return encoder_failed(NULL, NULL, NULL, NULL, "output create failed");
    }
    if (!encoder_status("READY\n")) {
        return encoder_failed(output, NULL, NULL, NULL, "status output failed");
    }

    unsigned char protocol[4];
    if (!encoder_read(protocol, sizeof(protocol)) || memcmp(protocol, "AVR1", 4) != 0) {
        return encoder_failed(output, NULL, NULL, NULL, "invalid protocol header");
    }

    video_avi_t *writer = NULL;
    unsigned char *pixels = NULL;
    unsigned char *jpeg = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t previous_index = 0;
    bool have_frame = false;
    size_t pixel_bytes = 0;
    size_t jpeg_capacity = 0;

    for (;;) {
        unsigned char command[4];
        if (!encoder_read(command, sizeof(command))) {
            return encoder_failed(output, writer, pixels, jpeg, "truncated command");
        }
        if (memcmp(command, "STOP", 4) == 0) {
            unsigned char count_bytes[4];
            if (!encoder_read(count_bytes, sizeof(count_bytes))) {
                return encoder_failed(output, writer, pixels, jpeg, "truncated stop command");
            }
            uint32_t frame_count = encoder_u32_le(count_bytes);
            if (!have_frame || frame_count == 0U || frame_count > VIDEO_ENCODER_FRAME_COUNT_MAX ||
                frame_count <= previous_index) {
                return encoder_failed(output, writer, pixels, jpeg, "invalid final frame count");
            }
            if (!video_avi_finish(writer, frame_count)) {
                video_avi_free(writer);
                return encoder_failed(output, NULL, pixels, jpeg, "AVI finalization failed");
            }
            video_avi_free(writer);
            writer = NULL;
            free(jpeg);
            jpeg = NULL;
            free(pixels);
            pixels = NULL;
            bool flushed = fflush(output) == 0;
            bool closed = fclose(output) == 0;
            if (!flushed || !closed) {
                output = NULL;
                return encoder_failed(NULL, NULL, NULL, NULL, "output close failed");
            }
            output = NULL;
            if (!encoder_status("SAVED\n")) {
                fputs("status output failed\n", stderr);
                return 1;
            }
            return 0;
        }
        if (memcmp(command, "FRAM", 4) != 0) {
            return encoder_failed(output, writer, pixels, jpeg, "unknown command");
        }

        unsigned char fields[16];
        if (!encoder_read(fields, sizeof(fields))) {
            return encoder_failed(output, writer, pixels, jpeg, "truncated frame header");
        }
        uint32_t frame_index = encoder_u32_le(fields);
        uint32_t frame_width = encoder_u32_le(fields + 4);
        uint32_t frame_height = encoder_u32_le(fields + 8);
        uint32_t frame_bytes = encoder_u32_le(fields + 12);
        if ((!have_frame && frame_index != 0U) || (have_frame && frame_index <= previous_index) ||
            frame_index >= VIDEO_ENCODER_FRAME_COUNT_MAX) {
            return encoder_failed(output, writer, pixels, jpeg, "invalid frame index");
        }
        if (frame_width == 0U || frame_height == 0U || frame_width > VIDEO_ENCODER_DIMENSION_MAX ||
            frame_height > VIDEO_ENCODER_DIMENSION_MAX ||
            frame_height > VIDEO_ENCODER_PIXELS_MAX / frame_width) {
            return encoder_failed(output, writer, pixels, jpeg, "invalid frame dimensions");
        }
        size_t frame_pixels = (size_t)frame_width * frame_height;
        size_t expected_bytes = frame_pixels * VIDEO_ENCODER_BYTES_PER_PIXEL;
        if (frame_bytes != expected_bytes) {
            return encoder_failed(output, writer, pixels, jpeg, "invalid frame byte count");
        }
        if (have_frame && (frame_width != width || frame_height != height)) {
            return encoder_failed(output, writer, pixels, jpeg, "frame dimensions changed");
        }

        if (!have_frame) {
            width = frame_width;
            height = frame_height;
            pixel_bytes = expected_bytes;
            jpeg_capacity = pixel_bytes + VIDEO_ENCODER_JPEG_OVERHEAD;
            pixels = malloc(pixel_bytes);
            jpeg = malloc(jpeg_capacity);
            if (pixels == NULL || jpeg == NULL) {
                return encoder_failed(output, writer, pixels, jpeg, "frame allocation failed");
            }
            writer = video_avi_open(output, width, height, VIDEO_ENCODER_FPS);
            if (writer == NULL) {
                return encoder_failed(output, NULL, pixels, jpeg, "AVI initialization failed");
            }
        }
        if (!encoder_read(pixels, pixel_bytes)) {
            return encoder_failed(output, writer, pixels, jpeg, "truncated frame pixels");
        }

        SDL_Surface *surface = SDL_CreateSurfaceFrom((int)width,
                                                     (int)height,
                                                     SDL_PIXELFORMAT_RGBA32,
                                                     pixels,
                                                     (int)(width * VIDEO_ENCODER_BYTES_PER_PIXEL));
        encoder_output_t encoded_output = {.data = jpeg, .capacity = jpeg_capacity};
        SDL_IOStreamInterface interface;
        SDL_INIT_INTERFACE(&interface);
        interface.write = encoder_output_write;
        interface.seek = encoder_output_seek;
        SDL_IOStream *io = surface != NULL ? SDL_OpenIO(&interface, &encoded_output) : NULL;
        bool encoded = io != NULL && IMG_SaveJPG_IO(surface, io, false, 85);
        size_t encoded_size = encoded_output.size;
        bool io_closed = io == NULL || SDL_CloseIO(io);
        if (surface != NULL) {
            SDL_DestroySurface(surface);
        }
        if (!encoded || !io_closed || encoded_output.failed || encoded_size < 4U ||
            jpeg[0] != 0xffU || jpeg[1] != 0xd8U || jpeg[encoded_size - 2U] != 0xffU ||
            jpeg[encoded_size - 1U] != 0xd9U) {
            return encoder_failed(output, writer, pixels, jpeg, "JPEG encoding failed");
        }
        if (!video_avi_frame(writer, jpeg, encoded_size, frame_index)) {
            return encoder_failed(output, writer, pixels, jpeg, "AVI frame write failed");
        }
        previous_index = frame_index;
        have_frame = true;
    }
}
