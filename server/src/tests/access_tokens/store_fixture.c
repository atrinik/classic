/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <toolkit/access_code.h>

static bool force_code, fail_generation;
static char forced_code[17];
static unsigned generation_calls;
static bool fixture_generate(char out[17]) {
    generation_calls++;
    if (fail_generation) {
        memset(out, 0, 17);
        return false;
    }
    if (force_code) {
        memcpy(out, forced_code, 17);
        return true;
    }
    return access_code_generate(out);
}

/* Deterministic syscall faults operate on fixture-owned files only. */
static int fail_write, fail_file_sync, fail_directory_sync, fail_rename;
static ssize_t fixture_write(int fd, const void *data, size_t n) {
    if (fail_write) {
        errno = ENOSPC;
        return -1;
    }
    return write(fd, data, n);
}
static int fixture_fsync(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    if ((S_ISDIR(st.st_mode) && fail_directory_sync) || (!S_ISDIR(st.st_mode) && fail_file_sync)) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static int fixture_renameat(int a, const char *b, int c, const char *d) {
    if (fail_rename) {
        errno = EIO;
        return -1;
    }
    return renameat(a, b, c, d);
}
/* Model an ownership mismatch and descriptor rebound between observations
 * without requiring privilege or racing a second thread nondeterministically. */
static int stat_fault_fd = -1, stat_fault_call, stat_fault_seen;
static bool stat_fault_owner;
static int fixture_fstat(int fd, struct stat *st) {
    int result = fstat(fd, st);
    if (result == 0 && fd == stat_fault_fd && ++stat_fault_seen == stat_fault_call) {
        if (stat_fault_owner)
            st->st_uid = geteuid() == 0 ? 1 : 0;
        else
            st->st_ino ^= 1;
    }
    return result;
}
#define fstat fixture_fstat
#define access_code_generate fixture_generate
#define write fixture_write
#define fsync fixture_fsync
#define renameat fixture_renameat
#include "../../server/access_tokens.c"
#undef fstat
#undef access_code_generate
#undef write
#undef fsync
#undef renameat

static const uint8_t identity[32] = {0xa5};
static const char *request1 = "11111111111111111111111111111111";
static const char *request2 = "22222222222222222222222222222222";
static const char *request3 = "33333333333333333333333333333333";
static const char *request4 = "44444444444444444444444444444444";
static unsigned routes;
static access_outcome_t accepted(void *context, const access_route_t *request) {
    (void)context;
    assert(!request->revoke);
    assert(hex_id(request->token.token_id));
    routes++;
    return ACCESS_COMMITTED;
}
static uint64_t revision(access_store_t *s) {
    return access_store_status(s).revision;
}
static access_store_t *fresh(char directory[64]) {
    strcpy(directory, "/tmp/atrinik-access-store-test-XXXXXX");
    assert(mkdtemp(directory));
    access_store_t *s = NULL;
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(access_store_open(&s, directory, identity, true, true) == ACCESS_COMMITTED);
    return s;
}
static void cleanup(char *directory, access_store_t *s) {
    access_store_close(s);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, SNAPSHOT);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}
static void basic(void) {
    char directory[64];
    access_store_t *s = fresh(directory), *other = NULL;
    assert(access_store_open(&other, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    access_result_t a =
        access_store_issue(s, request1, revision(s), "Family", false, 0, 100, accepted, NULL);
    assert(a.outcome == ACCESS_COMMITTED && strlen(a.code) == 16 && routes == 1 &&
           !a.route_pending);
    uint64_t first_revision = revision(s);
    access_result_t replay =
        access_store_issue(s, request1, 1, "Family", false, 0, 100, accepted, NULL);
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
    access_store_close(s);
    s = NULL;
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, a.code, identity, 102, &ref) == ACCESS_COMMITTED);
    uint64_t issue_revision = revision(s);
    access_result_t b = access_store_issue(s,
                                           request2,
                                           issue_revision,
                                           "Temporary",
                                           true,
                                           110,
                                           102,
                                           accepted,
                                           NULL);
    assert(b.outcome == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 103, &ref) == ACCESS_COMMITTED);
    assert(access_store_authorize(s, b.code, identity, 102, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 110, &ref) == ACCESS_DENIED);
    assert(access_store_authorize(s, b.code, identity, 109, &ref) == ACCESS_DENIED);
    assert(access_store_expire(s, 110) == ACCESS_COMMITTED);
    replay = access_store_issue(s,
                                request2,
                                issue_revision,
                                "Temporary",
                                true,
                                110,
                                111,
                                accepted,
                                NULL);
    assert(replay.outcome == ACCESS_SECRET_UNAVAILABLE && replay.code[0] == '\0');
    access_result_t revoked = access_store_revoke(s, request3, revision(s), a.token.token_id, 110);
    assert(revoked.outcome == ACCESS_LOCALLY_REVOKED && revoked.route_pending);
    assert(access_store_authorize(s, a.code, identity, 111, &ref) == ACCESS_DENIED);
    uint64_t remove_revision = revision(s);
    access_result_t removed =
        access_store_remove(s, request4, remove_revision, a.token.token_id, 111);
    assert(removed.outcome == ACCESS_PENDING);
    access_route_t outbox[ACCESS_OUTBOX_LIMIT];
    size_t count = 0;
    assert(access_store_outbox(s, outbox, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED &&
           count == 1);
    access_route_t incorrect = outbox[0];
    incorrect.token.revision--;
    assert(access_store_route_ack(s, &incorrect, 111) == ACCESS_CONFLICT);
    assert(access_store_route_ack(s, outbox, 111) == ACCESS_COMMITTED);
    removed = access_store_remove(s, request4, remove_revision, a.token.token_id, 111);
    assert(removed.outcome == ACCESS_COMMITTED && !removed.route_pending);
    assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_NOT_FOUND);
    assert(access_store_status(s).protected_policy);
    assert(access_store_flush_for_shutdown(s) == ACCESS_COMMITTED);
    assert(access_store_issue(s, request1, revision(s), "Family", false, 0, 112, accepted, NULL)
               .outcome == ACCESS_UNAVAILABLE);
    access_result_cleanse(&a);
    access_result_cleanse(&b);
    cleanup(directory, s);
}
static void crash_recovery(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    access_result_t a =
        access_store_issue(s, request1, revision(s), "Interrupted", false, 0, 100, NULL, NULL);
    assert(a.outcome == ACCESS_PENDING && a.code[0] == '\0');
    access_status_t status;
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_UNAVAILABLE);
    access_store_close(s);
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_COMMITTED);
    assert(status.pending_route_sync == 1 && status.revision == 2);
    assert(access_store_inspect(directory, identity, true, &status) == ACCESS_COMMITTED);
    assert(status.revision == 2); /* Inspection does not reconcile pending issuance. */
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    access_token_info_t row;
    assert(access_store_history(s, a.token.token_id, revision(s), &row) == ACCESS_COMMITTED);
    assert(row.state == ACCESS_TOKEN_REVOKED && row.history_count == 0 && row.route_pending);
    a = access_store_result(s, request1);
    assert(a.outcome == ACCESS_LOCALLY_REVOKED && a.code[0] == '\0');
    cleanup(directory, s);
}
static void fault_case(int *fault, bool uncertain) {
    char directory[64];
    access_store_t *s = fresh(directory);
    uint64_t before = revision(s);
    *fault = 1;
    access_result_t a =
        access_store_issue(s, request1, before, "Fault", false, 0, 100, accepted, NULL);
    assert(a.outcome == (uncertain ? ACCESS_INDETERMINATE : ACCESS_SAVE_FAILED));
    assert(a.code[0] == '\0');
    *fault = 0;
    assert(access_store_status(s).durability_ok == !uncertain);
    access_token_ref_t ref;
    assert(access_store_authorize(s, "0000000000000000", identity, 101, &ref) == ACCESS_DENIED);
    access_store_close(s);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(revision(s) == (uncertain ? before + 2 : before));
    cleanup(directory, s);
}
static void privacy(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    access_store_close(s);
    char path[256], link[256];
    snprintf(path, sizeof(path), "%s/%s", directory, SNAPSHOT);
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    assert(fd >= 0);
    assert(fchmod(fd, 0644) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(fchmod(fd, 0600) == 0);
    uint8_t wrong[32] = {0};
    assert(access_store_open(&s, directory, wrong, true, false) == ACCESS_UNAVAILABLE);
    snprintf(link, sizeof(link), "%s.sym", directory);
    assert(symlink(directory, link) == 0);
    assert(access_store_open(&s, link, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(unlink(link) == 0);
    snprintf(link, sizeof(link), "%s/held", directory);
    assert(rename(path, link) == 0);
    assert(symlink("held", path) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(unlink(path) == 0);
    assert(rename(link, path) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    access_store_close(s);
    /* Corrupt the pinned original snapshot, never a pathname checked earlier. */
    char byte = 1;
    assert(pwrite(fd, &byte, 1, 32) == 1);
    assert(close(fd) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_UNAVAILABLE);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}
static access_outcome_t concurrent_revoke(void *context, const access_route_t *request) {
    access_store_t *s = context;
    access_result_t r = access_store_revoke(s, request2, revision(s), request->token.token_id, 101);
    assert(r.outcome == ACCESS_LOCALLY_REVOKED);
    return ACCESS_COMMITTED;
}
static void races(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    access_result_t a =
        access_store_issue(s, request1, revision(s), "Race", false, 0, 100, concurrent_revoke, s);
    assert(a.outcome == ACCESS_DENIED && a.code[0] == '\0');
    access_token_info_t rows[1];
    size_t count, next;
    assert(access_store_list(s, revision(s), 0, 1, rows, &count, &next) == ACCESS_COMMITTED);
    assert(count == 1 && rows[0].state == ACCESS_TOKEN_REVOKED);
    assert(pthread_mutex_lock(&s->mutex) == 0);
    assert(access_store_session_check(s, &rows[0].ref, 102) == ACCESS_SESSION_BUSY);
    assert(pthread_mutex_unlock(&s->mutex) == 0);
    cleanup(directory, s);
}
static void bounds(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    access_result_t active = access_store_issue(s,
                                                request2,
                                                revision(s),
                                                "Active before outage",
                                                false,
                                                0,
                                                100,
                                                accepted,
                                                NULL);
    assert(active.outcome == ACCESS_COMMITTED);
    for (unsigned i = 0; i < ACCESS_OUTBOX_LIMIT; i++) {
        char request[33];
        snprintf(request, sizeof(request), "%032x", i + 1);
        access_result_t a =
            access_store_issue(s, request, revision(s), "Pending", false, 0, 100, NULL, NULL);
        assert(a.outcome == ACCESS_PENDING && a.code[0] == '\0');
    }
    access_result_t a =
        access_store_issue(s, request1, revision(s), "Full", false, 0, 100, NULL, NULL);
    assert(a.outcome == ACCESS_LIMIT);
    a = access_store_revoke(s, request3, revision(s), active.token.token_id, 101);
    assert(a.outcome == ACCESS_LOCALLY_REVOKED && access_store_status(s).pending_route_sync == 33);
    access_route_t page[ACCESS_OUTBOX_LIMIT];
    size_t count;
    assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED &&
           count == ACCESS_OUTBOX_LIMIT);
    assert(page[0].revoke && !strcmp(page[0].token.token_id, active.token.token_id));
    access_token_ref_t ref;
    assert(access_store_authorize(s, active.code, identity, 102, &ref) == ACCESS_DENIED);
    access_result_cleanse(&active);
    cleanup(directory, s);
}

static access_outcome_t activation_fault(void *context, const access_route_t *request) {
    (void)request;
    *(int *)context = 1;
    return ACCESS_COMMITTED;
}
static void activation_failures(void) {
    for (unsigned i = 0; i < 2; i++) {
        char directory[64];
        access_store_t *s = fresh(directory);
        int *fault = i ? &fail_directory_sync : &fail_file_sync;
        access_result_t a = access_store_issue(s,
                                               request1,
                                               revision(s),
                                               "Activation fault",
                                               false,
                                               0,
                                               100,
                                               activation_fault,
                                               fault);
        assert(a.outcome == (i ? ACCESS_INDETERMINATE : ACCESS_SAVE_FAILED) && a.code[0] == '\0');
        *fault = 0;
        access_store_close(s);
        assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
        a = access_store_result(s, request1);
        assert(a.outcome == (i ? ACCESS_SECRET_UNAVAILABLE : ACCESS_LOCALLY_REVOKED) &&
               a.code[0] == '\0');
        cleanup(directory, s);
    }
}
static void paths(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    char child[128];
    snprintf(child, sizeof(child), "%s/missing", directory);
    assert(access_store_absent(child));
    snprintf(child, sizeof(child), "%s/missing/leaf", directory);
    assert(!access_store_absent(child));
    assert(chdir(directory) == 0);
    assert(access_store_absent("./missing"));
    assert(symlink("absent", "dangling") == 0);
    assert(!access_store_absent("./dangling"));
    assert(unlink("dangling") == 0);
    access_store_close(s);
    assert(access_store_open(&s, "./", identity, true, false) == ACCESS_UNAVAILABLE);
    assert(mkdir("child", 0700) == 0);
    int lock = access_state_lock("./child");
    assert(lock >= 0);
    access_state_unlock(lock);
    assert(rmdir("child") == 0);
    assert(fchdir(cwd) == 0);
    assert(close(cwd) == 0);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    cleanup(directory, s);
}

static void expiry_faults_and_remove(void) {
    for (unsigned i = 0; i < 3; i++) {
        char directory[64];
        access_store_t *s = fresh(directory);
        access_result_t a = access_store_issue(s,
                                               request1,
                                               revision(s),
                                               "Expiry failure",
                                               true,
                                               110,
                                               100,
                                               accepted,
                                               NULL);
        assert(a.outcome == ACCESS_COMMITTED);
        access_token_ref_t ref;
        assert(access_store_authorize(s, a.code, identity, 101, &ref) == ACCESS_COMMITTED);
        if (i == 2) {
            assert(access_store_session_check(s, &ref, 110) == ACCESS_SESSION_BUSY);
            assert(access_store_session_check(s, &ref, 109) == ACCESS_SESSION_BUSY);
            assert(access_store_expire(s, 109) == ACCESS_COMMITTED);
            assert(access_store_session_check(s, &ref, 109) == ACCESS_SESSION_DENIED);
        } else {
            fail_file_sync = 1;
            assert((i ? access_store_expire(s, 110)
                      : access_store_authorize(s, a.code, identity, 110, &ref)) ==
                   ACCESS_SAVE_FAILED);
            fail_file_sync = 0;
            assert(!access_store_status(s).durability_ok);
            assert(access_store_authorize(s, a.code, identity, 109, &ref) == ACCESS_DENIED);
        }
        access_result_cleanse(&a);
        cleanup(directory, s);
    }
    char directory[64];
    access_store_t *s = fresh(directory);
    access_result_t a = access_store_issue(s,
                                           request1,
                                           revision(s),
                                           "Remove directly",
                                           false,
                                           0,
                                           100,
                                           accepted,
                                           NULL);
    access_result_t removed = access_store_remove(s, request2, revision(s), a.token.token_id, 101);
    assert(removed.outcome == ACCESS_PENDING && removed.route_pending);
    access_token_ref_t ref;
    assert(access_store_authorize(s, a.code, identity, 102, &ref) == ACCESS_DENIED);
    access_route_t page[ACCESS_OUTBOX_LIMIT];
    size_t count;
    assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED &&
           count == 1 && page[0].revoke);
    assert(access_store_route_ack(s, page, 102) == ACCESS_COMMITTED);
    assert(access_store_result(s, request2).outcome == ACCESS_COMMITTED);
    assert(access_store_status(s).protected_policy && s->state->token_count == 0 &&
           s->state->tombstone_count == 1);
    access_result_cleanse(&a);
    cleanup(directory, s);
}

typedef struct {
    unsigned calls, collisions;
    access_route_t previous;
    bool unavailable;
} collision_context_t;
static access_outcome_t remote_collision(void *opaque, const access_route_t *request) {
    collision_context_t *context = opaque;
    assert(request->deadline_monotonic_ms > monotonic_ms());
    if (context->calls) {
        assert(strcmp(request->request_id, context->previous.request_id));
        assert(strcmp(request->token.token_id, context->previous.token.token_id));
        assert(CRYPTO_memcmp(request->index, context->previous.index, 32));
        assert(request->deadline_monotonic_ms == context->previous.deadline_monotonic_ms);
    }
    context->previous = *request;
    context->calls++;
    return context->calls <= context->collisions ? ACCESS_ROUTE_COLLISION
           : context->unavailable                ? ACCESS_UNAVAILABLE
                                                 : ACCESS_COMMITTED;
}
/* A missing receipt must fail closed even if an unexpected state fault is
 * observed after the remote callback returns and before retry materialization. */
static access_outcome_t collision_missing_receipt(void *opaque, const access_route_t *request) {
    (void)request;
    access_store_t *store = opaque;
    pthread_mutex_lock(&store->mutex);
    store->state->receipt_count = 0;
    pthread_mutex_unlock(&store->mutex);
    return ACCESS_ROUTE_COLLISION;
}
static void collision_receipt_fault(void) {
    char directory[64];
    access_store_t *store = fresh(directory);
    access_result_t issued = access_store_issue(store,
                                               request1,
                                               revision(store),
                                               "Missing retry receipt",
                                               false,
                                               0,
                                               100,
                                               collision_missing_receipt,
                                               store);
    assert(issued.outcome == ACCESS_PENDING && issued.code[0] == '\0');
    assert(store->state->token_count == 1 &&
           store->state->tokens[0].info.state == ACCESS_TOKEN_PENDING);
    access_result_cleanse(&issued);
    cleanup(directory, store);
}

static void collisions(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    collision_context_t context = {.collisions = 2};
    access_result_t a = access_store_issue(s,
                                           request1,
                                           revision(s),
                                           "Collision retry",
                                           false,
                                           0,
                                           100,
                                           remote_collision,
                                           &context);
    assert(a.outcome == ACCESS_COMMITTED && context.calls == 3);
    assert(!strcmp(a.token.token_id, context.previous.token.token_id));
    access_token_ref_t ref;
    assert(access_store_authorize(s, a.code, identity, 101, &ref) == ACCESS_COMMITTED);
    assert(access_store_result(s, request1).outcome == ACCESS_SECRET_UNAVAILABLE);
    memcpy(forced_code, a.code, 17);
    force_code = true;
    generation_calls = 0;
    access_result_t b = access_store_issue(s,
                                           request2,
                                           revision(s),
                                           "Forced local collision",
                                           false,
                                           0,
                                           102,
                                           accepted,
                                           NULL);
    assert(b.outcome == ACCESS_UNAVAILABLE && b.code[0] == '\0' && generation_calls == 3);
    force_code = false;
    fail_generation = true;
    b = access_store_issue(s,
                           request2,
                           revision(s),
                           "Entropy failure",
                           false,
                           0,
                           102,
                           accepted,
                           NULL);
    assert(b.outcome == ACCESS_UNAVAILABLE && b.code[0] == '\0');
    fail_generation = false;
    context = (collision_context_t){.collisions = 1, .unavailable = true};
    b = access_store_issue(s,
                           request2,
                           revision(s),
                           "Retried then interrupted",
                           false,
                           0,
                           102,
                           remote_collision,
                           &context);
    assert(b.outcome == ACCESS_PENDING && context.calls == 2 && b.code[0] == '\0');
    access_store_close(s);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    b = access_store_result(s, request2);
    assert(b.outcome == ACCESS_LOCALLY_REVOKED && b.route_pending);
    context = (collision_context_t){.collisions = 3};
    b = access_store_issue(s,
                           request3,
                           revision(s),
                           "Bounded remote collisions",
                           false,
                           0,
                           103,
                           remote_collision,
                           &context);
    assert(b.outcome == ACCESS_PENDING && context.calls == 3 && b.code[0] == '\0');
    access_result_cleanse(&a);
    cleanup(directory, s);
}

static void removal_capacity_receipts(void) {
    for (unsigned mode = 0; mode < 2; mode++) {
        char directory[64];
        access_store_t *s = fresh(directory);
        access_result_t issued = access_store_issue(s,
                                                    request1,
                                                    revision(s),
                                                    "Shared removal",
                                                    false,
                                                    0,
                                                    100,
                                                    accepted,
                                                    NULL);
        snapshot_t *n = candidate(s, 100);
        assert(n);
        n->tombstone_count = ACCESS_TOMBSTONE_LIMIT;
        for (unsigned i = 0; i < ACCESS_TOMBSTONE_LIMIT; i++) {
            snprintf(n->tombstones[i].id, 33, "a%031x", i + 1);
            n->tombstones[i].removed = 100;
            n->tombstones[i].revision = n->revision;
        }
        assert(commit(s, n, false) == ACCESS_COMMITTED);
        discard(n);
        uint64_t expected_a = revision(s);
        access_result_t a =
            access_store_remove(s, request2, expected_a, issued.token.token_id, 101);
        assert(a.outcome == ACCESS_PENDING);
        access_result_t b =
            access_store_remove(s, request3, revision(s), issued.token.token_id, 101);
        assert(b.outcome == ACCESS_PENDING);
        access_route_t page[ACCESS_OUTBOX_LIMIT];
        size_t count;
        assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED &&
               count == 1);
        assert(access_store_route_ack(s, page, 102) == ACCESS_COMMITTED);
        assert(access_store_result(s, request2).outcome == ACCESS_PENDING);
        assert(access_store_result(s, request3).outcome == ACCESS_PENDING);
        int64_t later = TOMBSTONE_DAYS + 101;
        if (mode)
            a = access_store_remove(s, request4, revision(s), issued.token.token_id, later);
        else
            a = access_store_remove(s, request2, expected_a, issued.token.token_id, later);
        assert(a.outcome == ACCESS_COMMITTED);
        assert(access_store_result(s, request2).outcome == ACCESS_COMMITTED);
        assert(access_store_result(s, request3).outcome == ACCESS_COMMITTED);
        assert(s->state->token_count == 0 && s->state->tombstone_count == 1);
        access_result_cleanse(&issued);
        cleanup(directory, s);
    }
}

static void full_capacity(void) {
    char directory[64];
    access_store_t *s = fresh(directory);
    snapshot_t *n = candidate(s, 100);
    assert(n);
    n->token_count = ACCESS_TOKEN_LIMIT;
    for (unsigned i = 0; i < ACCESS_TOKEN_LIMIT; i++) {
        token_t *t = &n->tokens[i];
        snprintf(t->info.ref.token_id, 33, "%032x", i + 1);
        snprintf(t->route_request, 33, "%032x", i + 1);
        strcpy(t->info.label, "Capacity fixture");
        t->info.ref.revision = 1;
        t->info.created_at = 100;
        t->info.state = ACCESS_TOKEN_ACTIVE;
        assert(digest(t->info.ref.token_id, 32, t->index));
        assert(digest(t->index, 32, t->verifier));
    }
    assert(commit(s, n, false) == ACCESS_COMMITTED);
    discard(n);
    access_result_t a =
        access_store_issue(s, request1, revision(s), "No eviction", false, 0, 101, accepted, NULL);
    assert(a.outcome == ACCESS_LIMIT);
    for (unsigned i = 0; i < ACCESS_TOKEN_LIMIT; i++) {
        char id[33], request[33];
        snprintf(id, sizeof(id), "%032x", i + 1);
        snprintf(request, sizeof(request), "%032x", i + 2000);
        a = access_store_revoke(s, request, revision(s), id, 102);
        assert(a.outcome == ACCESS_LOCALLY_REVOKED);
    }
    assert(access_store_status(s).pending_route_sync == ACCESS_TOKEN_LIMIT);
    access_route_t page[ACCESS_OUTBOX_LIMIT];
    size_t count;
    assert(access_store_outbox(s, page, ACCESS_OUTBOX_LIMIT, &count) == ACCESS_COMMITTED &&
           count == ACCESS_OUTBOX_LIMIT);
    access_store_close(s);
    assert(access_store_open(&s, directory, identity, true, false) == ACCESS_COMMITTED);
    assert(access_store_status(s).pending_route_sync == ACCESS_TOKEN_LIMIT);
    cleanup(directory, s);
}

static void descriptor_capabilities(void) {
    char root[] = "/tmp/atrinik-access-fd-test-XXXXXX";
    assert(mkdtemp(root));
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(parent >= 3 && flock(parent, LOCK_EX | LOCK_NB) == 0);
    int retained = access_state_lock_fd(parent);
    assert(retained >= 3 && retained != parent);
    assert(fcntl(retained, F_GETFD) & FD_CLOEXEC);
    struct stat original, copy;
    assert(fstat(parent, &original) == 0 && fstat(retained, &copy) == 0);
    assert(original.st_dev == copy.st_dev && original.st_ino == copy.st_ino);

    /* The child borrows the wrapper's already locked open-file description.
     * Its close must not release the supervisor's lock. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int borrowed = access_state_lock_fd(parent);
        if (borrowed < 0)
            _exit(1);
        access_state_unlock(borrowed);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int independent = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(independent >= 3 && flock(independent, LOCK_EX | LOCK_NB) < 0);
    assert(errno == EWOULDBLOCK || errno == EAGAIN);
    assert(access_state_lock_fd(independent) < 0);
    close(independent);

    /* Ordinary path consumers still reject proc magic and arbitrary links. */
    char proc[128], child_path[256], moved[256];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", parent);
    assert(access_state_lock(proc) < 0);
    snprintf(child_path, sizeof(child_path), "%s/access-tokens", proc);
    assert(!access_store_absent(child_path));
    access_store_t *store = NULL, *other = NULL;
    assert(access_store_open(&store, child_path, identity, true, true) == ACCESS_UNAVAILABLE);
    assert(access_store_absent_at(retained, "access-tokens"));
    assert(access_store_open_at(&store, retained, "access-tokens", identity, true, false) ==
           ACCESS_UNAVAILABLE && store == NULL);
    assert(mkdirat(parent, "access-tokens", 0700) == 0);
    assert(!access_store_absent_at(retained, "access-tokens"));
    assert(access_store_open_at(&store, retained, "access-tokens", identity, true, true) ==
           ACCESS_COMMITTED);
    assert(access_store_open_at(&other, retained, "access-tokens", identity, true, false) ==
           ACCESS_UNAVAILABLE && other == NULL);
    assert(access_store_status(store).revision == 1);
    access_store_close(store);
    assert(access_store_open_at(&store, retained, "access-tokens", identity, true, false) ==
           ACCESS_COMMITTED);
    access_result_t issued = access_store_issue(store, request1, revision(store), "Descriptor",
                                               false, 0, 100, accepted, NULL);
    assert(issued.outcome == ACCESS_COMMITTED);
    access_token_ref_t ref;
    assert(access_store_authorize(store, issued.code, identity, 101, &ref) == ACCESS_COMMITTED);
    uint64_t saved_revision = revision(store);
    access_result_cleanse(&issued);
    access_store_close(store);

    const char *bad_leaves[] = {NULL, "", ".", "..", "/access-tokens", "a/b", "a/../b"};
    for (size_t i = 0; i < sizeof(bad_leaves) / sizeof(*bad_leaves); i++) {
        assert(!access_store_absent_at(retained, bad_leaves[i]));
        assert(access_store_open_at(&store, retained, bad_leaves[i], identity, true, true) ==
               ACCESS_UNAVAILABLE && store == NULL);
    }
    assert(symlinkat("access-tokens", parent, "linked") == 0);
    assert(symlinkat("missing", parent, "dangling") == 0);
    for (size_t i = 0; i < 2; i++) {
        const char *name = i ? "dangling" : "linked";
        assert(!access_store_absent_at(retained, name));
        assert(access_store_open_at(&store, retained, name, identity, true, false) ==
               ACCESS_UNAVAILABLE && store == NULL);
        assert(unlinkat(parent, name, 0) == 0);
    }
    assert(fchmodat(parent, "access-tokens", 0750, 0) == 0);
    assert(access_store_open_at(&store, retained, "access-tokens", identity, true, false) ==
           ACCESS_UNAVAILABLE);
    assert(fchmodat(parent, "access-tokens", 0700, 0) == 0);

    /* Replacing the parent pathname cannot redirect child reads/writes. */
    snprintf(moved, sizeof(moved), "%s-moved", root);
    assert(rename(root, moved) == 0 && mkdir(root, 0700) == 0);
    assert(access_store_open_at(&store, retained, "access-tokens", identity, true, false) ==
           ACCESS_COMMITTED);
    assert(access_store_status(store).revision == saved_revision);
    assert(access_store_flush_for_shutdown(store) == ACCESS_COMMITTED);
    access_store_close(store);
    snprintf(child_path, sizeof(child_path), "%s/access-tokens", root);
    assert(access_store_absent(child_path));
    assert(rmdir(root) == 0);
    assert(unlinkat(parent, "access-tokens/access-tokens.snapshot", 0) == 0);
    assert(unlinkat(parent, "access-tokens", AT_REMOVEDIR) == 0);
    independent = open(moved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(independent >= 3);
    close(parent);
    assert(flock(independent, LOCK_EX | LOCK_NB) < 0);
    access_state_unlock(retained);
    assert(flock(independent, LOCK_EX | LOCK_NB) == 0);
    close(independent);
    assert(rmdir(moved) == 0);
}

static void descriptor_rejections(void) {
    char root[] = "/tmp/atrinik-access-fd-negative-XXXXXX";
    assert(mkdtemp(root));
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(fd >= 3);
    assert(access_state_lock_fd(-1) < 0 && access_state_lock_fd(0) < 0 &&
           access_state_lock_fd(1) < 0 && access_state_lock_fd(2) < 0);
    int stale = dup(fd);
    assert(stale >= 3 && close(stale) == 0 && access_state_lock_fd(stale) < 0);
    int regular = openat(fd, "file", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(regular >= 3 && access_state_lock_fd(regular) < 0);
    assert(!access_store_absent_at(regular, "missing"));
    close(regular);
    int pipes[2];
    assert(pipe(pipes) == 0 && access_state_lock_fd(pipes[0]) < 0);
    close(pipes[0]);
    close(pipes[1]);
    int pathonly = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    assert(pathonly >= 3 && access_state_lock_fd(pathonly) < 0);
    close(pathonly);
    const mode_t modes[] = {0755, 0750, 0770, 0707, 01700};
    access_store_t *store = NULL;
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
        assert(fchmod(fd, modes[i]) == 0 && access_state_lock_fd(fd) < 0);
        assert(!access_store_absent_at(fd, "missing"));
        assert(access_store_open_at(&store, fd, "missing", identity, true, true) ==
               ACCESS_UNAVAILABLE && store == NULL);
    }
    assert(fchmod(fd, 0700) == 0);
    stat_fault_fd = fd;
    stat_fault_call = 1;
    stat_fault_seen = 0;
    stat_fault_owner = true;
    assert(access_state_lock_fd(fd) < 0);
    stat_fault_seen = 0;
    assert(!access_store_absent_at(fd, "missing"));
    stat_fault_seen = 0;
    assert(access_store_open_at(&store, fd, "missing", identity, true, true) == ACCESS_UNAVAILABLE);
    stat_fault_owner = false;
    stat_fault_call = 2;
    stat_fault_seen = 0;
    assert(access_state_lock_fd(fd) < 0);
    stat_fault_seen = 0;
    assert(!access_store_absent_at(fd, "missing"));
    stat_fault_seen = 0;
    assert(access_store_open_at(&store, fd, "missing", identity, true, true) == ACCESS_UNAVAILABLE);
    stat_fault_fd = -1;
    assert(unlinkat(fd, "file", 0) == 0);
    close(fd);
    assert(rmdir(root) == 0);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--capacity")) {
        full_capacity();
        puts("1024-token outage capacity passed");
        return 0;
    }
    descriptor_capabilities();
    descriptor_rejections();
    routes = 0;
    basic();
    crash_recovery();
    fault_case(&fail_write, false);
    fault_case(&fail_file_sync, false);
    fault_case(&fail_directory_sync, true);
    fault_case(&fail_rename, false);
    privacy();
    races();
    bounds();
    activation_failures();
    paths();
    expiry_faults_and_remove();
    collisions();
    collision_receipt_fault();
    removal_capacity_receipts();
    puts("access token store fixtures passed");
    return 0;
}
