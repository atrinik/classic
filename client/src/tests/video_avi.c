#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*****************************************************************************
 * Copyright 2026 The Atrinik Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *****************************************************************************/

/** @file Standalone structure and boundary tests for the MJPEG AVI writer. */

#include <video_avi.h>

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

static uint16_t test_u16(const unsigned char *data) {
    return (uint16_t)data[0] | (uint16_t)data[1] << 8U;
}

static uint32_t test_u32(const unsigned char *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8U | (uint32_t)data[2] << 16U |
           (uint32_t)data[3] << 24U;
}

static unsigned char *test_read_file(FILE *stream, size_t *size) {
    long end;
    unsigned char *data;

    TEST_CHECK(fseek(stream, 0L, SEEK_END) == 0);
    end = ftell(stream);
    TEST_CHECK(end >= 0);
    TEST_CHECK(fseek(stream, 0L, SEEK_SET) == 0);
    data = malloc((size_t)end);
    TEST_CHECK(data != NULL);
    TEST_CHECK(fread(data, 1U, (size_t)end, stream) == (size_t)end);
    *size = (size_t)end;
    return data;
}

static void test_sparse_timeline_and_structure(void) {
    static const unsigned char first[] = {0xffU, 0xd8U, 'A', 0xffU, 0xd9U};
    static const unsigned char second[] = {0xffU, 0xd8U, 'B', 'B', 0xffU, 0xd9U};
    static const unsigned char *expected[] = {first, first, first, second, second};
    static const uint32_t expected_sizes[] = {sizeof(first),
                                              sizeof(first),
                                              sizeof(first),
                                              sizeof(second),
                                              sizeof(second)};
    FILE *stream = tmpfile();
    video_avi_t *avi;
    unsigned char *data;
    size_t size;
    size_t cursor = 224U;
    uint32_t offsets[5];

    TEST_CHECK(stream != NULL);
    avi = video_avi_open(stream, 320U, 200U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_frame(avi, first, sizeof(first), 0U));
    TEST_CHECK(video_avi_frame(avi, second, sizeof(second), 3U));
    TEST_CHECK(video_avi_frames(avi) == 4U);
    TEST_CHECK(video_avi_finish(avi, 5U));
    TEST_CHECK(video_avi_finish(avi, 5U));
    TEST_CHECK(video_avi_frames(avi) == 5U);
    TEST_CHECK(video_avi_error(avi)[0] == '\0');

    data = test_read_file(stream, &size);
    TEST_CHECK(size >= 224U + 8U + 5U * 16U);
    TEST_CHECK(memcmp(data, "RIFF", 4U) == 0);
    TEST_CHECK(test_u32(data + 4U) == (uint32_t)size - 8U);
    TEST_CHECK(memcmp(data + 8U, "AVI ", 4U) == 0);
    TEST_CHECK(memcmp(data + 12U, "LIST", 4U) == 0);
    TEST_CHECK(test_u32(data + 16U) == 192U);
    TEST_CHECK(memcmp(data + 20U, "hdrl", 4U) == 0);
    TEST_CHECK(memcmp(data + 24U, "avih", 4U) == 0);
    TEST_CHECK(test_u32(data + 32U) == 33333U);
    TEST_CHECK(test_u32(data + 48U) == 5U);
    TEST_CHECK(test_u32(data + 56U) == 1U);
    TEST_CHECK(test_u32(data + 64U) == 320U);
    TEST_CHECK(test_u32(data + 68U) == 200U);
    TEST_CHECK(memcmp(data + 88U, "LIST", 4U) == 0);
    TEST_CHECK(memcmp(data + 96U, "strl", 4U) == 0);
    TEST_CHECK(memcmp(data + 100U, "strh", 4U) == 0);
    TEST_CHECK(memcmp(data + 108U, "vidsMJPG", 8U) == 0);
    TEST_CHECK(test_u32(data + 128U) == 1U);
    TEST_CHECK(test_u32(data + 132U) == 30U);
    TEST_CHECK(test_u32(data + 140U) == 5U);
    TEST_CHECK(test_u16(data + 160U) == 320U);
    TEST_CHECK(test_u16(data + 162U) == 200U);
    TEST_CHECK(memcmp(data + 164U, "strf", 4U) == 0);
    TEST_CHECK(test_u32(data + 176U) == 320U);
    TEST_CHECK(test_u32(data + 180U) == 200U);
    TEST_CHECK(test_u16(data + 184U) == 1U);
    TEST_CHECK(test_u16(data + 186U) == 24U);
    TEST_CHECK(memcmp(data + 188U, "MJPG", 4U) == 0);
    TEST_CHECK(memcmp(data + 212U, "LIST", 4U) == 0);
    TEST_CHECK(memcmp(data + 220U, "movi", 4U) == 0);

    for (size_t i = 0U; i < 5U; i++) {
        offsets[i] = (uint32_t)cursor - 220U;
        TEST_CHECK(memcmp(data + cursor, "00dc", 4U) == 0);
        TEST_CHECK(test_u32(data + cursor + 4U) == expected_sizes[i]);
        TEST_CHECK(memcmp(data + cursor + 8U, expected[i], expected_sizes[i]) == 0);
        cursor += 8U + expected_sizes[i] + (expected_sizes[i] & 1U);
    }
    TEST_CHECK(test_u32(data + 216U) == (uint32_t)cursor - 220U);
    TEST_CHECK(memcmp(data + cursor, "idx1", 4U) == 0);
    TEST_CHECK(test_u32(data + cursor + 4U) == 5U * 16U);
    cursor += 8U;
    for (size_t i = 0U; i < 5U; i++) {
        TEST_CHECK(memcmp(data + cursor, "00dc", 4U) == 0);
        TEST_CHECK(test_u32(data + cursor + 4U) == 0x10U);
        TEST_CHECK(test_u32(data + cursor + 8U) == offsets[i]);
        TEST_CHECK(test_u32(data + cursor + 12U) == expected_sizes[i]);
        cursor += 16U;
    }
    TEST_CHECK(cursor == size);

    free(data);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_empty_file(void) {
    FILE *stream = tmpfile();
    video_avi_t *avi;
    unsigned char *data;
    size_t size;

    TEST_CHECK(stream != NULL);
    avi = video_avi_open(stream, 1U, 1U, 1U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_finish(avi, 0U));
    data = test_read_file(stream, &size);
    TEST_CHECK(size == 232U);
    TEST_CHECK(test_u32(data + 48U) == 0U);
    TEST_CHECK(test_u32(data + 216U) == 4U);
    TEST_CHECK(memcmp(data + 224U, "idx1", 4U) == 0);
    TEST_CHECK(test_u32(data + 228U) == 0U);
    free(data);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_index_rejections(void) {
    static const unsigned char jpeg[] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    FILE *stream = tmpfile();
    video_avi_t *avi;

    TEST_CHECK(stream != NULL);
    avi = video_avi_open(stream, 10U, 10U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(!video_avi_frame(avi, jpeg, sizeof(jpeg), 1U));
    TEST_CHECK(strstr(video_avi_error(avi), "start at zero") != NULL);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);

    stream = tmpfile();
    TEST_CHECK(stream != NULL);
    avi = video_avi_open(stream, 10U, 10U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_frame(avi, jpeg, sizeof(jpeg), 0U));
    TEST_CHECK(!video_avi_frame(avi, jpeg, sizeof(jpeg), 0U));
    TEST_CHECK(!video_avi_finish(avi, 1U));
    TEST_CHECK(video_avi_frames(avi) == 1U);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_input_limits_and_write_failure(const char *readonly_path) {
    static const unsigned char byte = 0U;
    FILE *stream = tmpfile();
    video_avi_t *avi;
    FILE *readonly;

    TEST_CHECK(stream != NULL);
    TEST_CHECK(video_avi_open(stream, 0U, 1U, 30U) == NULL);
    TEST_CHECK(video_avi_open(stream, 1U, 0U, 30U) == NULL);
    TEST_CHECK(video_avi_open(stream, 32768U, 1U, 30U) == NULL);
    TEST_CHECK(video_avi_open(stream, 1U, 32768U, 30U) == NULL);
    TEST_CHECK(video_avi_open(stream, 1U, 1U, 0U) == NULL);
    TEST_CHECK(video_avi_open(stream, 1U, 1U, 1001U) == NULL);
    avi = video_avi_open(stream, 1U, 1U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(!video_avi_frame(avi, &byte, 64U * 1024U + 5U, 0U));
    TEST_CHECK(strstr(video_avi_error(avi), "JPEG frame") != NULL);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);

    readonly = fopen(readonly_path, "rb");
    TEST_CHECK(readonly != NULL);
    TEST_CHECK(video_avi_open(readonly, 1U, 1U, 30U) == NULL);
    TEST_CHECK(ferror(readonly));
    TEST_CHECK(fclose(readonly) == 0);
}

static void test_frame_cap_finalizes_complete_prefix(void) {
    static const unsigned char jpeg[] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    FILE *stream = tmpfile();
    video_avi_t *avi;
    unsigned char *data;
    size_t size;

    TEST_CHECK(stream != NULL);
    avi = video_avi_open(stream, 1U, 1U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_frame(avi, jpeg, sizeof(jpeg), 0U));
    TEST_CHECK(!video_avi_finish(avi, 108001U));
    TEST_CHECK(strstr(video_avi_error(avi), "frame limit") != NULL);
    TEST_CHECK(video_avi_frames(avi) == 1U);
    data = test_read_file(stream, &size);
    TEST_CHECK(test_u32(data + 4U) == (uint32_t)size - 8U);
    TEST_CHECK(test_u32(data + 48U) == 1U);
    TEST_CHECK(memcmp(data + 236U, "idx1", 4U) == 0);
    TEST_CHECK(test_u32(data + 240U) == 16U);
    free(data);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_nonzero_start_and_jpeg_copy(void) {
    unsigned char jpeg[] = {0xffU, 0xd8U, 'A', 0xffU, 0xd9U};
    FILE *stream = tmpfile();
    video_avi_t *avi;
    unsigned char *data;
    size_t size;

    TEST_CHECK(stream != NULL);
    TEST_CHECK(fwrite("pre", 1U, 3U, stream) == 3U);
    avi = video_avi_open(stream, 1U, 1U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_frame(avi, jpeg, sizeof(jpeg), 0U));
    jpeg[2] = 'X';
    TEST_CHECK(video_avi_finish(avi, 2U));
    data = test_read_file(stream, &size);
    TEST_CHECK(size > 3U + 224U);
    TEST_CHECK(memcmp(data, "preRIFF", 7U) == 0);
    TEST_CHECK(test_u32(data + 7U) == (uint32_t)size - 3U - 8U);
    TEST_CHECK(memcmp(data + 3U + 232U, "\xff\xd8\x41\xff\xd9", sizeof(jpeg)) == 0);
    TEST_CHECK(memcmp(data + 3U + 246U, "\xff\xd8\x41\xff\xd9", sizeof(jpeg)) == 0);
    free(data);
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

#ifdef __GLIBC__
#define TEST_AVI_HEADER_SIZE 224U
#define TEST_AVI_MAX_FRAMES 108000U
#define TEST_AVI_FILE_LIMIT UINT32_C(0x7fffffff)

typedef struct test_stream {
    unsigned char data[512];
    off64_t position;
    size_t length;
    size_t fail_after;
} test_stream_t;

typedef struct test_virtual_stream {
    unsigned char header[TEST_AVI_HEADER_SIZE];
    unsigned char *index;
    size_t index_capacity;
    size_t index_size;
    off64_t index_start;
    off64_t position;
    off64_t length;
} test_virtual_stream_t;

static ssize_t test_stream_write(void *context, const char *data, size_t size) {
    test_stream_t *stream = context;
    size_t allowed = size;

    if ((size_t)stream->position >= stream->fail_after) {
        return -1;
    }
    if (allowed > stream->fail_after - (size_t)stream->position) {
        allowed = stream->fail_after - (size_t)stream->position;
    }
    memcpy(stream->data + stream->position, data, allowed);
    stream->position += (off64_t)allowed;
    if ((size_t)stream->position > stream->length) {
        stream->length = (size_t)stream->position;
    }
    return (ssize_t)allowed;
}

static int test_stream_seek(void *context, off64_t *offset, int origin) {
    test_stream_t *stream = context;
    off64_t position;

    if (origin == SEEK_SET) {
        position = *offset;
    } else if (origin == SEEK_CUR) {
        position = stream->position + *offset;
    } else if (origin == SEEK_END) {
        position = (off64_t)stream->length + *offset;
    } else {
        return -1;
    }
    if (position < 0 || (uint64_t)position > sizeof(stream->data)) {
        return -1;
    }
    stream->position = position;
    *offset = position;
    return 0;
}

static ssize_t test_virtual_stream_write(void *context, const char *data, size_t size) {
    test_virtual_stream_t *stream = context;

    if (stream->position < TEST_AVI_HEADER_SIZE) {
        size_t offset = (size_t)stream->position;
        size_t copy_size = size;

        if (copy_size > TEST_AVI_HEADER_SIZE - offset) {
            copy_size = TEST_AVI_HEADER_SIZE - offset;
        }
        memcpy(stream->header + offset, data, copy_size);
    }
    if (size == 4U && memcmp(data, "idx1", 4U) == 0) {
        stream->index_start = stream->position;
        stream->index_size = 0U;
    }
    if (stream->index_start >= 0 && stream->position >= stream->index_start) {
        uint64_t offset = (uint64_t)(stream->position - stream->index_start);

        TEST_CHECK(offset <= stream->index_capacity);
        TEST_CHECK(size <= stream->index_capacity - (size_t)offset);
        memcpy(stream->index + offset, data, size);
        if ((size_t)offset + size > stream->index_size) {
            stream->index_size = (size_t)offset + size;
        }
    }
    stream->position += (off64_t)size;
    if (stream->position > stream->length) {
        stream->length = stream->position;
    }
    return (ssize_t)size;
}

static int test_virtual_stream_seek(void *context, off64_t *offset, int origin) {
    test_virtual_stream_t *stream = context;
    off64_t position;

    if (origin == SEEK_SET) {
        position = *offset;
    } else if (origin == SEEK_CUR) {
        position = stream->position + *offset;
    } else if (origin == SEEK_END) {
        position = stream->length + *offset;
    } else {
        return -1;
    }
    if (position < 0) {
        return -1;
    }
    stream->position = position;
    *offset = position;
    return 0;
}

static FILE *test_virtual_stream_open(test_virtual_stream_t *stream) {
    cookie_io_functions_t functions = {
        .read = NULL,
        .write = test_virtual_stream_write,
        .seek = test_virtual_stream_seek,
        .close = NULL,
    };
    FILE *file;

    stream->index_capacity = 8U + TEST_AVI_MAX_FRAMES * 16U;
    stream->index = malloc(stream->index_capacity);
    TEST_CHECK(stream->index != NULL);
    stream->index_start = -1;
    file = fopencookie(stream, "w+b", functions);
    TEST_CHECK(file != NULL);
    TEST_CHECK(setvbuf(file, NULL, _IONBF, 0) == 0);
    return file;
}

static void test_partial_frame_write_preserves_prefix(void) {
    static const unsigned char jpeg[] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    static const unsigned char next_jpeg[64] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    test_stream_t backing = {.fail_after = 512U};
    cookie_io_functions_t functions = {
        .read = NULL,
        .write = test_stream_write,
        .seek = test_stream_seek,
        .close = NULL,
    };
    FILE *stream = fopencookie(&backing, "w+b", functions);
    video_avi_t *avi;

    TEST_CHECK(stream != NULL);
    TEST_CHECK(setvbuf(stream, NULL, _IONBF, 0) == 0);
    avi = video_avi_open(stream, 1U, 1U, 30U);
    TEST_CHECK(avi != NULL);
    TEST_CHECK(video_avi_frame(avi, jpeg, sizeof(jpeg), 0U));
    backing.fail_after = 264U;
    TEST_CHECK(!video_avi_frame(avi, next_jpeg, sizeof(next_jpeg), 1U));
    TEST_CHECK(strstr(video_avi_error(avi), "write AVI") != NULL);
    TEST_CHECK(video_avi_frames(avi) == 1U);
    TEST_CHECK(test_u32(backing.data + 4U) == 252U);
    TEST_CHECK(test_u32(backing.data + 48U) == 1U);
    TEST_CHECK(test_u32(backing.data + 216U) == 16U);
    TEST_CHECK(memcmp(backing.data + 236U, "idx1", 4U) == 0);
    TEST_CHECK(test_u32(backing.data + 240U) == 16U);
    TEST_CHECK(memcmp(backing.data + 244U, "00dc", 4U) == 0);
    TEST_CHECK(test_u32(backing.data + 252U) == 4U);
    TEST_CHECK(test_u32(backing.data + 256U) == sizeof(jpeg));
    TEST_CHECK(!video_avi_finish(avi, 1U));
    video_avi_free(avi);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_virtual_file_size_boundary(void) {
    const size_t jpeg_size = 1024U * 1024U;
    const uint64_t record_size = 8U + jpeg_size;
    const uint32_t expected_frames =
        (uint32_t)((TEST_AVI_FILE_LIMIT - TEST_AVI_HEADER_SIZE - 8U) / (record_size + 16U));
    unsigned char *jpeg = calloc(1U, jpeg_size);
    test_virtual_stream_t backing = {0};
    FILE *stream = test_virtual_stream_open(&backing);
    video_avi_t *avi;
    uint64_t expected_size;
    size_t last_entry;

    TEST_CHECK(jpeg != NULL);
    avi = video_avi_open(stream, 512U, 512U, 30U);
    TEST_CHECK(avi != NULL);
    for (uint32_t i = 0U; i < expected_frames; i++) {
        TEST_CHECK(video_avi_frame(avi, jpeg, jpeg_size, i));
    }
    TEST_CHECK(!video_avi_frame(avi, jpeg, jpeg_size, expected_frames));
    TEST_CHECK(strstr(video_avi_error(avi), "recording limit") != NULL);
    TEST_CHECK(video_avi_frames(avi) == expected_frames);
    expected_size = TEST_AVI_HEADER_SIZE + (uint64_t)expected_frames * record_size + 8U +
                    (uint64_t)expected_frames * 16U;
    TEST_CHECK(expected_size <= TEST_AVI_FILE_LIMIT);
    TEST_CHECK(expected_size + record_size + 16U > TEST_AVI_FILE_LIMIT);
    TEST_CHECK((uint64_t)backing.length == expected_size);
    TEST_CHECK(test_u32(backing.header + 4U) == expected_size - 8U);
    TEST_CHECK(test_u32(backing.header + 48U) == expected_frames);
    TEST_CHECK(test_u32(backing.header + 216U) == 4U + expected_frames * (uint32_t)record_size);
    TEST_CHECK(backing.index_size == 8U + expected_frames * 16U);
    TEST_CHECK(memcmp(backing.index, "idx1", 4U) == 0);
    TEST_CHECK(test_u32(backing.index + 4U) == expected_frames * 16U);
    TEST_CHECK(test_u32(backing.index + 16U) == 4U);
    TEST_CHECK(test_u32(backing.index + 20U) == jpeg_size);
    last_entry = 8U + (expected_frames - 1U) * 16U;
    TEST_CHECK(test_u32(backing.index + last_entry + 8U) ==
               4U + (expected_frames - 1U) * (uint32_t)record_size);
    TEST_CHECK(test_u32(backing.index + last_entry + 12U) == jpeg_size);
    TEST_CHECK(!video_avi_finish(avi, expected_frames));

    video_avi_free(avi);
    free(jpeg);
    free(backing.index);
    TEST_CHECK(fclose(stream) == 0);
}

static void test_virtual_frame_count_boundary(void) {
    static const unsigned char jpeg[] = {1U, 2U, 3U, 4U};
    test_virtual_stream_t backing = {0};
    FILE *stream = test_virtual_stream_open(&backing);
    video_avi_t *avi = video_avi_open(stream, 1U, 1U, 30U);
    uint64_t expected_size;
    size_t last_entry;

    TEST_CHECK(avi != NULL);
    for (uint32_t i = 0U; i < TEST_AVI_MAX_FRAMES; i++) {
        TEST_CHECK(video_avi_frame(avi, jpeg, sizeof(jpeg), i));
    }
    TEST_CHECK(!video_avi_frame(avi, jpeg, sizeof(jpeg), TEST_AVI_MAX_FRAMES));
    TEST_CHECK(strstr(video_avi_error(avi), "frame limit") != NULL);
    TEST_CHECK(video_avi_frames(avi) == TEST_AVI_MAX_FRAMES);
    expected_size = TEST_AVI_HEADER_SIZE + (uint64_t)TEST_AVI_MAX_FRAMES * 12U + 8U +
                    (uint64_t)TEST_AVI_MAX_FRAMES * 16U;
    TEST_CHECK((uint64_t)backing.length == expected_size);
    TEST_CHECK(test_u32(backing.header + 4U) == expected_size - 8U);
    TEST_CHECK(test_u32(backing.header + 48U) == TEST_AVI_MAX_FRAMES);
    TEST_CHECK(backing.index_size == 8U + TEST_AVI_MAX_FRAMES * 16U);
    TEST_CHECK(test_u32(backing.index + 4U) == TEST_AVI_MAX_FRAMES * 16U);
    last_entry = 8U + (TEST_AVI_MAX_FRAMES - 1U) * 16U;
    TEST_CHECK(test_u32(backing.index + last_entry + 8U) == 4U + (TEST_AVI_MAX_FRAMES - 1U) * 12U);
    TEST_CHECK(test_u32(backing.index + last_entry + 12U) == sizeof(jpeg));

    video_avi_free(avi);
    free(backing.index);
    TEST_CHECK(fclose(stream) == 0);
}
#endif

int main(int argc, char **argv) {
    TEST_CHECK(argc == 2);
    test_sparse_timeline_and_structure();
    test_empty_file();
    test_index_rejections();
    test_input_limits_and_write_failure(argv[1]);
    test_frame_cap_finalizes_complete_prefix();
    test_nonzero_start_and_jpeg_copy();
#ifdef __GLIBC__
    test_partial_frame_write_preserves_prefix();
    test_virtual_file_size_boundary();
    test_virtual_frame_count_boundary();
#endif
    return 0;
}
