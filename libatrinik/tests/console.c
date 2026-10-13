/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <toolkit/console.h>
#ifdef WIN32
#include <io.h>
#endif

#define require(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: requirement failed: %s\n",                   \
                    __FILE__, __LINE__, #condition);                              \
            return 1;                                                             \
        }                                                                         \
    } while (0)

bool console_test_read_line(FILE *stream);
ssize_t console_test_getline(char **line, size_t *size, FILE *stream);
void console_test_fail_realloc_after(size_t count);

static unsigned calls;
static char observed[512];

static void capture(const char *params) {
    calls++;
    snprintf(observed, sizeof(observed), "%s", params != NULL ? params : "<none>");
}

static FILE *input(const char *text) {
    int descriptors[2];
    /* Match redirected binary stdin without requiring a writable root drive
     * (the Windows CRT tmpfile default) or altering the test's own stdin. */
#ifdef WIN32
    if (_pipe(descriptors, 1024, _O_BINARY) != 0) {
#else
    if (pipe(descriptors) != 0) {
#endif
        return NULL;
    }
    size_t length = strlen(text);
    if (write(descriptors[1], text, (unsigned int)length) != (ssize_t)length) {
        close(descriptors[0]);
        close(descriptors[1]);
        return NULL;
    }
    close(descriptors[1]);
    FILE *stream = fdopen(descriptors[0], "rb");
    if (stream == NULL) {
        close(descriptors[0]);
    }
    return stream;
}

static int test_dispatch(void) {
    static const struct {
        const char *text;
        const char *params;
    } cases[] = {
        {"shutdown\n", "<none>"},
        {"shutdown\r\n", "<none>"},
        {"shutdown reason with spaces\r\n", "reason with spaces"},
        {"shutdown \r\n", "<none>"},
        {"shutdown", "<none>"},
        {"shutdown reason\rwith spaces\r\n", "reason\rwith spaces"},
    };
    toolkit_import(console);
    console_command_add("shutdown", capture, "test", "test");
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        FILE *stream = input(cases[i].text);
        require(stream != NULL);
        require(console_test_read_line(stream));
        console_command_handle();
        require(calls == i + 1);
        require(strcmp(observed, cases[i].params) == 0);
        require(!console_test_read_line(stream));
        console_command_handle();
        require(calls == i + 1);
        require(fclose(stream) == 0);
    }
    FILE *stream = input("\r\nunknown\r\nshutdown\r\nshutdown\n");
    require(stream != NULL);
    while (console_test_read_line(stream)) {
        console_command_handle();
    }
    require(calls == 8);
    require(fclose(stream) == 0);
    stream = input("");
    require(stream != NULL);
    require(!console_test_read_line(stream));
    console_command_handle();
    require(calls == 8);
    require(fclose(stream) == 0);
    toolkit_deinit();
    return 0;
}

static int test_fallback_bounds(void) {
    char text[258];
    memset(text, 'x', sizeof(text) - 2);
    text[sizeof(text) - 2] = '\n';
    text[sizeof(text) - 1] = '\0';
    FILE *stream = input(text);
    require(stream != NULL);
    /* A one-byte allocation forces capacity growth and checks the terminator
     * contract explicitly, as well as through sanitizer instrumentation. */
    char *line = malloc(1);
    require(line != NULL);
    size_t capacity = 1;
    ssize_t length = console_test_getline(&line, &capacity, stream);
    require(length == (ssize_t)strlen(text));
    require(capacity > (size_t)length);
    require(strcmp(line, text) == 0);
    require(console_test_getline(&line, &capacity, stream) == -1);
    require(feof(stream));
    free(line);
    require(fclose(stream) == 0);

    stream = input("\nreused\n");
    require(stream != NULL);
    line = malloc(1);
    require(line != NULL);
    capacity = 0;
    require(console_test_getline(&line, &capacity, stream) == 1);
    require(capacity >= 2 && strcmp(line, "\n") == 0);
    require(console_test_getline(&line, &capacity, stream) == 7);
    require(capacity >= 8 && strcmp(line, "reused\n") == 0);
    free(line);
    require(fclose(stream) == 0);

    stream = input("growth\n");
    require(stream != NULL);
    line = malloc(1);
    require(line != NULL);
    capacity = 1;
    console_test_fail_realloc_after(2);
    require(console_test_getline(&line, &capacity, stream) == -1);
    require(errno == ENOMEM);
    require(capacity == 3);
    require(memcmp(line, "gr", 2) == 0);
    /* This frees the last successful growth, including after realloc moved it. */
    free(line);
    console_test_fail_realloc_after(SIZE_MAX);
    require(fclose(stream) == 0);
    return 0;
}

int main(void) {
    require(test_dispatch() == 0);
    require(test_fallback_bounds() == 0);
    return 0;
}
