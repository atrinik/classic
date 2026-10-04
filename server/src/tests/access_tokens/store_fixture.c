/* Copyright (c) 2026 The Atrinik Project. SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Deterministic syscall faults operate on fixture-owned files only. */
static int fail_write, fail_file_sync, fail_directory_sync, fail_rename;
static ssize_t fixture_write(int fd, const void *data, size_t n)
{
    if (fail_write) { errno = ENOSPC; return -1; }
    return write(fd, data, n);
}
static int fixture_fsync(int fd)
{
    struct stat st; assert(fstat(fd, &st) == 0);
    if ((S_ISDIR(st.st_mode) && fail_directory_sync) || (!S_ISDIR(st.st_mode) && fail_file_sync)) {
        errno = EIO; return -1;
    }
    return fsync(fd);
}
static int fixture_renameat(int a, const char *b, int c, const char *d)
{
    if (fail_rename) { errno = EIO; return -1; }
    return renameat(a, b, c, d);
}
#define write fixture_write
#define fsync fixture_fsync
#define renameat fixture_renameat
#include "../../server/access_tokens.c"
#undef write
#undef fsync
#undef renameat

static const uint8_t identity[32] = {0xa5};
static const char *request1 = "11111111111111111111111111111111";
static const char *request2 = "22222222222222222222222222222222";
static const char *request3 = "33333333333333333333333333333333";
static const char *request4 = "44444444444444444444444444444444";
static unsigned routes;
static access_outcome_t accepted(void *context, const access_route_t *request)
{
    (void) context; assert(!request->revoke); assert(hex_id(request->token.token_id));
    routes++; return ACCESS_COMMITTED;
}
static uint64_t revision(access_store_t *s) { return access_store_status(s).revision; }
static access_store_t *fresh(char directory[64])
{
    strcpy(directory, "/tmp/atrinik-access-store-test-XXXXXX"); assert(mkdtemp(directory));
    access_store_t *s = NULL;
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(access_store_open(&s, directory, identity, true, true) == ACCESS_COMMITTED);
    return s;
}
static void cleanup(char *directory, access_store_t *s)
{
    access_store_close(s); char path[256]; snprintf(path, sizeof(path), "%s/%s", directory, SNAPSHOT);
    assert(unlink(path) == 0); assert(rmdir(directory) == 0);
}
static void basic(void)
{
    char directory[64]; access_store_t *s = fresh(directory), *other = NULL;
    assert(access_store_open(&other, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    access_result_t a = access_store_issue(s, request1, revision(s), "Family", false, 0, 100, accepted, NULL);
    assert(a.outcome == ACCESS_COMMITTED && strlen(a.code) == 16 && routes == 1 && !a.route_pending);
    uint64_t first_revision = revision(s);
    access_result_t replay = access_store_issue(s, request1, 1, "Family", false, 0, 100, accepted, NULL);
    assert(replay.outcome == ACCESS_SECRET_UNAVAILABLE && replay.code[0] == '\0' && routes == 1);
    replay = access_store_issue(s, request1, 1, "Changed label", false, 0, 100, accepted, NULL);
    assert(replay.outcome == ACCESS_CONFLICT);
    access_token_ref_t ref;
    assert(access_store_authorize(s, "0000000000000000", identity, 101, &ref) == ACCESS_DENIED);
    assert(revision(s) == first_revision);
    assert(access_store_authorize(s, a.code, identity, 101, &ref) == ACCESS_COMMITTED);
    assert(access_store_session_check(s, &ref, 101) == ACCESS_SESSION_VALID);
    access_token_info_t row;
    assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_COMMITTED);
    assert(row.history_count == 1 && row.last_admitted_at == 101 && !row.has_expiry);
    access_store_close(s); s = NULL;
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, a.code, identity, 102, &ref) == ACCESS_COMMITTED);
    access_result_t b = access_store_issue(s, request2, revision(s), "Temporary", true, 110, 102, accepted, NULL);
    assert(b.outcome == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 103, &ref) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 102, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 110, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 109, &ref) == ACCESS_DENIED);
    assert(access_store_expire(s, 110) == ACCESS_COMMITTED);
    access_result_t revoked = access_store_revoke(s, request3, revision(s), a.token.token_id, 110);
    assert(revoked.outcome == ACCESS_LOCALLY_REVOKED && revoked.route_pending);
    assert(access_store_authorize(s, a.code, identity, 111, &ref) == ACCESS_DENIED);
    access_result_t removed = access_store_remove(s, request4, revision(s), a.token.token_id, 111);
    assert(removed.outcome == ACCESS_PENDING);
    access_route_t outbox[ACCESS_OUTBOX_LIMIT]; size_t count = 0;
    assert(access_store_outbox(s, outbox, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED && count == 1);
    access_route_t incorrect = outbox[0]; incorrect.token.revision--;
    assert(access_store_route_ack(s, &incorrect, 111) == ACCESS_CONFLICT);
    assert(access_store_route_ack(s, outbox, 111) == ACCESS_COMMITTED);
    removed = access_store_remove(s, request4, revision(s), a.token.token_id, 111);
    assert(removed.outcome == ACCESS_COMMITTED && !removed.route_pending);
    assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_NOT_FOUND);
    assert(access_store_status(s).protected_policy);
    assert(access_store_flush_for_shutdown(s) == ACCESS_COMMITTED);
    assert(access_store_issue(s, request1, revision(s), "Family", false, 0, 112, accepted, NULL).outcome == ACCESS_UNAVAILABLE);
    access_result_cleanse(&a); access_result_cleanse(&b); cleanup(directory, s);
}
static void crash_recovery(void)
{
    char directory[64]; access_store_t *s = fresh(directory);
    access_result_t a = access_store_issue(s, request1, revision(s), "Interrupted", false, 0, 100, NULL, NULL);
    assert(a.outcome == ACCESS_PENDING && a.code[0] == '\0');
    access_store_close(s); assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    access_token_info_t row; assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_COMMITTED);
    assert(row.state == ACCESS_TOKEN_REVOKED && row.history_count == 0 && row.route_pending);
    a = access_store_result(s, request1); assert(a.outcome == ACCESS_LOCALLY_REVOKED && a.code[0] == '\0');
    cleanup(directory, s);
}
static void fault_case(int *fault, bool uncertain)
{
    char directory[64]; access_store_t *s = fresh(directory); uint64_t before = revision(s);
    *fault = 1;
    access_result_t a = access_store_issue(s, request1, before, "Fault", false, 0, 100, accepted, NULL);
    assert(a.outcome == (uncertain ? ACCESS_INDETERMINATE : ACCESS_SAVE_FAILED));
    assert(a.code[0] == '\0'); *fault = 0;
    assert(access_store_status(s).durability_ok == !uncertain);
    access_token_ref_t ref; assert(access_store_authorize(s, "0000000000000000", identity, 101, &ref) == ACCESS_DENIED);
    access_store_close(s); assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(revision(s) == (uncertain ? before + 2 : before));
    cleanup(directory, s);
}
static void privacy(void)
{
    char directory[64]; access_store_t *s = fresh(directory); access_store_close(s);
    char path[256], link[256]; snprintf(path, sizeof(path), "%s/%s", directory, SNAPSHOT);
    assert(chmod(path, 0644) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(chmod(path, 0600) == 0);
    uint8_t wrong[32] = {0}; assert(access_store_open(&s, directory, wrong, true, false) == ACCESS_UNAVAILABLE);
    snprintf(link, sizeof(link), "%s.sym", directory); assert(symlink(directory, link) == 0);
    assert(access_store_open(&s, link, identity, true, false) == ACCESS_UNAVAILABLE); assert(unlink(link) == 0);
    snprintf(link, sizeof(link), "%s/held", directory); assert(rename(path, link) == 0);
    assert(symlink("held", path) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(unlink(path) == 0); assert(rename(link, path) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    access_store_close(s); int fd = open(path, O_RDWR); assert(fd >= 0);
    char byte = 1; assert(pwrite(fd, &byte, 1, 32) == 1); assert(close(fd) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(unlink(path) == 0); assert(rmdir(directory) == 0);
}
static access_outcome_t concurrent_revoke(void *context, const access_route_t *request)
{
    access_store_t *s = context;
    access_result_t r = access_store_revoke(s, request2, revision(s), request->token.token_id, 101);
    assert(r.outcome == ACCESS_LOCALLY_REVOKED); return ACCESS_COMMITTED;
}
static void races(void)
{
    char directory[64]; access_store_t *s = fresh(directory);
    access_result_t a = access_store_issue(s, request1, revision(s), "Race", false, 0, 100, concurrent_revoke, s);
    assert(a.outcome == ACCESS_DENIED && a.code[0] == '\0');
    access_token_info_t rows[1]; size_t count, next;
    assert(access_store_list(s, revision(s), 0, 1, rows, &count, &next) == ACCESS_COMMITTED);
    assert(count == 1 && rows[0].state == ACCESS_TOKEN_REVOKED);
    assert(pthread_mutex_lock(&s->mutex) == 0);
    assert(access_store_session_check(s, &rows[0].ref, 102) == ACCESS_SESSION_BUSY);
    assert(pthread_mutex_unlock(&s->mutex) == 0);
    cleanup(directory, s);
}
static void bounds(void)
{
    char directory[64]; access_store_t *s = fresh(directory);
    for (unsigned i = 0; i < ACCESS_OUTBOX_LIMIT; i++) {
        char request[33]; snprintf(request, sizeof(request), "%032x", i + 1);
        access_result_t a = access_store_issue(s, request, revision(s), "Pending", false, 0, 100, NULL, NULL);
        assert(a.outcome == ACCESS_PENDING && a.code[0] == '\0');
    }
    access_result_t a = access_store_issue(s, request1, revision(s), "Full", false, 0, 100, NULL, NULL);
    assert(a.outcome == ACCESS_LIMIT); cleanup(directory, s);
    assert(!access_operator_allowed("/does/not/exist", "default-op"));
    assert(!access_operator_allowed("relative", "default-op"));
}
int main(void)
{
    basic(); crash_recovery(); fault_case(&fail_write, false); fault_case(&fail_file_sync, false);
    fault_case(&fail_directory_sync, true); fault_case(&fail_rename, false); privacy(); races(); bounds();
    puts("access token store fixtures passed"); return 0;
}
