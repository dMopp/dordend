#pragma once
#include <filesystem>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {
// Runs the FileChooser backend on the session bus, without connecting to Wayland.
int run_file_picker_portal(core::EventLoop& loop, const std::filesystem::path& picker_program = {});
}  // namespace rummage::platform
