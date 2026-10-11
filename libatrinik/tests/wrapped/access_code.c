/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Load declarations before redirecting calls in the production implementation. */
#include <openssl/rand.h>

int __wrap_RAND_priv_bytes(unsigned char *out, int size);
#define RAND_priv_bytes __wrap_RAND_priv_bytes
#include "../../access_code.c"
