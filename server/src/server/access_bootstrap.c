/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <access_bootstrap.h>
#include <access_tokens.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool bootstrap_trusted_directory(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
        (st.st_uid == 0 || st.st_uid == geteuid()) && !(st.st_mode & 0022);
}

int access_bootstrap_open_path(const char *path, bool directory, bool *absent)
{
    if (absent == NULL) return -1;
    *absent = false;
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return -1;
    char copy[PATH_MAX]; memcpy(copy, path, strlen(path) + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!bootstrap_trusted_directory(fd)) { close(fd); return -1; }
    char *part = copy + 1, *slash;
    while ((slash = strchr(part, '/')) != NULL) {
        *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) return -1;
        if (!bootstrap_trusted_directory(next)) { close(next); return -1; }
        fd = next; part = slash + 1;
    }
    if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
    int result = openat(fd, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    int saved = errno;
    if (result < 0 && saved == ENOENT) {
        struct stat st;
        *absent = fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
    }
    close(fd);
    if (result >= 0 && directory && !bootstrap_trusted_directory(result)) {
        close(result); return -1;
    }
    return result;
}

bool access_bootstrap_certificate_identity(const char *path, uint8_t identity[32])
{
    if (identity == NULL) return false;
    bool absent;
    int fd = access_bootstrap_open_path(path, false, &absent);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        (st.st_mode & 07777) == 0600 && st.st_nlink == 1 && st.st_size > 0 && st.st_size <= 65536;
    BIO *bio = ok ? BIO_new_fd(fd, BIO_NOCLOSE) : NULL;
    X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
    unsigned length = 0;
    ok = cert && X509_digest(cert, EVP_sha256(), identity, &length) == 1 && length == 32;
    X509_free(cert); BIO_free(bio);
    if (close(fd) != 0) ok = false;
    return ok;
}

/* Resolve only the leading ./ convenience, never realpath (which follows
 * symlinks). The checked traversal rejects embedded dot/dotdot components. */
static bool bootstrap_absolute_data(const char *path, char out[PATH_MAX])
{
    if (!path || !*path || strlen(path) >= PATH_MAX) return false;
    if (*path == '/') { memcpy(out, path, strlen(path) + 1); return true; }
    while (path[0] == '.' && path[1] == '/') path += 2;
    if (!*path || !getcwd(out, PATH_MAX)) return false;
    size_t length = strlen(out);
    int written = snprintf(out + length, PATH_MAX - length, "%s%s", length == 1 ? "" : "/", path);
    return written > 0 && (size_t) written < PATH_MAX - length;
}

bool access_bootstrap_initialize(const char *data_directory,
    const char *store_directory, bool protected_policy)
{
    char data[PATH_MAX], certificate[PATH_MAX], store_path[PATH_MAX];
    if (!bootstrap_absolute_data(data_directory, data)) return false;
    if (snprintf(certificate, sizeof(certificate), "%s/quic-identity.pem", data) >= (int) sizeof(certificate)) return false;
    if (store_directory && *store_directory) {
        if (*store_directory != '/' || strlen(store_directory) >= sizeof(store_path)) return false;
        memcpy(store_path, store_directory, strlen(store_directory) + 1);
    } else if (snprintf(store_path, sizeof(store_path), "%s/access-tokens", data) >= (int) sizeof(store_path)) return false;

    int lock = access_state_lock(data);
    if (lock < 0) return false;
    uint8_t identity[32];
    bool ok = access_bootstrap_certificate_identity(certificate, identity);
    int parent = -1;
    access_store_t *store = NULL;
    if (!ok) goto done;
    char parent_path[PATH_MAX]; memcpy(parent_path, store_path, strlen(store_path) + 1);
    char *leaf = strrchr(parent_path, '/');
    if (!leaf || !leaf[1] || !strcmp(leaf + 1, ".") || !strcmp(leaf + 1, "..")) { ok = false; goto done; }
    *leaf++ = '\0';
    bool absent;
    /* A leaf directly below / is deliberately unsupported. */
    parent = access_bootstrap_open_path(parent_path, true, &absent);
    if (parent < 0) { ok = false; goto done; }
    /* mkdirat is exclusive: even an empty existing directory is refused. */
    if (mkdirat(parent, leaf, 0700) != 0) { ok = false; goto done; }
    if (fsync(parent) != 0) { ok = false; goto done; }
    ok = access_store_open(&store, store_path, identity, protected_policy, true) == ACCESS_COMMITTED;
    if (ok) {
        access_status_t status = access_store_status(store);
        ok = status.integrity_ok && status.durability_ok && status.revision == 1 && status.pending_route_sync == 0;
    }
done:
    access_store_close(store);
    if (parent >= 0 && close(parent) != 0) ok = false;
    access_state_unlock(lock);
    return ok;
}
