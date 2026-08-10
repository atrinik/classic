#include <toolkit/signals.h>

#include <signal.h>

int main(void) {
    toolkit_import(signals);
    signals_enable_graceful_termination();

    if (raise(SIGTERM) != 0 || !signals_termination_requested()) {
        toolkit_deinit();
        return 1;
    }

    toolkit_deinit();
    return 0;
}
