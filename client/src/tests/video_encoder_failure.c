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

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fprintf(stderr, "%d\n", __LINE__); \
            return 1;                          \
        }                                      \
    } while (0)

typedef enum encoder_mock_mode {
    ENCODER_MOCK_FAIL,
    ENCODER_MOCK_IGNORED_SHORT_WRITE,
} encoder_mock_mode_t;

static encoder_mock_mode_t encoder_mock_mode;

bool IMG_SaveJPG_IO(SDL_Surface *surface, SDL_IOStream *stream, bool close_stream, int quality) {
    static const unsigned char jpeg[] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    static const unsigned char overflow[65537U];

    if (surface == NULL || stream == NULL || close_stream || quality != 85 ||
        SDL_WriteIO(stream, jpeg, sizeof(jpeg)) != sizeof(jpeg)) {
        return false;
    }
    if (encoder_mock_mode == ENCODER_MOCK_FAIL) {
        return false;
    }
    /* Emulate a codec backend that ignores its destination's short write. */
    (void)SDL_WriteIO(stream, overflow, sizeof(overflow));
    return true;
}

#ifdef __linux__

#define FIXTURE_PATH_MAX 4096U
#define AVI_EMPTY_SIZE 232U

static void put_u32(unsigned char *destination, uint32_t value) {
    destination[0] = (unsigned char)value;
    destination[1] = (unsigned char)(value >> 8U);
    destination[2] = (unsigned char)(value >> 16U);
    destination[3] = (unsigned char)(value >> 24U);
}

static uint32_t get_u32(const unsigned char *source) {
    return (uint32_t)source[0] | (uint32_t)source[1] << 8U |
           (uint32_t)source[2] << 16U | (uint32_t)source[3] << 24U;
}

static bool fixture_path(char path[FIXTURE_PATH_MAX], const char *label, bool preserve) {
    char working[FIXTURE_PATH_MAX];
    if (getcwd(working, sizeof(working)) == NULL ||
        snprintf(path, FIXTURE_PATH_MAX, "%s/video-encoder-%s-XXXXXX", working, label) >=
            (int)FIXTURE_PATH_MAX) {
        return false;
    }
    int descriptor = mkstemp(path);
    if (descriptor < 0 || close(descriptor) != 0) {
        return false;
    }
    return preserve || unlink(path) == 0;
}

static bool write_packet(const char *path) {
    unsigned char packet[28] = {0};
    memcpy(packet, "AVR1FRAM", 8U);
    put_u32(packet + 8U, 0U);
    put_u32(packet + 12U, 1U);
    put_u32(packet + 16U, 1U);
    put_u32(packet + 20U, 4U);
    packet[24] = 0x10U;
    packet[25] = 0x20U;
    packet[26] = 0x30U;
    packet[27] = 0xffU;

    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        return false;
    }
    bool written = fwrite(packet, 1U, sizeof(packet), stream) == sizeof(packet);
    return fclose(stream) == 0 && written;
}

static bool read_exact_file(const char *path,
                            unsigned char *contents,
                            size_t capacity,
                            size_t *size) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    *size = fread(contents, 1U, capacity, stream);
    bool complete = !ferror(stream) && fgetc(stream) == EOF;
    return fclose(stream) == 0 && complete;
}

static bool empty_partial_avi(const char *path) {
    unsigned char contents[AVI_EMPTY_SIZE];
    size_t size = 0U;
    return read_exact_file(path, contents, sizeof(contents), &size) &&
           size == sizeof(contents) && memcmp(contents, "RIFF", 4U) == 0 &&
           get_u32(contents + 4U) == AVI_EMPTY_SIZE - 8U &&
           memcmp(contents + 8U, "AVI ", 4U) == 0 &&
           get_u32(contents + 48U) == 0U && get_u32(contents + 140U) == 0U &&
           memcmp(contents + 212U, "LIST", 4U) == 0 &&
           get_u32(contents + 216U) == 4U && memcmp(contents + 220U, "movi", 4U) == 0 &&
           memcmp(contents + 224U, "idx1", 4U) == 0 && get_u32(contents + 228U) == 0U;
}

static int child_main(const char *mode, const char *output_path) {
    if (strcmp(mode, "fail") == 0) {
        encoder_mock_mode = ENCODER_MOCK_FAIL;
    } else if (strcmp(mode, "short-write") == 0) {
        encoder_mock_mode = ENCODER_MOCK_IGNORED_SHORT_WRITE;
    } else {
        return 120;
    }
    return video_encoder_main(output_path);
}

static int run_failure_case(const char *mode) {
    char packet_path[FIXTURE_PATH_MAX];
    char status_path[FIXTURE_PATH_MAX];
    char output_path[FIXTURE_PATH_MAX];
    REQUIRE(fixture_path(packet_path, "packet", true));
    REQUIRE(fixture_path(status_path, "status", true));
    REQUIRE(fixture_path(output_path, "output", false));
    REQUIRE(write_packet(packet_path));

    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int packet = open(packet_path, O_RDONLY);
        int status = open(status_path, O_WRONLY | O_TRUNC);
        if (packet < 0 || status < 0 || dup2(packet, STDIN_FILENO) < 0 ||
            dup2(status, STDOUT_FILENO) < 0 || close(packet) != 0 || close(status) != 0) {
            _exit(121);
        }
        execl("/proc/self/exe",
              "video-encoder-failure-tests",
              "--encoder-failure-child",
              mode,
              output_path,
              (char *)NULL);
        _exit(122);
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 1);

    unsigned char output_status[32];
    size_t output_status_size = 0U;
    REQUIRE(read_exact_file(status_path,
                            output_status,
                            sizeof(output_status),
                            &output_status_size));
    REQUIRE(output_status_size == sizeof("READY\nFAILED\n") - 1U);
    REQUIRE(memcmp(output_status, "READY\nFAILED\n", output_status_size) == 0);
    REQUIRE(empty_partial_avi(output_path));
    REQUIRE(unlink(packet_path) == 0);
    REQUIRE(unlink(status_path) == 0);
    REQUIRE(unlink(output_path) == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--encoder-failure-child") == 0) {
        return child_main(argv[2], argv[3]);
    }
    REQUIRE(argc == 1);
    REQUIRE(run_failure_case("fail") == 0);
    REQUIRE(run_failure_case("short-write") == 0);
    return 0;
}

#else

int main(void) {
    return 0;
}

#endif
