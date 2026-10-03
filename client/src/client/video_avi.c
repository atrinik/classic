/*****************************************************************************
 * Copyright 2026 The Atrinik Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *****************************************************************************/

#include <video_avi.h>

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_AVI_HEADER_SIZE 224U
#define VIDEO_AVI_MAX_FRAMES 108000U
#define VIDEO_AVI_MAX_JPEG_SIZE (64U * 1024U * 1024U)
#define VIDEO_AVI_MAX_FILE_SIZE UINT32_C(0x7fffffff)
#define VIDEO_AVI_MAX_FPS 1000U

#define VIDEO_AVI_RIFF_SIZE_OFFSET 4U
#define VIDEO_AVI_TOTAL_FRAMES_OFFSET 48U
#define VIDEO_AVI_STREAM_LENGTH_OFFSET 140U
#define VIDEO_AVI_MOVI_SIZE_OFFSET 216U
#define VIDEO_AVI_MOVI_TYPE_OFFSET 220U

struct video_avi {
    FILE *stream;
    long start;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t last_index;
    uint32_t maximum_jpeg_size;
    uint32_t file_limit;
    uint32_t *offsets;
    uint32_t *sizes;
    unsigned char *previous_jpeg;
    size_t previous_size;
    char error[160];
    bool have_frame;
    bool finished;
    bool finish_result;
};

static void video_avi_put_u16(unsigned char *data, uint16_t value) {
    data[0] = (unsigned char) value;
    data[1] = (unsigned char) (value >> 8U);
}

static void video_avi_put_u32(unsigned char *data, uint32_t value) {
    data[0] = (unsigned char) value;
    data[1] = (unsigned char) (value >> 8U);
    data[2] = (unsigned char) (value >> 16U);
    data[3] = (unsigned char) (value >> 24U);
}

static bool video_avi_fail(video_avi_t *avi, const char *format, ...) {
    if (avi != NULL && avi->error[0] == '\0') {
        va_list args;

        va_start(args, format);
        (void) vsnprintf(avi->error, sizeof(avi->error), format, args);
        va_end(args);
    }
    return false;
}

static bool video_avi_write(video_avi_t *avi, const void *data, size_t size) {
    if (size != 0U && fwrite(data, 1U, size, avi->stream) != size) {
        return video_avi_fail(avi, "failed to write AVI stream");
    }
    return true;
}

static bool video_avi_seek(video_avi_t *avi, uint32_t offset) {
    if (avi->start > LONG_MAX - (long) offset ||
        fseek(avi->stream, avi->start + (long) offset, SEEK_SET) != 0) {
        return video_avi_fail(avi, "failed to seek AVI stream");
    }
    return true;
}

static bool video_avi_write_u32(video_avi_t *avi, uint32_t value) {
    unsigned char data[4];

    video_avi_put_u32(data, value);
    return video_avi_write(avi, data, sizeof(data));
}

static void video_avi_make_header(video_avi_t *avi, unsigned char *header) {
    memset(header, 0, VIDEO_AVI_HEADER_SIZE);
    memcpy(header + 0U, "RIFF", 4U);
    memcpy(header + 8U, "AVI ", 4U);
    memcpy(header + 12U, "LIST", 4U);
    video_avi_put_u32(header + 16U, 192U);
    memcpy(header + 20U, "hdrl", 4U);
    memcpy(header + 24U, "avih", 4U);
    video_avi_put_u32(header + 28U, 56U);
    video_avi_put_u32(header + 32U, 1000000U / avi->fps);
    video_avi_put_u32(header + 44U, 0x10U);
    video_avi_put_u32(header + 56U, 1U);
    video_avi_put_u32(header + 60U, avi->maximum_jpeg_size);
    video_avi_put_u32(header + 64U, avi->width);
    video_avi_put_u32(header + 68U, avi->height);

    memcpy(header + 88U, "LIST", 4U);
    video_avi_put_u32(header + 92U, 116U);
    memcpy(header + 96U, "strl", 4U);
    memcpy(header + 100U, "strh", 4U);
    video_avi_put_u32(header + 104U, 56U);
    memcpy(header + 108U, "vids", 4U);
    memcpy(header + 112U, "MJPG", 4U);
    video_avi_put_u32(header + 128U, 1U);
    video_avi_put_u32(header + 132U, avi->fps);
    video_avi_put_u32(header + 144U, avi->maximum_jpeg_size);
    video_avi_put_u32(header + 148U, UINT32_MAX);
    video_avi_put_u16(header + 160U, (uint16_t) avi->width);
    video_avi_put_u16(header + 162U, (uint16_t) avi->height);

    memcpy(header + 164U, "strf", 4U);
    video_avi_put_u32(header + 168U, 40U);
    video_avi_put_u32(header + 172U, 40U);
    video_avi_put_u32(header + 176U, avi->width);
    video_avi_put_u32(header + 180U, avi->height);
    video_avi_put_u16(header + 184U, 1U);
    video_avi_put_u16(header + 186U, 24U);
    memcpy(header + 188U, "MJPG", 4U);
    video_avi_put_u32(header + 192U, avi->maximum_jpeg_size);

    memcpy(header + 212U, "LIST", 4U);
    memcpy(header + VIDEO_AVI_MOVI_TYPE_OFFSET, "movi", 4U);
}

video_avi_t *video_avi_open(FILE *stream, uint32_t width, uint32_t height, uint32_t fps) {
    video_avi_t *avi;
    unsigned char header[VIDEO_AVI_HEADER_SIZE];
    uint64_t maximum_jpeg_size;
    long start;

    if (stream == NULL || width == 0U || height == 0U || width > INT16_MAX ||
        height > INT16_MAX || fps == 0U || fps > VIDEO_AVI_MAX_FPS) {
        return NULL;
    }
    maximum_jpeg_size = (uint64_t) width * height * 4U + 64U * 1024U;
    if (maximum_jpeg_size > VIDEO_AVI_MAX_JPEG_SIZE) {
        maximum_jpeg_size = VIDEO_AVI_MAX_JPEG_SIZE;
    }
    start = ftell(stream);
    if (start < 0 || start > LONG_MAX - (long) VIDEO_AVI_HEADER_SIZE - 8L) {
        return NULL;
    }
    avi = calloc(1U, sizeof(*avi));
    if (avi == NULL) {
        return NULL;
    }
    avi->offsets = malloc(VIDEO_AVI_MAX_FRAMES * sizeof(*avi->offsets));
    avi->sizes = malloc(VIDEO_AVI_MAX_FRAMES * sizeof(*avi->sizes));
    if (avi->offsets == NULL || avi->sizes == NULL) {
        video_avi_free(avi);
        return NULL;
    }
    avi->stream = stream;
    avi->start = start;
    avi->width = width;
    avi->height = height;
    avi->fps = fps;
    avi->maximum_jpeg_size = (uint32_t) maximum_jpeg_size;
    avi->file_limit = VIDEO_AVI_MAX_FILE_SIZE;
    if ((uint64_t) (LONG_MAX - start) < avi->file_limit) {
        avi->file_limit = (uint32_t) (LONG_MAX - start);
    }
    video_avi_make_header(avi, header);
    if (!video_avi_write(avi, header, sizeof(header)) ||
        !video_avi_seek(avi, VIDEO_AVI_RIFF_SIZE_OFFSET) || !video_avi_write_u32(avi, 0U) ||
        ftell(stream) != start + 8L || !video_avi_seek(avi, VIDEO_AVI_HEADER_SIZE)) {
        video_avi_free(avi);
        return NULL;
    }
    return avi;
}

static bool video_avi_can_append(video_avi_t *avi, size_t jpeg_size) {
    uint64_t padded_size = jpeg_size + (jpeg_size & 1U);
    uint64_t body_end = VIDEO_AVI_HEADER_SIZE;
    uint64_t final_size;

    if (avi->frame_count != 0U) {
        uint32_t last = avi->frame_count - 1U;

        body_end = (uint64_t) avi->offsets[last] + VIDEO_AVI_MOVI_TYPE_OFFSET +
                   8U + avi->sizes[last] + (avi->sizes[last] & 1U);
    }
    final_size = body_end + 8U + padded_size + 8U +
                 ((uint64_t) avi->frame_count + 1U) * 16U;
    return avi->frame_count < VIDEO_AVI_MAX_FRAMES && final_size <= avi->file_limit;
}

static bool video_avi_emit(video_avi_t *avi, const unsigned char *jpeg, size_t size) {
    unsigned char chunk[8];
    unsigned char padding = 0U;
    uint64_t position;

    if (!video_avi_can_append(avi, size)) {
        return video_avi_fail(avi, "AVI recording limit reached");
    }
    position = VIDEO_AVI_HEADER_SIZE;
    if (avi->frame_count != 0U) {
        uint32_t last = avi->frame_count - 1U;

        position = (uint64_t) avi->offsets[last] + VIDEO_AVI_MOVI_TYPE_OFFSET +
                   8U + avi->sizes[last] + (avi->sizes[last] & 1U);
    }
    memcpy(chunk, "00dc", 4U);
    video_avi_put_u32(chunk + 4U, (uint32_t) size);
    if (!video_avi_write(avi, chunk, sizeof(chunk)) || !video_avi_write(avi, jpeg, size) ||
        ((size & 1U) != 0U && !video_avi_write(avi, &padding, 1U))) {
        return false;
    }
    avi->offsets[avi->frame_count] = (uint32_t) position - VIDEO_AVI_MOVI_TYPE_OFFSET;
    avi->sizes[avi->frame_count] = (uint32_t) size;
    avi->frame_count++;
    return true;
}

static bool video_avi_finalize(video_avi_t *avi) {
    unsigned char entry[16];
    uint64_t movi_end = VIDEO_AVI_HEADER_SIZE;
    uint64_t final_size;

    if (avi->frame_count != 0U) {
        uint32_t last = avi->frame_count - 1U;

        movi_end = (uint64_t) avi->offsets[last] + VIDEO_AVI_MOVI_TYPE_OFFSET +
                   8U + avi->sizes[last] + (avi->sizes[last] & 1U);
    }
    if (!video_avi_seek(avi, (uint32_t) movi_end) ||
        !video_avi_write(avi, "idx1", 4U) ||
        !video_avi_write_u32(avi, avi->frame_count * 16U)) {
        return false;
    }
    for (uint32_t i = 0U; i < avi->frame_count; i++) {
        memcpy(entry, "00dc", 4U);
        video_avi_put_u32(entry + 4U, 0x10U);
        video_avi_put_u32(entry + 8U, avi->offsets[i]);
        video_avi_put_u32(entry + 12U, avi->sizes[i]);
        if (!video_avi_write(avi, entry, sizeof(entry))) {
            return false;
        }
    }
    final_size = movi_end + 8U + (uint64_t) avi->frame_count * 16U;
    if (!video_avi_seek(avi, VIDEO_AVI_RIFF_SIZE_OFFSET) ||
        !video_avi_write_u32(avi, (uint32_t) final_size - 8U) ||
        !video_avi_seek(avi, VIDEO_AVI_TOTAL_FRAMES_OFFSET) ||
        !video_avi_write_u32(avi, avi->frame_count) ||
        !video_avi_seek(avi, VIDEO_AVI_STREAM_LENGTH_OFFSET) ||
        !video_avi_write_u32(avi, avi->frame_count) ||
        !video_avi_seek(avi, VIDEO_AVI_MOVI_SIZE_OFFSET) ||
        !video_avi_write_u32(avi, (uint32_t) movi_end - VIDEO_AVI_MOVI_TYPE_OFFSET) ||
        !video_avi_seek(avi, (uint32_t) final_size) || fflush(avi->stream) != 0) {
        return video_avi_fail(avi, "failed to finalize AVI stream");
    }
    return true;
}

static bool video_avi_stop_and_finalize(video_avi_t *avi, const char *message) {
    video_avi_fail(avi, "%s", message);
    avi->finished = true;
    clearerr(avi->stream);
    (void) video_avi_finalize(avi);
    avi->finish_result = false;
    return false;
}

bool video_avi_frame(video_avi_t *avi,
                     const unsigned char *jpeg,
                     size_t size,
                     uint32_t frame_index) {
    unsigned char *copy;

    if (avi == NULL || jpeg == NULL) {
        return false;
    }
    if (avi->finished || avi->error[0] != '\0') {
        return false;
    }
    if (size == 0U || size > avi->maximum_jpeg_size || size > UINT32_MAX) {
        return video_avi_fail(avi, "JPEG frame exceeds the recording limit");
    }
    if ((!avi->have_frame && frame_index != 0U) ||
        (avi->have_frame && frame_index <= avi->last_index)) {
        return video_avi_fail(avi, "frame indices must start at zero and increase");
    }
    if (frame_index >= VIDEO_AVI_MAX_FRAMES) {
        return video_avi_stop_and_finalize(avi, "AVI frame limit reached");
    }
    copy = malloc(size);
    if (copy == NULL) {
        return video_avi_fail(avi, "failed to copy JPEG frame");
    }
    memcpy(copy, jpeg, size);
    while (avi->have_frame && avi->last_index + 1U < frame_index) {
        if (!video_avi_emit(avi, avi->previous_jpeg, avi->previous_size)) {
            free(copy);
            return video_avi_stop_and_finalize(avi, "AVI recording limit reached");
        }
        avi->last_index++;
    }
    if (!video_avi_emit(avi, copy, size)) {
        free(copy);
        return video_avi_stop_and_finalize(avi, "AVI recording limit reached");
    }
    free(avi->previous_jpeg);
    avi->previous_jpeg = copy;
    avi->previous_size = size;
    avi->last_index = frame_index;
    avi->have_frame = true;
    return true;
}

bool video_avi_finish(video_avi_t *avi, uint32_t frame_count) {
    if (avi == NULL) {
        return false;
    }
    if (avi->finished) {
        return avi->finish_result;
    }
    if (avi->error[0] != '\0') {
        avi->finished = true;
        (void) video_avi_finalize(avi);
        return false;
    }
    if ((!avi->have_frame && frame_count != 0U) || frame_count < avi->frame_count) {
        return video_avi_stop_and_finalize(avi, "invalid final AVI frame count");
    }
    if (frame_count > VIDEO_AVI_MAX_FRAMES) {
        return video_avi_stop_and_finalize(avi, "AVI frame limit reached");
    }
    while (avi->frame_count < frame_count) {
        if (!video_avi_emit(avi, avi->previous_jpeg, avi->previous_size)) {
            return video_avi_stop_and_finalize(avi, "AVI recording limit reached");
        }
    }
    avi->finished = true;
    avi->finish_result = video_avi_finalize(avi);
    return avi->finish_result;
}

void video_avi_free(video_avi_t *avi) {
    if (avi == NULL) {
        return;
    }
    free(avi->previous_jpeg);
    free(avi->offsets);
    free(avi->sizes);
    free(avi);
}

const char *video_avi_error(const video_avi_t *avi) {
    return avi != NULL ? avi->error : "invalid AVI writer";
}

uint32_t video_avi_frames(const video_avi_t *avi) {
    return avi != NULL ? avi->frame_count : 0U;
}
