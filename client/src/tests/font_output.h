/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ATRINIK_FONT_OUTPUT_H
#define ATRINIK_FONT_OUTPUT_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static FILE *font_output(const char *directory, const char *name) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    /* MSVCRT does not support fopen's C11 "x" mode. Create atomically before
     * attaching a binary stream, preserving existing evidence on every CRT. */
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
#endif
    if (descriptor < 0) {
        return NULL;
    }
#ifdef _WIN32
    FILE *file = _fdopen(descriptor, "wb");
#else
    FILE *file = fdopen(descriptor, "wb");
#endif
    if (file == NULL) {
        int error = errno;
#ifdef _WIN32
        _close(descriptor);
#else
        close(descriptor);
#endif
        /* Retain the created file as failed evidence. Unlinking its pathname
         * could remove a replacement belonging to another process. */
        errno = error;
    }
    return file;
}

#endif
