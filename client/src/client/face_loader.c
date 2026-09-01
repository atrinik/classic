/**
 * @file
 * Bounded priority-aware filesystem and PNG face loader.
 */

#include <face_asset.h>
#include <face_loader.h>
#include <image_codec.h>
#include <asset.h>
#include <sprite.h>
#include <toolkit/logger.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>
#include <SDL3/SDL.h>
#include <zlib.h>

#define FACE_LOADER_OUTSTANDING_MAX 8U
#define FACE_LOADER_BACKGROUND_OUTSTANDING_MAX 2U

static_assert(FACE_LOADER_BACKGROUND_OUTSTANDING_MAX < FACE_LOADER_OUTSTANDING_MAX,
              "foreground loads must retain reserved capacity");

typedef struct face_loader_job {
    struct face_loader_job *next;
    uint16_t face;
    uint64_t token;
    uint64_t sequence;
    face_loader_kind_t kind;
    bool foreground;
    bool urgent;
    bool cancelled;
    char *name;
    uint32_t checksum;
    long pack_offset;
    size_t pack_size;
    uint8_t *data;
    size_t size;
} face_loader_job_t;

static face_loader_job_t *face_loader_foreground_jobs;
static face_loader_job_t *face_loader_foreground_jobs_tail;
static face_loader_job_t *face_loader_background_jobs;
static face_loader_job_t *face_loader_background_jobs_tail;
static face_loader_job_t *face_loader_active_job;
static face_loader_result_t *face_loader_foreground_results;
static face_loader_result_t *face_loader_foreground_results_tail;
static face_loader_result_t *face_loader_background_results;
static face_loader_result_t *face_loader_background_results_tail;
static size_t face_loader_outstanding;
static size_t face_loader_background_outstanding;
static uint64_t face_loader_next_sequence;
static SDL_Mutex *face_loader_mutex;
static SDL_Condition *face_loader_condition;
static SDL_Thread *face_loader_thread;
static bool face_loader_stopping;
static unsigned int face_loader_foreground_job_streak;
static unsigned int face_loader_foreground_result_streak;
static char *face_loader_pack_path;
static char *face_loader_gfx_user_path;
static char *face_loader_gfx_current_path;
static char *face_loader_gfx_installed_path;
static char *face_loader_cache_path;
static SDL_PixelFormat face_loader_display_format;

static void
face_loader_job_append(face_loader_job_t **head, face_loader_job_t **tail, face_loader_job_t *job) {
    job->next = NULL;
    if (*tail != NULL) {
        (*tail)->next = job;
    } else {
        *head = job;
    }
    *tail = job;
}

static void face_loader_job_prepend(face_loader_job_t **head,
                                    face_loader_job_t **tail,
                                    face_loader_job_t *job) {
    job->next = *head;
    *head = job;
    if (*tail == NULL) {
        *tail = job;
    }
}

static face_loader_job_t *face_loader_job_pop(face_loader_job_t **head, face_loader_job_t **tail) {
    face_loader_job_t *job = *head;
    if (job != NULL) {
        *head = job->next;
        if (*head == NULL) {
            *tail = NULL;
        }
        job->next = NULL;
    }
    return job;
}

static face_loader_job_t *face_loader_job_remove(face_loader_job_t **head,
                                                 face_loader_job_t **tail,
                                                 uint16_t face,
                                                 uint64_t token) {
    face_loader_job_t *previous = NULL;
    for (face_loader_job_t *job = *head; job != NULL; job = job->next) {
        if (job->face != face || job->token != token) {
            previous = job;
            continue;
        }
        if (previous != NULL) {
            previous->next = job->next;
        } else {
            *head = job->next;
        }
        if (*tail == job) {
            *tail = previous;
        }
        job->next = NULL;
        return job;
    }
    return NULL;
}

static face_loader_job_t *face_loader_job_remove_first_urgent(face_loader_job_t **head,
                                                              face_loader_job_t **tail) {
    for (face_loader_job_t *job = *head; job != NULL; job = job->next) {
        if (job->urgent) {
            return face_loader_job_remove(head, tail, job->face, job->token);
        }
    }
    return NULL;
}

static face_loader_job_t *face_loader_job_remove_oldest(face_loader_job_t **head,
                                                        face_loader_job_t **tail) {
    face_loader_job_t *oldest = NULL;
    for (face_loader_job_t *job = *head; job != NULL; job = job->next) {
        if (oldest == NULL || job->sequence < oldest->sequence) {
            oldest = job;
        }
    }
    return oldest != NULL ? face_loader_job_remove(head, tail, oldest->face, oldest->token) : NULL;
}

static void face_loader_result_append(face_loader_result_t **head,
                                      face_loader_result_t **tail,
                                      face_loader_result_t *result) {
    result->next = NULL;
    if (*tail != NULL) {
        (*tail)->next = result;
    } else {
        *head = result;
    }
    *tail = result;
}

static void face_loader_result_prepend(face_loader_result_t **head,
                                       face_loader_result_t **tail,
                                       face_loader_result_t *result) {
    result->next = *head;
    *head = result;
    if (*tail == NULL) {
        *tail = result;
    }
}

static face_loader_result_t *face_loader_result_remove(face_loader_result_t **head,
                                                       face_loader_result_t **tail,
                                                       uint16_t face,
                                                       uint64_t token) {
    face_loader_result_t *previous = NULL;
    for (face_loader_result_t *result = *head; result != NULL; result = result->next) {
        if (result->face != face || result->token != token) {
            previous = result;
            continue;
        }
        if (previous != NULL) {
            previous->next = result->next;
        } else {
            *head = result->next;
        }
        if (*tail == result) {
            *tail = previous;
        }
        result->next = NULL;
        return result;
    }
    return NULL;
}

static face_loader_result_t *face_loader_result_remove_first_urgent(face_loader_result_t **head,
                                                                    face_loader_result_t **tail) {
    for (face_loader_result_t *result = *head; result != NULL; result = result->next) {
        if (result->urgent) {
            return face_loader_result_remove(head, tail, result->face, result->token);
        }
    }
    return NULL;
}

static face_loader_result_t *face_loader_result_remove_oldest(face_loader_result_t **head,
                                                              face_loader_result_t **tail) {
    face_loader_result_t *oldest = NULL;
    for (face_loader_result_t *result = *head; result != NULL; result = result->next) {
        if (oldest == NULL || result->sequence < oldest->sequence) {
            oldest = result;
        }
    }
    return oldest != NULL ? face_loader_result_remove(head, tail, oldest->face, oldest->token)
                          : NULL;
}

static void face_loader_job_free(face_loader_job_t *job) {
    if (job == NULL) {
        return;
    }
    free(job->name);
    free(job->data);
    free(job);
}

void face_loader_result_free(face_loader_result_t *result) {
    if (result == NULL) {
        return;
    }
    sprite_free_sprite(result->sprite);
    free(result->data);
    free(result->override_name);
    free(result);
}

static uint8_t *face_loader_read_file(const char *path, size_t *size, bool *opened) {
    HARD_ASSERT(path != NULL);
    HARD_ASSERT(size != NULL);
    HARD_ASSERT(opened != NULL);

    *size = 0;
    *opened = false;
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    *opened = true;

    struct stat info;
    bool valid = fstat(fileno(fp), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
                 (uint64_t)info.st_size <= ASSET_FACE_MAX_SIZE;
    uint8_t *data = valid ? xmalloc((size_t)info.st_size) : NULL;
    if (valid && fread(data, 1, (size_t)info.st_size, fp) != (size_t)info.st_size) {
        valid = false;
    }
    if (fclose(fp) != 0) {
        valid = false;
    }
    if (!valid) {
        free(data);
        return NULL;
    }
    *size = (size_t)info.st_size;
    return data;
}

static uint8_t *face_loader_read_pack(FILE *pack, long offset, size_t size) {
    if (pack == NULL || offset < 0 || size == 0 || size > ASSET_FACE_MAX_SIZE ||
        fseek(pack, offset, SEEK_SET) != 0) {
        return NULL;
    }
    uint8_t *data = xmalloc(size);
    if (fread(data, 1, size, pack) != size) {
        clearerr(pack);
        free(data);
        return NULL;
    }
    return data;
}

static sprite_struct *face_loader_decode(const uint8_t *data, size_t size, uint32_t checksum) {
    if (!face_asset_validate(data, size, checksum)) {
        return NULL;
    }
    SDL_IOStream *stream = SDL_IOFromConstMem(data, size);
    SDL_Surface *surface = stream != NULL ? image_codec_load_png_io_background(stream) : NULL;
    sprite_struct *sprite = surface != NULL ? sprite_from_surface(surface, 0, true) : NULL;
    if (sprite != NULL) {
        SDL_PixelFormat format = face_loader_display_format != SDL_PIXELFORMAT_UNKNOWN
                                     ? face_loader_display_format
                                     : sprite->bitmap->format;
        SDL_Surface *target = SDL_CreateSurface(1, 1, format);
        SDL_Rect source = {0, 0, 1, 1};
        SDL_Rect destination = {0, 0, 1, 1};
        /* SDL defers its full-source RLE scan until the first
         * destination-specific blit. Warm that mapping on the worker so the
         * first visible map frame keeps RLE's steady-state benefit without
         * inheriting its allocation/scan. If warming fails, disable RLE and
         * keep the render thread on the bounded generic blitter. */
        if (target == NULL || !SDL_BlitSurface(sprite->bitmap, &source, target, &destination)) {
            SDL_SetSurfaceRLE(sprite->bitmap, false);
        }
        if (target != NULL) {
            SDL_DestroySurface(target);
        }
    }
    if (stream != NULL) {
        SDL_CloseIO(stream);
    }
    return sprite;
}

static bool face_loader_path(char *path, size_t size, const char *directory, const char *name) {
    if (directory == NULL || name == NULL) {
        return false;
    }
    int length = snprintf(path,
                          size,
                          "%s%s%s",
                          directory,
                          *directory != '\0' && directory[strlen(directory) - 1] != '/' ? "/" : "",
                          name);
    return length >= 0 && (size_t)length < size;
}

static face_loader_result_t *face_loader_process_local(face_loader_job_t *job,
                                                       FILE *pack,
                                                       bool gfx_user_available,
                                                       bool gfx_current_available,
                                                       bool gfx_installed_available) {
    face_loader_result_t *result = xcalloc(1, sizeof(*result));
    result->face = job->face;
    result->token = job->token;
    result->kind = job->kind;

    char path[HUGE_BUF];
    size_t size = 0;
    bool opened = false;
    uint8_t *data = NULL;
    if (gfx_user_available && face_loader_path(VS(path), face_loader_gfx_user_path, job->name)) {
        data = face_loader_read_file(path, &size, &opened);
    }
    if (!opened && gfx_current_available &&
        face_loader_path(VS(path), face_loader_gfx_current_path, job->name)) {
        data = face_loader_read_file(path, &size, &opened);
    }
    if (!opened && gfx_installed_available &&
        face_loader_path(VS(path), face_loader_gfx_installed_path, job->name)) {
        data = face_loader_read_file(path, &size, &opened);
    }
    if (opened) {
        if (data != NULL) {
            uint32_t checksum = (uint32_t)crc32(1L, data, size);
            result->sprite = face_loader_decode(data, size, checksum);
            if (result->sprite != NULL) {
                char override[MAX_BUF];
                int length = snprintf(VS(override), DIRECTORY_GFX_USER "/%s", job->name);
                if (length > 0 && (size_t)length < sizeof(override)) {
                    result->override_name = xstrdup(override);
                    result->override_checksum = checksum;
                } else {
                    sprite_free_sprite(result->sprite);
                    result->sprite = NULL;
                }
            }
        }
        free(data);
        if (result->sprite != NULL) {
            return result;
        }
    }

    data = face_loader_read_pack(pack, job->pack_offset, job->pack_size);
    if (data != NULL) {
        result->sprite = face_loader_decode(data, job->pack_size, job->checksum);
        free(data);
        if (result->sprite != NULL) {
            return result;
        }
    }

    bool cache_opened = false;
    if (face_loader_path(VS(path), face_loader_cache_path, job->name)) {
        data = face_loader_read_file(path, &size, &cache_opened);
    }
    if (data != NULL) {
        result->sprite = face_loader_decode(data, size, job->checksum);
    }
    free(data);
    if (cache_opened && result->sprite == NULL) {
        unlink(path);
    }
    return result;
}

static face_loader_result_t *face_loader_process(face_loader_job_t *job,
                                                 FILE *pack,
                                                 bool gfx_user_available,
                                                 bool gfx_current_available,
                                                 bool gfx_installed_available) {
    uint64_t started_ns = SDL_GetTicksNS();
    face_loader_result_t *result;
    if (job->kind == FACE_LOADER_LOCAL) {
        result = face_loader_process_local(job,
                                           pack,
                                           gfx_user_available,
                                           gfx_current_available,
                                           gfx_installed_available);
    } else {
        result = xcalloc(1, sizeof(*result));
        result->face = job->face;
        result->token = job->token;
        result->kind = job->kind;
        result->sprite = face_loader_decode(job->data, job->size, job->checksum);
        result->data = job->data;
        result->size = job->size;
        job->data = NULL;
    }
    result->sequence = job->sequence;
    result->load_us = (SDL_GetTicksNS() - started_ns) / 1000U;
    return result;
}

static int SDLCALL face_loader_worker(void *unused) {
    (void)unused;
    FILE *pack = face_loader_pack_path != NULL ? fopen(face_loader_pack_path, "rb") : NULL;
    struct stat info;
    bool gfx_user_available = stat(face_loader_gfx_user_path, &info) == 0 && S_ISDIR(info.st_mode);
    bool gfx_current_available =
        stat(face_loader_gfx_current_path, &info) == 0 && S_ISDIR(info.st_mode);
    bool gfx_installed_available =
        strcmp(face_loader_gfx_installed_path, face_loader_gfx_current_path) != 0 &&
        stat(face_loader_gfx_installed_path, &info) == 0 && S_ISDIR(info.st_mode);

    while (true) {
        SDL_LockMutex(face_loader_mutex);
        while (!face_loader_stopping && face_loader_foreground_jobs == NULL &&
               face_loader_background_jobs == NULL) {
            SDL_WaitCondition(face_loader_condition, face_loader_mutex);
        }
        if (face_loader_stopping) {
            SDL_UnlockMutex(face_loader_mutex);
            break;
        }
        face_loader_job_t *job;
        if (face_loader_foreground_jobs != NULL && face_loader_foreground_job_streak >= 2U) {
            job = face_loader_job_remove_oldest(&face_loader_foreground_jobs,
                                                &face_loader_foreground_jobs_tail);
            if (job != NULL) {
                face_loader_foreground_job_streak = 0;
            }
        } else {
            job = NULL;
        }
        if (job == NULL) {
            job = face_loader_job_remove_first_urgent(&face_loader_foreground_jobs,
                                                      &face_loader_foreground_jobs_tail);
            if (job != NULL) {
                face_loader_foreground_job_streak++;
            }
        }
        if (job == NULL) {
            job = face_loader_job_pop(&face_loader_foreground_jobs,
                                      &face_loader_foreground_jobs_tail);
            if (job != NULL) {
                face_loader_foreground_job_streak = 0;
            }
        }
        if (job == NULL) {
            job = face_loader_job_pop(&face_loader_background_jobs,
                                      &face_loader_background_jobs_tail);
        }
        HARD_ASSERT(job != NULL);
        face_loader_active_job = job;
        SDL_UnlockMutex(face_loader_mutex);

        face_loader_result_t *result = face_loader_process(job,
                                                           pack,
                                                           gfx_user_available,
                                                           gfx_current_available,
                                                           gfx_installed_available);

        SDL_LockMutex(face_loader_mutex);
        HARD_ASSERT(face_loader_active_job == job);
        face_loader_active_job = NULL;
        bool discard = face_loader_stopping || job->cancelled;
        if (discard) {
            HARD_ASSERT(face_loader_outstanding != 0);
            face_loader_outstanding--;
            if (!job->foreground) {
                HARD_ASSERT(face_loader_background_outstanding != 0);
                face_loader_background_outstanding--;
            }
        } else {
            result->foreground = job->foreground;
            result->urgent = job->urgent;
            if (result->foreground) {
                face_loader_result_append(&face_loader_foreground_results,
                                          &face_loader_foreground_results_tail,
                                          result);
            } else {
                face_loader_result_append(&face_loader_background_results,
                                          &face_loader_background_results_tail,
                                          result);
            }
        }
        SDL_BroadcastCondition(face_loader_condition);
        SDL_UnlockMutex(face_loader_mutex);

        if (discard) {
            face_loader_result_free(result);
        }
        face_loader_job_free(job);
    }

    if (pack != NULL) {
        fclose(pack);
    }
    return 0;
}

bool face_loader_start(const char *pack_path,
                       const char *gfx_user_path,
                       const char *gfx_current_path,
                       const char *gfx_installed_path,
                       const char *cache_path,
                       uint32_t display_format) {
    if (gfx_user_path == NULL || gfx_current_path == NULL || gfx_installed_path == NULL ||
        cache_path == NULL) {
        return false;
    }
    if (face_loader_thread != NULL) {
        return true;
    }
    if (!image_codec_parallel_start()) {
        return false;
    }
    if (face_loader_mutex == NULL) {
        face_loader_mutex = SDL_CreateMutex();
    }
    if (face_loader_condition == NULL) {
        face_loader_condition = SDL_CreateCondition();
    }
    if (face_loader_mutex == NULL || face_loader_condition == NULL) {
        face_loader_stop();
        return false;
    }

    free(face_loader_pack_path);
    free(face_loader_gfx_user_path);
    free(face_loader_gfx_current_path);
    free(face_loader_gfx_installed_path);
    free(face_loader_cache_path);
    face_loader_pack_path = pack_path != NULL ? xstrdup(pack_path) : NULL;
    face_loader_gfx_user_path = xstrdup(gfx_user_path);
    face_loader_gfx_current_path = xstrdup(gfx_current_path);
    face_loader_gfx_installed_path = xstrdup(gfx_installed_path);
    face_loader_cache_path = xstrdup(cache_path);
    face_loader_display_format = (SDL_PixelFormat)display_format;
    face_loader_stopping = false;
    face_loader_thread = SDL_CreateThread(face_loader_worker, "face-loader", NULL);
    if (face_loader_thread == NULL) {
        face_loader_stop();
        return false;
    }
    return true;
}

bool face_loader_available(void) {
    if (face_loader_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(face_loader_mutex);
    bool available = face_loader_thread != NULL && !face_loader_stopping;
    SDL_UnlockMutex(face_loader_mutex);
    return available;
}

bool face_loader_can_submit(bool foreground) {
    if (face_loader_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(face_loader_mutex);
    bool available =
        face_loader_thread != NULL && !face_loader_stopping &&
        face_loader_outstanding < FACE_LOADER_OUTSTANDING_MAX &&
        (foreground || face_loader_background_outstanding < FACE_LOADER_BACKGROUND_OUTSTANDING_MAX);
    SDL_UnlockMutex(face_loader_mutex);
    return available;
}

bool face_loader_submit(const face_loader_request_t *request) {
    if (request == NULL || request->token == 0 ||
        (request->kind == FACE_LOADER_LOCAL && request->name == NULL) ||
        (request->kind == FACE_LOADER_NETWORK &&
         (request->data == NULL || request->size == 0 || request->size > ASSET_FACE_MAX_SIZE))) {
        return false;
    }

    if (face_loader_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(face_loader_mutex);
    if (face_loader_thread == NULL || face_loader_stopping ||
        face_loader_outstanding >= FACE_LOADER_OUTSTANDING_MAX ||
        (!request->foreground &&
         face_loader_background_outstanding >= FACE_LOADER_BACKGROUND_OUTSTANDING_MAX)) {
        SDL_UnlockMutex(face_loader_mutex);
        return false;
    }

    face_loader_job_t *job = xcalloc(1, sizeof(*job));
    job->face = request->face;
    job->token = request->token;
    face_loader_next_sequence++;
    if (face_loader_next_sequence == 0) {
        face_loader_next_sequence++;
    }
    job->sequence = face_loader_next_sequence;
    job->kind = request->kind;
    job->foreground = request->foreground;
    job->urgent = request->urgent;
    job->name = request->name != NULL ? xstrdup(request->name) : NULL;
    job->checksum = request->checksum;
    job->pack_offset = request->pack_offset;
    job->pack_size = request->pack_size;
    if (request->data != NULL && request->size != 0) {
        job->data = xmalloc(request->size);
        memcpy(job->data, request->data, request->size);
        job->size = request->size;
    }

    if (job->foreground) {
        face_loader_job_append(&face_loader_foreground_jobs,
                               &face_loader_foreground_jobs_tail,
                               job);
    } else {
        face_loader_job_append(&face_loader_background_jobs,
                               &face_loader_background_jobs_tail,
                               job);
    }
    face_loader_outstanding++;
    if (!job->foreground) {
        face_loader_background_outstanding++;
    }
    SDL_SignalCondition(face_loader_condition);
    SDL_UnlockMutex(face_loader_mutex);
    return true;
}

void face_loader_promote(uint16_t face, uint64_t token) {
    if (face_loader_mutex == NULL) {
        return;
    }
    SDL_LockMutex(face_loader_mutex);
    face_loader_job_t *job = face_loader_job_remove(&face_loader_foreground_jobs,
                                                    &face_loader_foreground_jobs_tail,
                                                    face,
                                                    token);
    bool was_background = false;
    if (job == NULL) {
        job = face_loader_job_remove(&face_loader_background_jobs,
                                     &face_loader_background_jobs_tail,
                                     face,
                                     token);
        was_background = job != NULL;
    }
    if (job != NULL) {
        if (was_background) {
            HARD_ASSERT(face_loader_background_outstanding != 0);
            face_loader_background_outstanding--;
        }
        job->foreground = true;
        job->urgent = true;
        face_loader_job_prepend(&face_loader_foreground_jobs,
                                &face_loader_foreground_jobs_tail,
                                job);
        SDL_SignalCondition(face_loader_condition);
        SDL_UnlockMutex(face_loader_mutex);
        return;
    }
    if (face_loader_active_job != NULL && face_loader_active_job->face == face &&
        face_loader_active_job->token == token) {
        if (!face_loader_active_job->foreground) {
            HARD_ASSERT(face_loader_background_outstanding != 0);
            face_loader_background_outstanding--;
        }
        face_loader_active_job->foreground = true;
        face_loader_active_job->urgent = true;
        SDL_UnlockMutex(face_loader_mutex);
        return;
    }
    face_loader_result_t *result = face_loader_result_remove(&face_loader_foreground_results,
                                                             &face_loader_foreground_results_tail,
                                                             face,
                                                             token);
    was_background = false;
    if (result == NULL) {
        result = face_loader_result_remove(&face_loader_background_results,
                                           &face_loader_background_results_tail,
                                           face,
                                           token);
        was_background = result != NULL;
    }
    if (result != NULL) {
        if (was_background) {
            HARD_ASSERT(face_loader_background_outstanding != 0);
            face_loader_background_outstanding--;
        }
        result->foreground = true;
        result->urgent = true;
        face_loader_result_prepend(&face_loader_foreground_results,
                                   &face_loader_foreground_results_tail,
                                   result);
    }
    SDL_UnlockMutex(face_loader_mutex);
}

void face_loader_cancel(uint16_t face, uint64_t token) {
    if (face_loader_mutex == NULL) {
        return;
    }
    SDL_LockMutex(face_loader_mutex);
    face_loader_job_t *job = face_loader_job_remove(&face_loader_foreground_jobs,
                                                    &face_loader_foreground_jobs_tail,
                                                    face,
                                                    token);
    if (job == NULL) {
        job = face_loader_job_remove(&face_loader_background_jobs,
                                     &face_loader_background_jobs_tail,
                                     face,
                                     token);
    }
    face_loader_result_t *result = NULL;
    if (job == NULL && face_loader_active_job != NULL && face_loader_active_job->face == face &&
        face_loader_active_job->token == token) {
        face_loader_active_job->cancelled = true;
        SDL_UnlockMutex(face_loader_mutex);
        return;
    }
    if (job == NULL) {
        result = face_loader_result_remove(&face_loader_foreground_results,
                                           &face_loader_foreground_results_tail,
                                           face,
                                           token);
        if (result == NULL) {
            result = face_loader_result_remove(&face_loader_background_results,
                                               &face_loader_background_results_tail,
                                               face,
                                               token);
        }
    }
    if (job != NULL || result != NULL) {
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
        bool foreground = job != NULL ? job->foreground : result->foreground;
        if (!foreground) {
            HARD_ASSERT(face_loader_background_outstanding != 0);
            face_loader_background_outstanding--;
        }
    }
    SDL_UnlockMutex(face_loader_mutex);
    face_loader_job_free(job);
    face_loader_result_free(result);
}

face_loader_result_t *face_loader_result_pop(bool allow_background) {
    if (face_loader_mutex == NULL) {
        return NULL;
    }
    SDL_LockMutex(face_loader_mutex);
    face_loader_result_t *result;
    if (face_loader_foreground_results != NULL && face_loader_foreground_result_streak >= 2U) {
        result = face_loader_result_remove_oldest(&face_loader_foreground_results,
                                                  &face_loader_foreground_results_tail);
        if (result != NULL) {
            face_loader_foreground_result_streak = 0;
        }
    } else {
        result = NULL;
    }
    if (result == NULL) {
        result = face_loader_result_remove_first_urgent(&face_loader_foreground_results,
                                                        &face_loader_foreground_results_tail);
        if (result != NULL) {
            face_loader_foreground_result_streak++;
        }
    }
    if (result == NULL) {
        result = face_loader_foreground_results;
        if (result != NULL) {
            face_loader_foreground_results = result->next;
            if (face_loader_foreground_results == NULL) {
                face_loader_foreground_results_tail = NULL;
            }
            face_loader_foreground_result_streak = 0;
        }
    }
    if (result == NULL && allow_background) {
        result = face_loader_background_results;
        if (result != NULL) {
            face_loader_background_results = result->next;
            if (face_loader_background_results == NULL) {
                face_loader_background_results_tail = NULL;
            }
            face_loader_foreground_result_streak = 0;
        }
    }
    if (result != NULL) {
        result->next = NULL;
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
        if (!result->foreground) {
            HARD_ASSERT(face_loader_background_outstanding != 0);
            face_loader_background_outstanding--;
        }
    }
    SDL_UnlockMutex(face_loader_mutex);
    return result;
}

#ifdef ATRINIK_FACE_REQUEST_TESTING
bool face_loader_test_wait_results(size_t minimum, uint32_t timeout_ms) {
    if (face_loader_mutex == NULL) {
        return false;
    }
    uint64_t deadline = SDL_GetTicks() + timeout_ms;
    SDL_LockMutex(face_loader_mutex);
    while (true) {
        size_t count = 0;
        for (face_loader_result_t *result = face_loader_foreground_results; result != NULL;
             result = result->next) {
            count++;
        }
        for (face_loader_result_t *result = face_loader_background_results; result != NULL;
             result = result->next) {
            count++;
        }
        if (count >= minimum) {
            SDL_UnlockMutex(face_loader_mutex);
            return true;
        }
        uint64_t now = SDL_GetTicks();
        if (now >= deadline ||
            !SDL_WaitConditionTimeout(face_loader_condition,
                                      face_loader_mutex,
                                      (Sint32)MIN(deadline - now, (uint64_t)INT32_MAX))) {
            SDL_UnlockMutex(face_loader_mutex);
            return false;
        }
    }
}
#endif

void face_loader_stop(void) {
    if (face_loader_mutex != NULL) {
        SDL_LockMutex(face_loader_mutex);
        face_loader_stopping = true;
        if (face_loader_condition != NULL) {
            SDL_SignalCondition(face_loader_condition);
        }
        SDL_UnlockMutex(face_loader_mutex);
    }
    if (face_loader_thread != NULL) {
        SDL_WaitThread(face_loader_thread, NULL);
        face_loader_thread = NULL;
    }

    face_loader_job_t *job;
    while ((job = face_loader_job_pop(&face_loader_foreground_jobs,
                                      &face_loader_foreground_jobs_tail)) != NULL) {
        face_loader_job_free(job);
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
    }
    while ((job = face_loader_job_pop(&face_loader_background_jobs,
                                      &face_loader_background_jobs_tail)) != NULL) {
        face_loader_job_free(job);
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
        HARD_ASSERT(face_loader_background_outstanding != 0);
        face_loader_background_outstanding--;
    }
    face_loader_result_t *result;
    while ((result = face_loader_foreground_results) != NULL) {
        face_loader_foreground_results = result->next;
        face_loader_result_free(result);
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
    }
    face_loader_foreground_results_tail = NULL;
    while ((result = face_loader_background_results) != NULL) {
        face_loader_background_results = result->next;
        face_loader_result_free(result);
        HARD_ASSERT(face_loader_outstanding != 0);
        face_loader_outstanding--;
        HARD_ASSERT(face_loader_background_outstanding != 0);
        face_loader_background_outstanding--;
    }
    face_loader_background_results_tail = NULL;
    HARD_ASSERT(face_loader_active_job == NULL);
    HARD_ASSERT(face_loader_outstanding == 0);
    HARD_ASSERT(face_loader_background_outstanding == 0);
    face_loader_foreground_job_streak = 0;
    face_loader_foreground_result_streak = 0;

    if (face_loader_condition != NULL) {
        SDL_DestroyCondition(face_loader_condition);
    }
    if (face_loader_mutex != NULL) {
        SDL_DestroyMutex(face_loader_mutex);
    }
    face_loader_condition = NULL;
    face_loader_mutex = NULL;
    face_loader_stopping = false;
    free(face_loader_pack_path);
    free(face_loader_gfx_user_path);
    free(face_loader_gfx_current_path);
    free(face_loader_gfx_installed_path);
    free(face_loader_cache_path);
    face_loader_pack_path = NULL;
    face_loader_gfx_user_path = NULL;
    face_loader_gfx_current_path = NULL;
    face_loader_gfx_installed_path = NULL;
    face_loader_cache_path = NULL;
    face_loader_display_format = SDL_PIXELFORMAT_UNKNOWN;
    image_codec_parallel_stop();
}
