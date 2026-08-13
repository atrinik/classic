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
    (void)argc;
    (void)argv;
#endif

    signals_enable_graceful_termination();

    if (raise(SIGTERM) != 0 || !signals_termination_requested()) {
        toolkit_deinit();
        return 1;
    }

    toolkit_deinit();
    return 0;
}
