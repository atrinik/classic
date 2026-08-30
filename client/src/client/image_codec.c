/**
 * @file
 * Serializes SDL_image while the client decodes faces in parallel.
 */

#include <image_codec.h>

#include <SDL3_image/SDL_image.h>
#include <toolkit/toolkit.h>

static SDL_Mutex *image_codec_mutex;
static SDL_Condition *image_codec_condition;
static size_t image_codec_main_waiters;
static bool image_codec_main_active;
static bool image_codec_background_active;
#ifdef ATRINIK_IMAGE_CODEC_TESTING
static size_t image_codec_background_waiters;
#endif

bool image_codec_parallel_start(void) {
    if (image_codec_mutex == NULL) {
        image_codec_mutex = SDL_CreateMutex();
    }
    if (image_codec_condition == NULL) {
        image_codec_condition = SDL_CreateCondition();
    }
    if (image_codec_mutex == NULL || image_codec_condition == NULL) {
        image_codec_parallel_stop();
        return false;
    }
    return true;
}

void image_codec_parallel_stop(void) {
    HARD_ASSERT(!image_codec_main_active);
    HARD_ASSERT(!image_codec_background_active);
    HARD_ASSERT(image_codec_main_waiters == 0);
#ifdef ATRINIK_IMAGE_CODEC_TESTING
    HARD_ASSERT(image_codec_background_waiters == 0);
#endif
    if (image_codec_condition != NULL) {
        SDL_DestroyCondition(image_codec_condition);
        image_codec_condition = NULL;
    }
    if (image_codec_mutex != NULL) {
        SDL_DestroyMutex(image_codec_mutex);
        image_codec_mutex = NULL;
    }
}

static bool image_codec_enter(bool background) {
    if (image_codec_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(image_codec_mutex);
    if (background) {
#ifdef ATRINIK_IMAGE_CODEC_TESTING
        image_codec_background_waiters++;
        SDL_BroadcastCondition(image_codec_condition);
#endif
    } else {
        image_codec_main_waiters++;
#ifdef ATRINIK_IMAGE_CODEC_TESTING
        SDL_BroadcastCondition(image_codec_condition);
#endif
    }
    while (background ? image_codec_main_active || image_codec_main_waiters != 0 ||
                            image_codec_background_active
                      : image_codec_background_active || image_codec_main_active) {
        SDL_WaitCondition(image_codec_condition, image_codec_mutex);
    }
    if (background) {
#ifdef ATRINIK_IMAGE_CODEC_TESTING
        HARD_ASSERT(image_codec_background_waiters != 0);
        image_codec_background_waiters--;
#endif
        image_codec_background_active = true;
    } else {
        HARD_ASSERT(image_codec_main_waiters != 0);
        image_codec_main_waiters--;
        image_codec_main_active = true;
    }
    SDL_UnlockMutex(image_codec_mutex);
    return true;
}

static void image_codec_leave(bool background, bool gated) {
    if (!gated) {
        return;
    }
    SDL_LockMutex(image_codec_mutex);
    if (background) {
        HARD_ASSERT(image_codec_background_active);
        image_codec_background_active = false;
    } else {
        HARD_ASSERT(image_codec_main_active);
        image_codec_main_active = false;
    }
    SDL_BroadcastCondition(image_codec_condition);
    SDL_UnlockMutex(image_codec_mutex);
}

#ifdef ATRINIK_IMAGE_CODEC_TESTING
bool image_codec_test_enter(bool background) {
    return image_codec_enter(background);
}

void image_codec_test_leave(bool background) {
    image_codec_leave(background, true);
}

void image_codec_test_wait_for_waiters(size_t main_waiters, size_t background_waiters) {
    HARD_ASSERT(image_codec_mutex != NULL);
    SDL_LockMutex(image_codec_mutex);
    while (image_codec_main_waiters < main_waiters ||
           image_codec_background_waiters < background_waiters) {
        SDL_WaitCondition(image_codec_condition, image_codec_mutex);
    }
    SDL_UnlockMutex(image_codec_mutex);
}
#endif

SDL_Surface *image_codec_load(const char *path) {
    bool gated = image_codec_enter(false);
    SDL_Surface *surface = IMG_Load(path);
    image_codec_leave(false, gated);
    return surface;
}

SDL_Surface *image_codec_load_io(SDL_IOStream *stream, bool close_stream) {
    bool gated = image_codec_enter(false);
    SDL_Surface *surface = IMG_Load_IO(stream, close_stream);
    image_codec_leave(false, gated);
    return surface;
}

SDL_Surface *image_codec_load_png_io(SDL_IOStream *stream) {
    bool gated = image_codec_enter(false);
    SDL_Surface *surface = IMG_LoadPNG_IO(stream);
    image_codec_leave(false, gated);
    return surface;
}

SDL_Surface *image_codec_load_png_io_background(SDL_IOStream *stream) {
    bool gated = image_codec_enter(true);
    SDL_Surface *surface = IMG_LoadPNG_IO(stream);
    image_codec_leave(true, gated);
    return surface;
}

bool image_codec_save_png(SDL_Surface *surface, const char *path) {
    bool gated = image_codec_enter(false);
    bool saved = IMG_SavePNG(surface, path);
    image_codec_leave(false, gated);
    return saved;
}

bool image_codec_save_png_io(SDL_Surface *surface, SDL_IOStream *stream, bool close_stream) {
    bool gated = image_codec_enter(false);
    bool saved = IMG_SavePNG_IO(surface, stream, close_stream);
    image_codec_leave(false, gated);
    return saved;
}
