/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned sync_calls, fail_sync;
static int bootstrap_fixture_fsync(int fd)
{
    if (++sync_calls == fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
#define fsync bootstrap_fixture_fsync
#include "../../server/access_tokens.c"
#include "../../server/access_bootstrap.c"
#undef fsync

/* Linked with --wrap=socket/--wrap=connect/--wrap=bind/--wrap=listen:
 * bootstrap must never reach transport initialization. */
int __wrap_socket(int domain, int type, int protocol)
{ (void) domain; (void) type; (void) protocol; abort(); }
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{ (void) fd; (void) address; (void) length; abort(); }
int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length)
{ (void) fd; (void) address; (void) length; abort(); }
int __wrap_listen(int fd, int backlog)
{ (void) fd; (void) backlog; abort(); }

static void certificate(const char *data)
{
    char path[PATH_MAX]; assert(snprintf(path, sizeof(path), "%s/quic-identity.pem", data) < (int) sizeof(path));
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1"); assert(key);
    X509 *cert = X509_new(); assert(cert);
    assert(X509_set_version(cert, 2));
    assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1));
    assert(X509_gmtime_adj(X509_getm_notBefore(cert), 0));
    assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600));
    assert(X509_set_pubkey(cert, key));
    X509_NAME *name = X509_get_subject_name(cert);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *) "offline fixture", -1, -1, 0));
    assert(X509_set_issuer_name(cert, name));
    assert(X509_sign(cert, key, EVP_sha256()) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    FILE *file = fdopen(fd, "w"); assert(file);
    assert(PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL));
    assert(PEM_write_X509(file, cert));
    assert(fclose(file) == 0);
    X509_free(cert); EVP_PKEY_free(key);
}
static void fresh(char data[PATH_MAX], const char *root, const char *name, bool identity)
{
    assert(snprintf(data, PATH_MAX, "%s/%s", root, name) < PATH_MAX);
    assert(mkdir(data, 0700) == 0);
    if (identity) certificate(data);
}
static void missing(const char *data)
{
    char path[PATH_MAX]; assert(snprintf(path, sizeof(path), "%s/access-tokens", data) < (int) sizeof(path));
    struct stat st; assert(lstat(path, &st) < 0 && errno == ENOENT);
}
static void inspect(const char *data, bool policy)
{
    char path[PATH_MAX], pem[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/access-tokens", data) < (int) sizeof(path));
    assert(snprintf(pem, sizeof(pem), "%s/quic-identity.pem", data) < (int) sizeof(pem));
    uint8_t identity[32]; assert(access_bootstrap_certificate_identity(pem, identity));
    access_status_t status; assert(access_store_inspect(path, identity, policy, &status) == ACCESS_COMMITTED);
    assert(status.integrity_ok && status.durability_ok && status.revision == 1);
    assert(status.protected_policy == policy && status.pending_route_sync == 0);
    access_store_t *store; assert(access_store_open(&store, path, identity, policy, false) == ACCESS_COMMITTED);
    access_token_info_t rows[1]; size_t count, next;
    assert(access_store_list(store, status.revision, 0, 1, rows, &count, &next) == ACCESS_COMMITTED);
    assert(count == 0); access_store_close(store);
}
int main(int argc, char **argv)
{
    assert(argc <= 2);
    char base[PATH_MAX], root[PATH_MAX], data[PATH_MAX], path[PATH_MAX], other[PATH_MAX];
    if (argc == 2) assert(snprintf(base, sizeof(base), "%s", argv[1]) < (int) sizeof(base));
    else assert(getcwd(base, sizeof(base)));
    assert(snprintf(root, sizeof(root), "%s/access-bootstrap-XXXXXX", base) < (int) sizeof(root));
    umask(077); assert(mkdtemp(root));
    fresh(data, root, "ok", true);
    assert(access_bootstrap_initialize(data, NULL, true)); inspect(data, true);
    assert(!access_bootstrap_initialize(data, NULL, true)); inspect(data, true);
    fresh(data, root, "relative", true);
    assert(chdir(root) == 0);
    assert(access_bootstrap_initialize("./relative", "", false)); inspect(data, false);
    fresh(data, root, "missing-identity", false);
    assert(!access_bootstrap_initialize(data, NULL, true)); missing(data);
    fresh(data, root, "locked", true);
    int lock = access_state_lock(data); assert(lock >= 0);
    assert(!access_bootstrap_initialize(data, NULL, true)); missing(data); access_state_unlock(lock);
    fresh(data, root, "unsafe-data", true); assert(chmod(data, 0770) == 0);
    assert(!access_bootstrap_initialize(data, NULL, true)); missing(data);
    fresh(data, root, "unsafe-certificate", true);
    assert(snprintf(path, sizeof(path), "%s/quic-identity.pem", data) < (int) sizeof(path));
    assert(chmod(path, 0644) == 0); assert(!access_bootstrap_initialize(data, NULL, true)); missing(data);
    fresh(data, root, "symlink-certificate", false);
    assert(snprintf(path, sizeof(path), "%s/quic-identity.pem", data) < (int) sizeof(path));
    assert(symlink("../ok/quic-identity.pem", path) == 0);
    assert(!access_bootstrap_initialize(data, NULL, true)); missing(data);
    fresh(data, root, "symlink-store", true);
    assert(snprintf(path, sizeof(path), "%s/access-tokens", data) < (int) sizeof(path));
    assert(symlink("../ok/access-tokens", path) == 0);
    assert(!access_bootstrap_initialize(data, NULL, true));
    assert(snprintf(other, sizeof(other), "%s/ok", root) < (int) sizeof(other)); inspect(other, true);
    fresh(data, root, "unsafe-parent", true);
    assert(snprintf(path, sizeof(path), "%s/unsafe-parent-link", root) < (int) sizeof(path));
    assert(symlink("unsafe-parent", path) == 0);
    assert(!access_bootstrap_initialize(path, NULL, true)); missing(data);
    assert(snprintf(other, sizeof(other), "%s/parent", data) < (int) sizeof(other));
    assert(mkdir(other, 0770) == 0); assert(chmod(other, 0770) == 0);
    assert(snprintf(path, sizeof(path), "%s/access-tokens", other) < (int) sizeof(path));
    assert(!access_bootstrap_initialize(data, path, true));
    struct stat st; assert(lstat(path, &st) < 0 && errno == ENOENT);
    assert(!access_bootstrap_initialize(data, "relative-store", true));
    assert(snprintf(path, sizeof(path), "%s/../ok", data) < (int) sizeof(path));
    assert(!access_bootstrap_initialize(path, NULL, true));
    fresh(data, root, "empty-existing", true);
    assert(snprintf(path, sizeof(path), "%s/access-tokens", data) < (int) sizeof(path));
    assert(mkdir(path, 0700) == 0); assert(!access_bootstrap_initialize(data, NULL, true));
    assert(rmdir(path) == 0); /* Refusal did not populate an existing empty leaf. */
    fresh(data, root, "explicit", true);
    assert(snprintf(path, sizeof(path), "%s/explicit-store", root) < (int) sizeof(path));
    assert(access_bootstrap_initialize(data, path, true));
    for (unsigned failure = 1; failure <= 3; failure++) {
        char name[32]; assert(snprintf(name, sizeof(name), "durability-%u", failure) > 0);
        fresh(data, root, name, true); sync_calls = 0; fail_sync = failure;
        assert(!access_bootstrap_initialize(data, NULL, true));
        assert(sync_calls == failure); fail_sync = 0;
        assert(!access_bootstrap_initialize(data, NULL, true));
        /* Partial/indeterminate state is retained, never silently reset. */
    }
    puts("offline bootstrap: success, empty state, preserved identity, lock/path/identity/durability failures passed");
    return 0;
}
