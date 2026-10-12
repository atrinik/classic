/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <toolkit/console.h>
#include <toolkit/string.h>

/* Force the same binary-stream reader used by Windows even when the test
 * platform has Readline and a native getline. */
#undef HAVE_READLINE
#undef getline
ssize_t console_test_getline(char **line, size_t *size, FILE *stream);
#define getline console_test_getline
#include "../../console.c"

bool console_test_read_line(FILE *stream);
bool console_test_read_line(FILE *stream) {
    return console_read_line(stream);
}
