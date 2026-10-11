/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Keep Darwin's libc assembler aliases on the real declarations only. */
#include <unistd.h>

int __wrap_fsync(int fd);
int __wrap_close(int fd);
#define fsync __wrap_fsync
#define close __wrap_close
#include "../../path.c"
