// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include <unistd.h>

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" __attribute__((weak)) volatile int runtime_restarting; // absent in the tests

void assert_fail_impl() {
    if (&runtime_restarting && runtime_restarting) {
        for (;;) {
            pause();
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    // sotc: where the assertion came from. The call chain (symbols of the GPU library and of
    // bb-probe, which is linked -rdynamic) is the only evidence a headless run leaves of which
    // Vulkan call failed. Best effort, async-signal-safe variant (no allocation).
    void* frames[32];
    const int depth = backtrace(frames, 32);
    backtrace_symbols_fd(frames, depth, STDERR_FILENO);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
