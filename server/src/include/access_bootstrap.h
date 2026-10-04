/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ATRINIK_ACCESS_BOOTSTRAP_H
#define ATRINIK_ACCESS_BOOTSTRAP_H
#include <stdbool.h>
#include <stdint.h>

/* Linux only. Owns/releases the exclusive DATA lock: call before ordinary
 * startup acquires that lock, then exit on BOTH success and failure. Reads the
 * existing DATA/quic-identity.pem; never creates/replaces an identity. NULL or
 * empty store selects DATA/access-tokens; explicit store must be absolute.
 * Refuses any existing store leaf. Failure may leave a newly created leaf or
 * committed snapshot for explicit operator inspection; never retries/reset.
 * No listeners, plugins, route callbacks, token issuance or publication. */
bool access_bootstrap_initialize(const char *data_directory,
    const char *store_directory, bool protected_policy);

/* Read-only shared stopped-state helpers. Absolute, bounded paths only;
 * trusted root/euid ancestry, no links/dot components. Returned descriptor is
 * caller-owned. absent is true ONLY for a missing final component. */
int access_bootstrap_open_path(const char *path, bool directory, bool *absent);
bool access_bootstrap_certificate_identity(const char *path, uint8_t identity[32]);
#endif
