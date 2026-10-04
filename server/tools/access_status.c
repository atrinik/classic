/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <access_admin.h>
#include <access_bootstrap.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *data = NULL, *store = NULL, *certificate = NULL, *policy = NULL;
    if (argc != 9) { fprintf(stderr, "usage: atrinik-access-status --data-dir PATH --store-dir PATH --certificate PATH --policy open|protected\n"); return 2; }
    for (int i = 1; i < argc; i += 2) {
        const char **target = NULL;
        if (!strcmp(argv[i], "--data-dir")) target = &data;
        else if (!strcmp(argv[i], "--store-dir")) target = &store;
        else if (!strcmp(argv[i], "--certificate")) target = &certificate;
        else if (!strcmp(argv[i], "--policy")) target = &policy;
        if (!target || *target) return 2;
        *target = argv[i + 1];
    }
    if (!data || !store || !certificate || !policy || (strcmp(policy, "open") && strcmp(policy, "protected"))) return 2;
    int lock = access_state_lock(data);
    if (lock < 0) { fprintf(stderr, "access state unavailable\n"); return 1; }
    access_status_t status = {.schema_version = ACCESS_STORE_SCHEMA, .protected_policy = !strcmp(policy, "protected")};
    bool ok = access_bootstrap_certificate_identity(certificate, status.server_identity), absent = false;
    if (ok) {
        int fd = access_bootstrap_open_path(store, true, &absent);
        if (fd >= 0) {
            close(fd);
            uint8_t identity[32]; memcpy(identity, status.server_identity, sizeof(identity));
            ok = access_store_inspect(store, identity, status.protected_policy, &status) == ACCESS_COMMITTED;
        } else ok = absent && !status.protected_policy;
    }
    char output[1024]; size_t length = 0;
    if (ok) ok = access_admin_status_encode(&status, absent, output, sizeof(output), &length);
    if (ok) ok = fwrite(output, 1, length, stdout) == length && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    access_state_unlock(lock);
    if (!ok) fprintf(stderr, "access state unavailable\n");
    return ok ? 0 : 1;
}
