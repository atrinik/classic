#include <toolkit/signals.h>

#include "../signals_private.h"

#include <signal.h>

#ifdef WIN32
#define TEST_EXCEPTION_CODE ((DWORD)0xe0423501)

static LONG WINAPI handled_exception_handler(EXCEPTION_POINTERS *exception_info) {
    if (exception_info->ExceptionRecord->ExceptionCode == TEST_EXCEPTION_CODE) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((noinline)) static void trigger_access_violation(void) {
    volatile int *invalid = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (invalid == NULL) {
        exit(2);
    }

    *invalid = 1;
}
#else
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define MISSING_HOME_PREFIX "libatrinik-signals-missing-home"

static bool is_traceback(const struct dirent *entry) {
    static const char prefix[] = MISSING_HOME_PREFIX "-traceback-";
    const char *timestamp;
    struct stat status;
    size_t i;

    if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1) != 0 ||
        stat(entry->d_name, &status) != 0 || !S_ISREG(status.st_mode)) {
        return false;
    }

    timestamp = entry->d_name + sizeof(prefix) - 1;
    if (strlen(timestamp) != strlen("0000_00_00_00-00-00.txt") ||
        timestamp[4] != '_' || timestamp[7] != '_' ||
        timestamp[10] != '_' || timestamp[13] != '-' ||
        timestamp[16] != '-' || strcmp(timestamp + 19, ".txt") != 0) {
        return false;
    }

    for (i = 0; i < 19; i++) {
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 &&
            !isdigit((unsigned char)timestamp[i])) {
            return false;
        }
    }

    return true;
}

static int remove_tracebacks(bool verify) {
    DIR *directory = opendir(".");
    struct dirent *entry;
    int found = 0;

    if (directory == NULL) {
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        if (!is_traceback(entry)) {
            continue;
        }

        if (verify) {
            char contents[HUGE_BUF];
            FILE *fp = fopen(entry->d_name, "r");
            size_t length;

            if (fp == NULL) {
                closedir(directory);
                return -1;
            }
            length = fread(contents, 1, sizeof(contents) - 1, fp);
            fclose(fp);
            contents[length] = '\0';
            if (length == 0 || strstr(contents, "Caught SIGABRT") == NULL) {
                closedir(directory);
                return -1;
            }
        }

        found++;
        if (remove(entry->d_name) != 0) {
            closedir(directory);
            return -1;
        }
    }

    closedir(directory);
    return found;
}
#endif

int main(int argc, char **argv) {
    toolkit_import(signals);

#ifdef WIN32
    if (argc == 2 && strcmp(argv[1], "--crash") == 0) {
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        signals_set_traceback_prefix("libatrinik-signals-test");
        trigger_access_violation();
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--handled-exception") == 0) {
        PVOID handler = AddVectoredExceptionHandler(0, handled_exception_handler);
        if (handler == NULL) {
            return 1;
        }
        RaiseException(TEST_EXCEPTION_CODE, 0, 0, NULL);
        if (RemoveVectoredExceptionHandler(handler) == 0) {
            return 1;
        }
    }
    if (argc == 2 && strcmp(argv[1], "--exception-guard") == 0) {
        volatile LONG stored_code = 0;
        signals_exception_claim first =
            signals_claim_exception(&stored_code, EXCEPTION_INT_DIVIDE_BY_ZERO);
        signals_exception_claim nested =
            signals_claim_exception(&stored_code, EXCEPTION_ACCESS_VIOLATION);

        if (!first.acquired || first.code != EXCEPTION_INT_DIVIDE_BY_ZERO ||
            nested.acquired || nested.code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
            return 1;
        }
    }
#else
    if (argc == 2 && strcmp(argv[1], "--missing-home") == 0) {
        if (unsetenv("HOME") != 0 || remove_tracebacks(false) < 0) {
            return 1;
        }
        signals_set_traceback_prefix(MISSING_HOME_PREFIX);
        if (raise(SIGABRT) != 0 || remove_tracebacks(true) != 1) {
            return 1;
        }
        if (setenv("HOME", "libatrinik-signals-home-does-not-exist", 1) != 0 ||
            raise(SIGABRT) != 0 || remove_tracebacks(true) != 1) {
            return 1;
        }
        toolkit_deinit();
        return 0;
    }
#endif

    signals_enable_graceful_termination();

    if (raise(SIGTERM) != 0 || !signals_termination_requested()) {
        toolkit_deinit();
        return 1;
    }

    toolkit_deinit();
    return 0;
}
