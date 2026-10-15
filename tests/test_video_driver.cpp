// SPDX-License-Identifier: GPL-2.0-or-later
// Window system chosen for an SDL video driver (gpu/shim/video_driver.h). No SDL, no Vulkan.
#include <cstdio>
#include <cstdlib>

#include "shim/video_driver.h"

using Frontend::ClassifyVideoDriver;
using Frontend::WindowSystemType;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

int main() {
    WindowSystemType type = WindowSystemType::Metal;
    // The compositors the presenter has always supported keep their handles.
    CHECK(ClassifyVideoDriver("x11", type) && type == WindowSystemType::X11);
    CHECK(ClassifyVideoDriver("wayland", type) && type == WindowSystemType::Wayland);
    // SDL's windowless drivers present to a headless surface (VK_EXT_headless_surface).
    CHECK(ClassifyVideoDriver("offscreen", type) && type == WindowSystemType::Headless);
    CHECK(ClassifyVideoDriver("dummy", type) && type == WindowSystemType::Headless);
    // Anything else is refused, and the output is left alone.
    type = WindowSystemType::Windows;
    CHECK(!ClassifyVideoDriver("kmsdrm", type) && type == WindowSystemType::Windows);
    CHECK(!ClassifyVideoDriver("X11", type)); // names are SDL's, lowercase
    CHECK(!ClassifyVideoDriver("", type));
    CHECK(!ClassifyVideoDriver(nullptr, type));
    if (failures) {
        return 1;
    }
    std::puts("PASS: video driver classification (x11, wayland, offscreen, dummy; others refused)");
    return 0;
}
