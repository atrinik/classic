/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <auth_worker.h>
#include <openssl/crypto.h>
#include <pthread.h>

typedef enum { WORK_FREE, WORK_QUEUED, WORK_RUNNING, WORK_DONE } work_state_t;
typedef struct {
    work_state_t state;
    bool cancelled;
    auth_work_t work;
} work_slot_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
/* crypt() returns shared storage, including on platforms without crypt_r(). */
#if defined(HAVE_CRYPT) && defined(HAVE_CRYPT_H)
static pthread_mutex_t crypt_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_t threads[AUTH_WORKER_THREADS];
static work_slot_t slots[AUTH_WORKER_CAPACITY];
static size_t thread_count;
static bool stopping;
#ifdef ATRINIK_TESTING
static bool paused;
#endif

static void execute(auth_work_t *work) {
    work->result = PASSWORD_VERIFY_MATCH;
    if (work->verify) {
        if (work->credential.legacy[0] != '\0') {
            work->result = PASSWORD_VERIFY_MISMATCH;
#if defined(HAVE_CRYPT) && defined(HAVE_CRYPT_H)
            pthread_mutex_lock(&crypt_mutex);
            char *calculated = crypt(work->password, work->credential.legacy);
            size_t length = strlen(work->credential.legacy);
            if (calculated != NULL && strlen(calculated) == length &&
                CRYPTO_memcmp(calculated, work->credential.legacy, length) == 0) {
                work->result = PASSWORD_VERIFY_MATCH;
            }
            if (calculated != NULL) {
                OPENSSL_cleanse(calculated, strlen(calculated));
            }
            pthread_mutex_unlock(&crypt_mutex);
#endif
        } else if (work->credential.pbkdf2) {
            work->result = password_pbkdf2_sha256_verify(work->password,
                                                        work->credential.salt,
                                                        work->credential.hash);
        } else {
            work->result = password_record_verify(work->password, work->credential.record);
        }
    }
    if (work->result == PASSWORD_VERIFY_MATCH && work->create &&
        !password_record_create(work->replacement, work->record)) {
        work->result = PASSWORD_VERIFY_ERROR;
    }
    OPENSSL_cleanse(work->password, sizeof(work->password));
    OPENSSL_cleanse(work->replacement, sizeof(work->replacement));
    OPENSSL_cleanse(&work->credential, sizeof(work->credential));
}

static void *run(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mutex);
    for (;;) {
        size_t index = AUTH_WORKER_CAPACITY;
        if (stopping) {
            break;
        }
        {
            /* Request IDs are monotonic, so select the oldest queued request. */
            for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
                if (slots[i].state == WORK_QUEUED &&
                    (index == AUTH_WORKER_CAPACITY ||
                     slots[i].work.request < slots[index].work.request)) {
                    index = i;
                }
            }
        }
        if (index == AUTH_WORKER_CAPACITY) {
            pthread_cond_wait(&wake, &mutex);
            continue;
        }
        work_slot_t *slot = &slots[index];
        slot->state = WORK_RUNNING;
        auth_work_t work = slot->work;
        OPENSSL_cleanse(&slot->work, sizeof(slot->work));
        /* Keep only IDs available for cancellation while the owned copy runs. */
        slot->work.generation = work.generation;
        slot->work.request = work.request;
        pthread_cond_broadcast(&wake);
#ifdef ATRINIK_TESTING
        while (paused && !stopping && !slot->cancelled) {
            pthread_cond_wait(&wake, &mutex);
        }
#endif
        bool cancelled = stopping || slot->cancelled;
        pthread_mutex_unlock(&mutex);
        if (!cancelled) {
            execute(&work);
        }
        pthread_mutex_lock(&mutex);
        if (stopping || slot->cancelled) {
            OPENSSL_cleanse(slot, sizeof(*slot));
        } else {
            slot->work = work;
            slot->state = WORK_DONE;
        }
        OPENSSL_cleanse(&work, sizeof(work));
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

bool auth_worker_start(void) {
    if (thread_count != 0) {
        return true;
    }
    stopping = false;
    for (size_t i = 0; i < AUTH_WORKER_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, run, NULL) != 0) {
            auth_worker_stop();
            return false;
        }
        thread_count++;
    }
    return true;
}

void auth_worker_stop(void) {
    pthread_mutex_lock(&mutex);
    stopping = true;
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        if (slots[i].state != WORK_RUNNING) {
            OPENSSL_cleanse(&slots[i], sizeof(slots[i]));
        }
    }
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&mutex);
    for (size_t i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    thread_count = 0;
    OPENSSL_cleanse(slots, sizeof(slots));
#ifdef ATRINIK_TESTING
    paused = false;
#endif
}

bool auth_worker_submit(const auth_work_t *work) {
    pthread_mutex_lock(&mutex);
    size_t queued = 0, available = AUTH_WORKER_CAPACITY;
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        queued += slots[i].state == WORK_QUEUED;
        if (slots[i].state == WORK_FREE) {
            available = i;
        }
    }
    bool ok = thread_count == AUTH_WORKER_THREADS && !stopping &&
              queued < AUTH_WORKER_QUEUE && available != AUTH_WORKER_CAPACITY;
    if (ok) {
        slots[available].work = *work;
        slots[available].state = WORK_QUEUED;
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&mutex);
    return ok;
}

bool auth_worker_take(auth_work_t *result) {
    pthread_mutex_lock(&mutex);
    bool found = false;
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        if (slots[i].state == WORK_DONE) {
            *result = slots[i].work;
            OPENSSL_cleanse(&slots[i], sizeof(slots[i]));
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&mutex);
    return found;
}

void auth_worker_cancel(uint64_t generation, uint64_t request) {
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        work_slot_t *slot = &slots[i];
        if (slot->state != WORK_FREE && slot->work.generation == generation &&
            slot->work.request == request) {
            if (slot->state == WORK_RUNNING) {
                slot->cancelled = true;
            } else {
                OPENSSL_cleanse(slot, sizeof(*slot));
            }
        }
    }
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&mutex);
}

#ifdef ATRINIK_TESTING
static bool wait_for_test(size_t running, bool idle) {
    struct timespec deadline;
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&mutex);
    bool ready;
    do {
        size_t active = 0, queued = 0;
        for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
            active += slots[i].state == WORK_RUNNING;
            queued += slots[i].state == WORK_QUEUED;
        }
        ready = idle ? active == 0 && queued == 0 : active == running;
        if (ready) {
            break;
        }
    } while (pthread_cond_timedwait(&wake, &mutex, &deadline) == 0);
    pthread_mutex_unlock(&mutex);
    return ready;
}

bool auth_worker_wait_running_for_test(size_t running) {
    return wait_for_test(running, false);
}

bool auth_worker_wait_idle_for_test(void) {
    return wait_for_test(0, true);
}

void auth_worker_pause_for_test(bool pause) {
    pthread_mutex_lock(&mutex);
    paused = pause;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&mutex);
}

size_t auth_worker_pending_for_test(void) {
    size_t count = 0;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        count += slots[i].state != WORK_FREE;
    }
    pthread_mutex_unlock(&mutex);
    return count;
}
#endif
