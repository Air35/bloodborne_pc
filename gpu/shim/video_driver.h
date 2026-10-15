// SPDX-License-Identifier: GPL-2.0-or-later
// Which window system the presenter uses for an SDL video driver. Pure: no SDL, no Vulkan, so
// it is unit-tested (tests/test_video_driver.cpp).
//
// sotc: SDL's windowless drivers ("offscreen", "dummy") map to Headless. The swapchain then
// lives on a VK_EXT_headless_surface (vk_platform.cpp), which lets the whole renderer run on a
// machine with no compositor: the development VM under lavapipe, frame dumps, goldens.
#pragma once
#include <cstdint>
#include <cstring>

namespace Frontend {

enum class WindowSystemType : std::uint8_t { Headless, Windows, X11, Wayland, Metal };

/// Writes the window system for SDL's current video driver name; false (and `type` untouched)
/// for a driver the presenter cannot present to. Names are SDL's own, lowercase.
inline bool ClassifyVideoDriver(const char* driver, WindowSystemType& type) {
    if (!driver) {
        return false;
    }
    if (!std::strcmp(driver, "x11")) {
        type = WindowSystemType::X11;
    } else if (!std::strcmp(driver, "wayland")) {
        type = WindowSystemType::Wayland;
    } else if (!std::strcmp(driver, "offscreen") || !std::strcmp(driver, "dummy")) {
        type = WindowSystemType::Headless;
    } else {
        return false;
    }
    return true;
}

} // namespace Frontend
