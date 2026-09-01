/**
 * @file
 * Bounded asynchronous private face-cache writer.
 */

#include <face_cache.h>
#include <config.h>
#include <SDL3/SDL.h>
#include <toolkit/logger.h>
#include <toolkit/memory.h>
#include <toolkit/path.h>
#include <toolkit/toolkit.h>
#include <wrapper.h>

#define FACE_CACHE_WRITE_MAX 64U

typedef struct face_cache_write {
    struct face_cache_write *next;
    char *path;
    uint8_t *data;
    size_t size;
} face_cache_write_t;

static face_cache_write_t *face_cache_writes;
static face_cache_write_t *face_cache_writes_tail;
static size_t face_cache_write_count;
static size_t face_cache_write_failures;
static SDL_Mutex *face_cache_mutex;
static SDL_Condition *face_cache_condition;
static SDL_Thread *face_cache_thread;
static bool face_cache_stopping;
static char *face_cache_directory;

static int SDLCALL face_cache_worker(void *unused) {
    while (true) {
        SDL_LockMutex(face_cache_mutex);
        while (face_cache_writes == NULL && !face_cache_stopping) {
            SDL_WaitCondition(face_cache_condition, face_cache_mutex);
        }
        if (face_cache_stopping) {
            SDL_UnlockMutex(face_cache_mutex);
            break;
        }
        face_cache_write_t *write = face_cache_writes;
        face_cache_writes = write->next;
        if (face_cache_writes == NULL) {
            face_cache_writes_tail = NULL;
        }
        HARD_ASSERT(face_cache_write_count != 0);
        face_cache_write_count--;
        SDL_UnlockMutex(face_cache_mutex);

        if (!path_write_atomic_existing(write->path, write->data, write->size, 0600)) {
            SDL_LockMutex(face_cache_mutex);
            face_cache_write_failures++;
            SDL_UnlockMutex(face_cache_mutex);
        }
        free(write->data);
        free(write->path);
        free(write);
    }
    return 0;
}

bool face_cache_start(void) {
    if (face_cache_thread != NULL) {
        return true;
    }
    if (face_cache_mutex == NULL) {
        face_cache_mutex = SDL_CreateMutex();
    }
    if (face_cache_condition == NULL) {
        face_cache_condition = SDL_CreateCondition();
    }
    if (face_cache_mutex == NULL || face_cache_condition == NULL) {
        return false;
    }
    if (face_cache_directory == NULL) {
        char *marker = file_path(DIRECTORY_CACHE "/.face-cache-root", "wb");
        if (marker == NULL) {
            LOG(ERROR, "Could not prepare the face cache directory");
            return false;
        }
        face_cache_directory = path_dirname(marker);
        free(marker);
        if (face_cache_directory == NULL) {
            LOG(ERROR, "Could not derive the face cache directory");
            return false;
        }
    }
    face_cache_stopping = false;
    face_cache_thread = SDL_CreateThread(face_cache_worker, "face-cache", NULL);
    return face_cache_thread != NULL;
}

void face_cache_enqueue(const char *name, const uint8_t *data, size_t size) {
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        data == NULL || size == 0) {
        LOG(ERROR, "Refusing unsafe face cache write");
        return;
    }
    if (!face_cache_start()) {
        LOG(ERROR, "Could not start the face cache writer");
        return;
    }

    SDL_LockMutex(face_cache_mutex);
    if (face_cache_write_count >= FACE_CACHE_WRITE_MAX) {
        SDL_UnlockMutex(face_cache_mutex);
        LOG(ERROR, "Skipping face cache write: pending write limit reached");
        return;
    }
    face_cache_write_t *write = xcalloc(1, sizeof(*write));
    write->path = path_join(face_cache_directory, name);
    write->data = xmalloc(size);
    memcpy(write->data, data, size);
    write->size = size;
    if (face_cache_writes_tail != NULL) {
        face_cache_writes_tail->next = write;
    } else {
        face_cache_writes = write;
    }
    face_cache_writes_tail = write;
    face_cache_write_count++;
    SDL_SignalCondition(face_cache_condition);
    SDL_UnlockMutex(face_cache_mutex);
}

void face_cache_report_failures(void) {
    if (face_cache_mutex == NULL) {
        return;
    }
    SDL_LockMutex(face_cache_mutex);
    size_t failures = face_cache_write_failures;
    face_cache_write_failures = 0;
    SDL_UnlockMutex(face_cache_mutex);
    if (failures != 0) {
        LOG(ERROR,
            "Could not write %" PRIu64 " face cache file%s",
            (uint64_t)failures,
            failures == 1 ? "" : "s");
    }
}

void face_cache_stop(void) {
    if (face_cache_thread != NULL) {
        SDL_LockMutex(face_cache_mutex);
        face_cache_stopping = true;
        SDL_SignalCondition(face_cache_condition);
        SDL_UnlockMutex(face_cache_mutex);
        SDL_WaitThread(face_cache_thread, NULL);
        face_cache_thread = NULL;
    }
    while (face_cache_writes != NULL) {
        face_cache_write_t *write = face_cache_writes;
        face_cache_writes = write->next;
        free(write->data);
        free(write->path);
        free(write);
    }
    face_cache_writes_tail = NULL;
    face_cache_write_count = 0;
    face_cache_report_failures();
    if (face_cache_condition != NULL) {
        SDL_DestroyCondition(face_cache_condition);
    }
    if (face_cache_mutex != NULL) {
        SDL_DestroyMutex(face_cache_mutex);
    }
    face_cache_condition = NULL;
    face_cache_mutex = NULL;
    face_cache_stopping = false;
    free(face_cache_directory);
    face_cache_directory = NULL;
}
