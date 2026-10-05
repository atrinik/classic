/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <initialization.h>
#include <access_server.h>
#include <access_admin.h>
#include <toolkit/access_code.h>
#include <metaserver_internal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>

typedef enum {
    JOB_FREE,
    JOB_QUEUED,
    JOB_RUNNING,
    JOB_DONE
} job_state;
typedef struct {
    uint64_t id;
    job_state state;
    bool auth, permitted, cancelled;
    access_admin_request_t request;
    char input[ACCESS_ADMIN_REQUEST_MAX + 1];
    size_t input_len;
    char output[ACCESS_ADMIN_RESPONSE_MAX + 1];
    size_t output_len;
    access_outcome_t outcome;
    access_token_ref_t ref;
} access_job;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t thread, expiry_thread;
    bool started, maintenance;
    _Atomic bool stopping;
    uint64_t next_id;
    access_job jobs[ACCESS_OUTBOX_LIMIT];
    access_store_t *store;
    access_status_t absent;
    _Atomic bool failed;
    int64_t tick;
    char retry_after[33];
} worker = {.mutex = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

static bool access_worker_cancelled(void *context) {
    (void)context;
    return atomic_load(&worker.stopping);
}
static const curl_cancel_t route_cancel = {.cancelled = access_worker_cancelled};
#ifdef ATRINIK_TESTING
static access_route_callback_t test_route;
void access_server_route_for_test(access_route_callback_t callback) {
    HARD_ASSERT(!worker.started);
    test_route = callback;
}
#endif
static access_outcome_t access_worker_route(void *context, const access_route_t *route) {
#ifdef ATRINIK_TESTING
    if (test_route != NULL)
        return test_route(context, route);
#endif
    return metaserver_access_route(context, route);
}

/* Once a connection or maintenance pass observes a deadline, a backwards
 * wall-clock step cannot reopen it before its durable expiry transaction. */
static int64_t access_clock_now(void) {
    static _Atomic int64_t observed;
    int64_t now = (int64_t)time(NULL);
    int64_t previous = atomic_load(&observed);
    while (now > previous && !atomic_compare_exchange_weak(&observed, &previous, now)) {}
    return now > previous ? now : previous;
}

static access_job *find_job(uint64_t id) {
    for (size_t i = 0; i < ACCESS_OUTBOX_LIMIT; i++)
        if (worker.jobs[i].state != JOB_FREE && worker.jobs[i].id == id)
            return &worker.jobs[i];
    return NULL;
}
static void clear_job(access_job *job) {
    access_code_clear(job, sizeof(*job));
}

static void maintain_store(void) {
    if (worker.store == NULL)
        return;
    access_outcome_t result = access_store_expire(worker.store, access_clock_now());
    if (result == ACCESS_SAVE_FAILED || result == ACCESS_INDETERMINATE)
        atomic_store(&worker.failed, true);
    access_route_t row;
    if (access_store_revoke_next(worker.store, worker.retry_after, &row) != ACCESS_COMMITTED)
        return;
    /* Advance even on failure; stable IDs survive acknowledgement/removal and
     * reach every retained row, including those beyond the first dispatch page. */
    memcpy(worker.retry_after, row.token.token_id, sizeof(worker.retry_after));
    if (access_worker_route((void *)&route_cancel, &row) == ACCESS_COMMITTED) {
        result = access_store_route_ack(worker.store, &row, access_clock_now());
        if (result == ACCESS_SAVE_FAILED || result == ACCESS_INDETERMINATE)
            atomic_store(&worker.failed, true);
    }
}
static void *run_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&worker.mutex);
    bool maintenance_served = false;
    for (;;) {
        access_job *job = NULL;
        for (size_t i = 0; i < ACCESS_OUTBOX_LIMIT; i++)
            if (worker.jobs[i].state == JOB_QUEUED && (job == NULL || worker.jobs[i].id < job->id))
                job = &worker.jobs[i];
        if (worker.stopping)
            break;
        /* Alternate pending maintenance and queued work. Neither continuous
         * admissions nor repeated ticks can starve the other class of work. */
        if (worker.maintenance && (job == NULL || !maintenance_served)) {
            worker.maintenance = false;
            maintenance_served = true;
            pthread_mutex_unlock(&worker.mutex);
            maintain_store();
            pthread_mutex_lock(&worker.mutex);
            continue;
        }
        if (job == NULL) {
            pthread_cond_wait(&worker.ready, &worker.mutex);
            continue;
        }
        maintenance_served = false;
        job->state = JOB_RUNNING;
        pthread_mutex_unlock(&worker.mutex);
        if (job->auth) {
            job->outcome = worker.store == NULL
                               ? ACCESS_UNAVAILABLE
                               : access_store_authorize(worker.store,
                                                        job->input,
                                                        worker.absent.server_identity,
                                                        access_clock_now(),
                                                        &job->ref);
        } else if (!job->permitted) {
            (void)access_admin_denied_encode(&job->request,
                                             job->output,
                                             sizeof(job->output),
                                             &job->output_len);
        } else if (worker.store != NULL) {
            if (!access_admin_execute(worker.store,
                                      job->input,
                                      job->input_len,
                                      access_worker_route,
                                      (void *)&route_cancel,
                                      job->output,
                                      sizeof(job->output),
                                      &job->output_len))
                atomic_store(&worker.failed, true);
        } else {
            (void)access_admin_execute_absent(&worker.absent,
                                              job->input,
                                              job->input_len,
                                              job->output,
                                              sizeof(job->output),
                                              &job->output_len);
        }
        access_code_clear(job->input, sizeof(job->input));
        pthread_mutex_lock(&worker.mutex);
        if (job->cancelled)
            clear_job(job);
        else
            job->state = JOB_DONE;
    }
    pthread_mutex_unlock(&worker.mutex);
    return NULL;
}

/* Expiry durability cannot sit behind a slow remote route operation. Both
 * threads use the store's same serialized transaction authority; only the
 * admin worker performs remote IO. The game loop uses its nonblocking view. */
static void *run_expiry(void *unused) {
    (void)unused;
    int64_t last = 0;
    for (;;) {
        pthread_mutex_lock(&worker.mutex);
        bool stop = worker.stopping;
        pthread_mutex_unlock(&worker.mutex);
        if (stop)
            break;
        int64_t now = access_clock_now();
        if (worker.store != NULL && now != last) {
            access_outcome_t result = access_store_expire(worker.store, now);
            if (result == ACCESS_SAVE_FAILED || result == ACCESS_INDETERMINATE)
                atomic_store(&worker.failed, true);
            last = now;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

bool access_server_init(const char identity_hex[65]) {
    if (worker.started || settings.access_initialize || strlen(identity_hex) != 64)
        return false;
    memset(&worker.absent, 0, sizeof(worker.absent));
    worker.absent.schema_version = ACCESS_STORE_SCHEMA;
    worker.absent.protected_policy = settings.access_required;
    for (size_t i = 0; i < 32; i++) {
        unsigned value = 0;
        for (size_t k = 0; k < 2; k++) {
            char c = identity_hex[i * 2 + k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
            value = value * 16 + (c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10));
        }
        worker.absent.server_identity[i] = (uint8_t)value;
    }
    char path[HUGE_BUF];
    bool relative_store = settings.access_store[0] == '\0' && initialization_data_descriptor() >= 0;
    if (settings.access_store[0] != '\0')
        snprintf(VS(path), "%s", settings.access_store);
    else if (snprintf(VS(path), "%s/access-tokens", settings.datapath) >= (int)sizeof(path))
        return false;
    int data = initialization_data_descriptor();
    bool absent =
        relative_store ? access_store_absent_at(data, "access-tokens") : access_store_absent(path);
    if (!absent || settings.access_required) {
        access_outcome_t opened = relative_store
                                      ? access_store_open_at(&worker.store,
                                                             data,
                                                             "access-tokens",
                                                             worker.absent.server_identity,
                                                             settings.access_required,
                                                             false)
                                      : access_store_open(&worker.store,
                                                          path,
                                                          worker.absent.server_identity,
                                                          settings.access_required,
                                                          false);
        if (opened != ACCESS_COMMITTED)
            return false;
    }
    worker.stopping = false;
    worker.maintenance = false;
    worker.tick = 0;
    memset(worker.retry_after, 0, sizeof(worker.retry_after));
    atomic_store(&worker.failed, false);
    if (pthread_create(&worker.thread, NULL, run_worker, NULL) != 0) {
        access_store_close(worker.store);
        worker.store = NULL;
        return false;
    }
    if (pthread_create(&worker.expiry_thread, NULL, run_expiry, NULL) != 0) {
        pthread_mutex_lock(&worker.mutex);
        worker.stopping = true;
        pthread_cond_signal(&worker.ready);
        pthread_mutex_unlock(&worker.mutex);
        pthread_join(worker.thread, NULL);
        access_store_close(worker.store);
        worker.store = NULL;
        return false;
    }
    worker.started = true;
    return true;
}
static void schedule_maintenance(void) {
    pthread_mutex_lock(&worker.mutex);
    worker.maintenance = true;
    pthread_cond_signal(&worker.ready);
    pthread_mutex_unlock(&worker.mutex);
}
#ifdef ATRINIK_TESTING
void access_server_maintenance_for_test(void) {
    HARD_ASSERT(worker.started);
    schedule_maintenance();
}
#endif
void access_server_tick(void) {
    if (!worker.started)
        return;
    int64_t now = access_clock_now();
    if (now == worker.tick)
        return;
    worker.tick = now;
    schedule_maintenance();
}
bool access_server_healthy(void) {
    return !atomic_load(&worker.failed);
}
void access_server_save_failed(void) {
    atomic_store(&worker.failed, true);
}

static uint64_t submit(const char *data, size_t len, bool auth, bool permitted) {
    if (!worker.started || atomic_load(&worker.failed) || len > ACCESS_ADMIN_REQUEST_MAX)
        return 0;
    access_admin_request_t request = {0};
    if (!auth && !access_admin_parse(data, len, &request))
        return 0;
    pthread_mutex_lock(&worker.mutex);
    access_job *slot = NULL;
    if (!worker.stopping && worker.next_id != UINT64_MAX)
        for (size_t i = 0; i < ACCESS_OUTBOX_LIMIT; i++)
            if (worker.jobs[i].state == JOB_FREE) {
                slot = &worker.jobs[i];
                break;
            }
    uint64_t id = 0;
    if (slot != NULL) {
        slot->id = id = ++worker.next_id;
        slot->auth = auth;
        /* Main-thread command admission authorizes this operation. No player
         * pointer or mutable permission list crosses the worker boundary. */
        slot->permitted = permitted;
        slot->request = request;
        memcpy(slot->input, data, len);
        slot->input_len = len;
        slot->state = JOB_QUEUED;
        pthread_cond_signal(&worker.ready);
    }
    pthread_mutex_unlock(&worker.mutex);
    return id;
}
uint64_t access_server_root_submit(const char *data, size_t len) {
    return submit(data, len, false, true);
}
uint64_t access_server_admin_submit(const char *data, size_t len, bool permitted) {
    return submit(data, len, false, permitted);
}
uint64_t access_server_auth_submit(const char code[16]) {
    return access_code_valid(code, 16) ? submit(code, 16, true, false) : 0;
}
void access_server_cancel(uint64_t id) {
    if (id == 0)
        return;
    pthread_mutex_lock(&worker.mutex);
    access_job *job = find_job(id);
    if (job != NULL) {
        if (job->state == JOB_RUNNING)
            job->cancelled = true;
        else
            clear_job(job);
    }
    pthread_mutex_unlock(&worker.mutex);
}
bool access_server_admin_poll_permitted(uint64_t id,
                                       bool permitted,
                                       char *out,
                                       size_t cap,
                                       size_t *length) {
    bool done = false;
    pthread_mutex_lock(&worker.mutex);
    access_job *job = find_job(id);
    if (job != NULL && !job->auth && job->state == JOB_DONE) {
        if (!permitted || !job->permitted) {
            /* A revoked character still receives its correlated generic error,
             * never the already-computed response or one-time issuance code. */
            if (!access_admin_denied_encode(&job->request, out, cap, length))
                *length = 0;
        } else if (job->output_len < cap) {
            memcpy(out, job->output, job->output_len);
            out[job->output_len] = '\0';
            *length = job->output_len;
        } else {
            *length = 0;
            atomic_store(&worker.failed, true);
        }
        clear_job(job);
        done = true;
    }
    pthread_mutex_unlock(&worker.mutex);
    return done;
}
bool access_server_admin_poll(uint64_t id, char *out, size_t cap, size_t *length) {
    /* This callback is used only by the independently authenticated root Unix
     * channel; game sockets use the current-character permission variant. */
    return access_server_admin_poll_permitted(id, true, out, cap, length);
}
bool access_server_auth_poll(uint64_t id, access_outcome_t *out, access_token_ref_t *ref) {
    bool done = false;
    pthread_mutex_lock(&worker.mutex);
    access_job *job = find_job(id);
    if (job != NULL && job->auth && job->state == JOB_DONE) {
        *out = job->outcome;
        *ref = job->ref;
        clear_job(job);
        done = true;
    }
    pthread_mutex_unlock(&worker.mutex);
    return done;
}
#ifdef ATRINIK_TESTING
uint64_t access_server_admin_result_for_test(bool permitted,
                                            const char *request,
                                            const char *response) {
    HARD_ASSERT(!worker.started);
    HARD_ASSERT(strlen(response) <= ACCESS_ADMIN_RESPONSE_MAX);
    access_job *job = &worker.jobs[0];
    HARD_ASSERT(job->state == JOB_FREE);
    if (!access_admin_parse(request, strlen(request), &job->request))
        return 0;
    job->id = ++worker.next_id;
    job->permitted = permitted;
    job->state = JOB_DONE;
    job->output_len = strlen(response);
    memcpy(job->output, response, job->output_len);
    return job->id;
}
static access_session_state_t test_session_sequence[8];
static size_t test_session_count, test_session_position;
void access_server_session_sequence_for_test(const access_session_state_t *states, size_t count) {
    HARD_ASSERT(count <= arraysize(test_session_sequence));
    if (count != 0)
        memcpy(test_session_sequence, states, count * sizeof(*states));
    test_session_count = count;
    test_session_position = 0;
}
#endif
access_session_state_t access_server_session_check(const access_token_ref_t *ref) {
#ifdef ATRINIK_TESTING
    if (test_session_position < test_session_count)
        return test_session_sequence[test_session_position++];
#endif
    if (atomic_load(&worker.failed) || worker.store == NULL)
        return ACCESS_SESSION_DENIED;
    return access_store_session_check(worker.store, ref, access_clock_now());
}
bool access_server_shutdown(void) {
    if (worker.started) {
        pthread_mutex_lock(&worker.mutex);
        worker.stopping = true;
        pthread_cond_signal(&worker.ready);
        pthread_mutex_unlock(&worker.mutex);
        pthread_join(worker.thread, NULL);
        pthread_join(worker.expiry_thread, NULL);
        worker.started = false;
        for (size_t i = 0; i < ACCESS_OUTBOX_LIMIT; i++)
            clear_job(&worker.jobs[i]);
    }
    bool ok =
        worker.store == NULL || access_store_flush_for_shutdown(worker.store) == ACCESS_COMMITTED;
    return ok && !atomic_load(&worker.failed);
}
void access_server_deinit(void) {
    (void)access_server_shutdown();
    access_store_close(worker.store);
    worker.store = NULL;
}
