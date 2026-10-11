/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdlib.h>

void __wrap_free(void *ptr);
#define free __wrap_free
#include "../../packet.c"
