/** @file Tests for bounded asynchronous face-cache writes. */

#include <global.h>
#include <face_cache.h>
#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <toolkit/string.h>
#include <wrapper.h>

static char test_directory[HUGE_BUF];

char *file_path(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return path_join(test_directory, ".face-cache-root");
}

static bool wait_for_file(const char *path) {
    uint64_t deadline = datetime_monotonic_ms() + 2000U;
    struct stat info;
    while (datetime_monotonic_ms() < deadline) {
        if (stat(path, &info) == 0) {
            return S_ISREG(info.st_mode);
        }
        SDL_Delay(1);
    }
    return false;
}

int main(void) {
    toolkit_import(logger);
    toolkit_import(path);

    snprintf(VS(test_directory), "/tmp/atrinik-face-cache-XXXXXX");
    if (mkdtemp(test_directory) == NULL || !face_cache_start() || !face_cache_start()) {
        return 1;
    }

    static const uint8_t contents[] = {0x89, 0x50, 0x4e, 0x47};
    face_cache_enqueue("../face.png", contents, sizeof(contents));
    face_cache_enqueue("face.png", NULL, 0);
    face_cache_enqueue("face.png", contents, sizeof(contents));
    char *path = path_join(test_directory, "face.png");
    bool passed = wait_for_file(path);
    if (passed) {
        uint8_t actual[sizeof(contents)];
        FILE *fp = fopen(path, "rb");
        passed = fp != NULL;
        if (fp != NULL) {
            passed = fread(actual, 1, sizeof(actual), fp) == sizeof(actual) &&
                     memcmp(actual, contents, sizeof(actual)) == 0;
            passed &= fclose(fp) == 0;
        }
    }

    face_cache_enqueue("missing/face.png", contents, sizeof(contents));
    SDL_Delay(20);
    face_cache_report_failures();
    face_cache_report_failures();
    face_cache_stop();
    face_cache_stop();

    unlink(path);
    free(path);
    rmdir(test_directory);
    toolkit_deinit();
    return passed ? 0 : 1;
}
