#include <toolkit/signals.h>

#include <signal.h>

int main(int argc, char **argv) {
    toolkit_import(signals);

#ifdef WIN32
    if (argc == 2 && strcmp(argv[1], "--crash") == 0) {
        const ULONG_PTR exception_arguments[] = {1, 1};

        signals_set_traceback_prefix("libatrinik-signals-test");
        RaiseException(EXCEPTION_ACCESS_VIOLATION,
                       EXCEPTION_NONCONTINUABLE,
                       arraysize(exception_arguments),
                       exception_arguments);
        return 1;
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
