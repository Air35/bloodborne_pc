// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <execinfo.h>
#include <unistd.h>
#endif
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"

void assert_fail_impl() {
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifndef _WIN32
    // sotc: where the assertion came from. The call chain (symbols of the GPU library and of
    // bb-probe, which is linked -rdynamic) is the only evidence a headless run leaves of which
    // Vulkan call failed. Best effort, async-signal-safe variant (no allocation).
    void* frames[32];
    const int depth = backtrace(frames, 32);
    backtrace_symbols_fd(frames, depth, STDERR_FILENO);
#endif
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
