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
    assert(revision(s) == first_revision);
    assert(access_store_session_check(s, &ref, 101) == ACCESS_SESSION_VALID);
    access_token_info_t row;
    assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_COMMITTED);
    assert(row.history_count == 1 && row.last_admitted_at == 101 && !row.has_expiry);
    access_store_close(s); s = NULL;
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, a.code, identity, 102, &ref) == ACCESS_COMMITTED);
    uint64_t issue_revision = revision(s);
    access_result_t b = access_store_issue(s, request2, issue_revision, "Temporary", true, 110, 102, accepted, NULL);
    assert(b.outcome == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 103, &ref) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 102, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 110, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 109, &ref) == ACCESS_DENIED);
    assert(access_store_expire(s, 110) == ACCESS_COMMITTED);
    replay = access_store_issue(s, request2, issue_revision, "Temporary", true, 110, 111, accepted, NULL);
    assert(replay.outcome == ACCESS_SECRET_UNAVAILABLE && replay.code[0] == '\0');
    access_result_t revoked = access_store_revoke(s, request3, revision(s), a.token.token_id, 110);
    assert(revoked.outcome == ACCESS_LOCALLY_REVOKED && revoked.route_pending);
    assert(access_store_authorize(s, a.code, identity, 111, &ref) == ACCESS_DENIED);
    uint64_t remove_revision = revision(s);
    access_result_t removed = access_store_remove(s, request4, remove_revision, a.token.token_id, 111);
    assert(removed.outcome == ACCESS_PENDING);
    access_route_t outbox[ACCESS_OUTBOX_LIMIT]; size_t count = 0;
    assert(access_store_outbox(s, outbox, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED && count == 1);
    access_route_t incorrect = outbox[0]; incorrect.token.revision--;
    assert(access_store_route_ack(s, &incorrect, 111) == ACCESS_CONFLICT);
    assert(access_store_route_ack(s, outbox, 111) == ACCESS_COMMITTED);
    removed = access_store_remove(s, request4, remove_revision, a.token.token_id, 111);
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
    access_status_t status;
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_UNAVAILABLE);
    access_store_close(s);
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_COMMITTED);
    assert(status.pending_route_sync == 1 && status.revision == 2);
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_COMMITTED);
    assert(status.revision == 2); /* Inspection does not reconcile pending issuance. */
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
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
    access_result_t active = access_store_issue(s, request2, revision(s), "Active before outage", false, 0, 100, accepted, NULL);
    assert(active.outcome == ACCESS_COMMITTED);
    for (unsigned i = 0; i < ACCESS_OUTBOX_LIMIT; i++) {
        char request[33]; snprintf(request, sizeof(request), "%032x", i + 1);
        access_result_t a = access_store_issue(s, request, revision(s), "Pending", false, 0, 100, NULL, NULL);
        assert(a.outcome == ACCESS_PENDING && a.code[0] == '\0');
    }
    access_result_t a = access_store_issue(s, request1, revision(s), "Full", false, 0, 100, NULL, NULL);
    assert(a.outcome == ACCESS_LIMIT);
    a = access_store_revoke(s, request3, revision(s), active.token.token_id, 101);
    assert(a.outcome == ACCESS_LOCALLY_REVOKED && access_store_status(s).pending_route_sync == 33);
    access_token_ref_t ref;
    assert(access_store_authorize(s, active.code, identity, 102, &ref) == ACCESS_DENIED);
    access_result_cleanse(&active); cleanup(directory, s);
    assert(!access_operator_allowed("/does/not/exist", "default-op"));
    assert(!access_operator_allowed("relative", "default-op"));
}

static access_outcome_t activation_fault(void *context, const access_route_t *request)
{
    (void) request; *(int *) context = 1; return ACCESS_COMMITTED;
}
static void activation_failures(void)
{
    for (unsigned i = 0; i < 2; i++) {
        char directory[64]; access_store_t *s = fresh(directory);
        int *fault = i ? &fail_directory_sync : &fail_file_sync;
        access_result_t a = access_store_issue(s, request1, revision(s), "Activation fault", false, 0, 100, activation_fault, fault);
        assert(a.outcome == (i ? ACCESS_INDETERMINATE : ACCESS_SAVE_FAILED) && a.code[0] == '\0');
        *fault = 0; access_store_close(s);
        assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
        a = access_store_result(s, request1);
        assert(a.outcome == (i ? ACCESS_SECRET_UNAVAILABLE : ACCESS_LOCALLY_REVOKED) && a.code[0] == '\0');
        cleanup(directory, s);
    }
}
static void paths(void)
{
    char directory[64]; access_store_t *s = fresh(directory);
    int cwd = open(".", O_RDONLY | O_DIRECTORY); assert(cwd >= 0);
    char child[128]; snprintf(child, sizeof(child), "%s/missing", directory);
    assert(access_store_absent(child));
    snprintf(child, sizeof(child), "%s/missing/leaf", directory); assert(!access_store_absent(child));
    assert(chdir(directory) == 0); assert(access_store_absent("./missing"));
    assert(symlink("absent", "dangling") == 0); assert(!access_store_absent("./dangling"));
    assert(unlink("dangling") == 0);
    access_store_close(s); assert(access_store_open(&s, "./", identity, true, false) == ACCESS_UNAVAILABLE);
    assert(mkdir("child", 0700) == 0); int lock = access_state_lock("./child"); assert(lock >= 0); access_state_unlock(lock);
    assert(rmdir("child") == 0);
    assert(fchdir(cwd) == 0); assert(close(cwd) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    cleanup(directory, s);
}



static void expiry_faults_and_remove(void)
{
    for (unsigned i = 0; i < 3; i++) {
        char directory[64]; access_store_t *s = fresh(directory);
        access_result_t a = access_store_issue(s, request1, revision(s), "Expiry failure", true, 110, 100, accepted, NULL);
        assert(a.outcome == ACCESS_COMMITTED); access_token_ref_t ref;
        assert(access_store_authorize(s, a.code, identity, 101, &ref) == ACCESS_COMMITTED);
        if (i == 2) {
            assert(access_store_session_check(s, &ref, 110) == ACCESS_SESSION_BUSY);
            assert(access_store_session_check(s, &ref, 109) == ACCESS_SESSION_BUSY);
            assert(access_store_expire(s, 109) == ACCESS_COMMITTED);
            assert(access_store_session_check(s, &ref, 109) == ACCESS_SESSION_DENIED);
        } else {
            fail_file_sync = 1;
            assert((i ? access_store_expire(s, 110) : access_store_authorize(s, a.code, identity, 110, &ref)) == ACCESS_SAVE_FAILED);
            fail_file_sync = 0;
            assert(!access_store_status(s).durability_ok);
            assert(access_store_authorize(s, a.code, identity, 109, &ref) == ACCESS_DENIED);
        }
        access_result_cleanse(&a); cleanup(directory, s);
    }
    char directory[64]; access_store_t *s = fresh(directory);
    access_result_t a = access_store_issue(s, request1, revision(s), "Remove directly", false, 0, 100, accepted, NULL);
    access_result_t removed = access_store_remove(s, request2, revision(s), a.token.token_id, 101);
    assert(removed.outcome == ACCESS_PENDING && removed.route_pending);
    access_token_ref_t ref; assert(access_store_authorize(s, a.code, identity, 102, &ref) == ACCESS_DENIED);
    access_route_t page[ACCESS_OUTBOX_LIMIT]; size_t count;
    assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED && count == 1 && page[0].revoke);
    assert(access_store_route_ack(s, page, 102) == ACCESS_COMMITTED);
    assert(access_store_result(s, request2).outcome == ACCESS_COMMITTED);
    assert(access_store_status(s).protected_policy && s->state->token_count == 0 && s->state->tombstone_count == 1);
    access_result_cleanse(&a); cleanup(directory, s);
}

static void full_capacity(void)
{
    char directory[64]; access_store_t *s = fresh(directory);
    snapshot_t *n = candidate(s, 100); assert(n);
    n->token_count = ACCESS_TOKEN_LIMIT;
    for (unsigned i = 0; i < ACCESS_TOKEN_LIMIT; i++) {
        token_t *t = &n->tokens[i]; snprintf(t->info.ref.token_id, 33, "%032x", i + 1);
        snprintf(t->route_request, 33, "%032x", i + 1);
        strcpy(t->info.label, "Capacity fixture"); t->info.ref.revision = 1;
        t->info.created_at = 100; t->info.state = ACCESS_TOKEN_ACTIVE;
        assert(digest(t->info.ref.token_id, 32, t->index));
        assert(digest(t->index, 32, t->verifier));
    }
    assert(commit(s, n, false) == ACCESS_COMMITTED); discard(n);
    access_result_t a = access_store_issue(s, request1, revision(s), "No eviction", false, 0, 101, accepted, NULL);
    assert(a.outcome == ACCESS_LIMIT);
    for (unsigned i = 0; i < ACCESS_TOKEN_LIMIT; i++) {
        char id[33], request[33]; snprintf(id, sizeof(id), "%032x", i + 1);
        snprintf(request, sizeof(request), "%032x", i + 2000);
        a = access_store_revoke(s, request, revision(s), id, 102);
        assert(a.outcome == ACCESS_LOCALLY_REVOKED);
    }
    assert(access_store_status(s).pending_route_sync == ACCESS_TOKEN_LIMIT);
    access_route_t page[ACCESS_OUTBOX_LIMIT]; size_t count;
    assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED && count == ACCESS_OUTBOX_LIMIT);
    access_store_close(s); assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(access_store_status(s).pending_route_sync == ACCESS_TOKEN_LIMIT);
    cleanup(directory, s);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--capacity")) { full_capacity(); puts("1024-token outage capacity passed"); return 0; }
    basic(); crash_recovery(); fault_case(&fail_write, false); fault_case(&fail_file_sync, false);
    fault_case(&fail_directory_sync, true); fault_case(&fail_rename, false); privacy(); races(); bounds(); activation_failures(); paths(); expiry_faults_and_remove();
    puts("access token store fixtures passed"); return 0;
}
