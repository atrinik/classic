/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <toolkit/porting.h>

static size_t successful_allocations = SIZE_MAX;

void console_test_fail_realloc_after(size_t count);
void console_test_fail_realloc_after(size_t count) {
    successful_allocations = count;
}

static void *console_test_realloc(void *pointer, size_t size) {
    if (successful_allocations == 0) {
        errno = ENOMEM;
        return NULL;
    }
    if (successful_allocations != SIZE_MAX) {
        successful_allocations--;
    }
    return realloc(pointer, size);
}

/* Keep the fallback independently testable on hosts with libc getline. */
#undef HAVE_GETLINE
#undef _getline
ssize_t console_test_getline(char **line, size_t *size, FILE *stream);
#define _getline console_test_getline
#define realloc console_test_realloc
#include "../../porting.c"
