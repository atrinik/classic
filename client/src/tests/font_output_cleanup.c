/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Inject a stream allocation failure after the real exclusive file open.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
static HANDLE opened_handle;
#else
#include <fcntl.h>
static int opened_descriptor;
#endif

static FILE *fail_fdopen(int descriptor, const char *mode) {
    (void)mode;
#ifdef _WIN32
    opened_handle = (HANDLE)_get_osfhandle(descriptor);
#else
    opened_descriptor = descriptor;
#endif
    errno = ENOMEM;
    return NULL;
}

#define fdopen fail_fdopen
#define _fdopen fail_fdopen
#include "font_output.h"
#undef fdopen
#undef _fdopen

int main(int argc, char **argv) {
    if (argc != 2 || font_output(argv[1], "font-output-test.bin") != NULL || errno != ENOMEM) {
        return 1;
    }
#ifdef _WIN32
    DWORD flags;
    if (GetHandleInformation(opened_handle, &flags) || GetLastError() != ERROR_INVALID_HANDLE) {
        return 1;
    }
#else
    if (fcntl(opened_descriptor, F_GETFD) != -1 || errno != EBADF) {
        return 1;
    }
#endif
    /* Earlier failed evidence must still exclude a later writer. */
    return font_output(argv[1], "font-output-test.bin") == NULL && errno == EEXIST ? 0 : 1;
}
