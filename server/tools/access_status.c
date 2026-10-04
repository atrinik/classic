/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <access_admin.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Pin the whole trusted ancestry. ENOENT is returned only for a missing final
 * component, never for an inaccessible or missing ancestor. */
static int open_path(const char *path, bool directory, bool *absent) {
    *absent = false;
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return -1;
    char copy[PATH_MAX]; memcpy(copy, path, strlen(path) + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = copy + 1, *slash;
    while ((slash = strchr(part, '/')) != NULL) {
        *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) return -1;
        struct stat st;
        if (fstat(next, &st) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 0022)) { close(next); return -1; }
        fd = next; part = slash + 1;
    }
    if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
    int result = openat(fd, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    int saved = errno;
    /* A dangling link is not absence. */
    if (result < 0 && saved == ENOENT) {
        struct stat st;
        *absent = fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
    }
    close(fd);
    return result;
}
static bool certificate_identity(const char *path, uint8_t identity[32]) {
    bool absent;
    int fd = open_path(path, false, &absent);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size > 0 && st.st_size <= 65536;
    BIO *bio = ok ? BIO_new_fd(fd, BIO_NOCLOSE) : NULL;
    X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
    unsigned length = 0;
    ok = cert && X509_digest(cert, EVP_sha256(), identity, &length) == 1 && length == 32;
    X509_free(cert); BIO_free(bio); close(fd);
    return ok;
}
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
    bool ok = certificate_identity(certificate, status.server_identity), absent = false;
    if (ok) {
        int fd = open_path(store, true, &absent);
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
