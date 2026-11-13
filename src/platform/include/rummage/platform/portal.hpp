#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {

// The desktop's look as the freedesktop portal tells it (org.freedesktop.portal.Settings,
// namespace org.freedesktop.appearance), which GNOME and Plasma both fill in.
struct Appearance {
    // color-scheme: 0 no preference, 1 dark, 2 light. No preference reads as light, as GTK and
    // Qt apps read it.
    bool dark{false};
    // accent-color, sRGB components 0 to 1; none when the desktop has no accent.
    std::optional<std::array<double, 3>> accent;

    bool operator==(const Appearance&) const = default;
};

// The portal's values, checked: an unknown scheme is no preference, an accent outside 0..1 (the
// portal's way of saying "unset") is none.
[[nodiscard]] Appearance appearance_from(std::optional<std::uint32_t> color_scheme,
                                         std::optional<std::array<double, 3>> accent);

// Reads the appearance at start (waiting a moment for the portal, so the first frame has the
// right colours) and follows its changes. Without a session bus or a portal, available() is
// false and nothing else happens.
//
// Loop thread only. Must not outlive the loop.
class DesktopPortal {
public:
    // changed runs when appearance() changed after the start.
    DesktopPortal(core::EventLoop& loop, std::function<void()> changed);
    ~DesktopPortal();

    DesktopPortal(const DesktopPortal&) = delete;
    DesktopPortal& operator=(const DesktopPortal&) = delete;
    DesktopPortal(DesktopPortal&&) = delete;
    DesktopPortal& operator=(DesktopPortal&&) = delete;

    // The portal answered; until then (or without one) appearance() means nothing.
    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] const Appearance& appearance() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rummage::platform
