#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rummage::core {

struct PickerPattern {
    std::uint32_t kind{};  // 0: shell glob, 1: MIME type
    std::string value;
    bool operator==(const PickerPattern&) const = default;
};

struct PickerFilter {
    std::string label;
    std::vector<PickerPattern> patterns;
    bool operator==(const PickerFilter&) const = default;
};

struct PickerOption {
    std::string id;
    std::string label;
    bool operator==(const PickerOption&) const = default;
};

struct PickerChoice {
    std::string id;
    std::string label;
    std::vector<PickerOption> options;  // empty: boolean, values "true" and "false"
    std::string selected;
    bool operator==(const PickerChoice&) const = default;
};

struct PickerRequest {
    enum class Mode : std::uint32_t { open, save, save_files };
    Mode mode{Mode::open};
    bool multiple{};
    bool directory{};
    std::string title;
    std::string accept_label;
    std::string folder;
    std::string name;
    std::string parent_window;
    bool modal{true};
    std::vector<std::string> files;
    std::vector<PickerFilter> filters;
    std::uint32_t filter{};
    std::vector<PickerChoice> choices;
    bool operator==(const PickerRequest&) const = default;
};

struct PickerResult {
    std::uint32_t response{1};  // 0: accepted, 1: cancelled, 2: failed
    std::vector<std::string> paths;
    std::uint32_t filter{};
    std::vector<std::string> choices;
    bool operator==(const PickerResult&) const = default;
};

// Private, bounded binary protocol over anonymous files inherited by the picker child.
// Paths are byte strings (including filenames which are not UTF-8); never shell arguments.
void write_picker_request(int fd, const PickerRequest& request);
PickerRequest read_picker_request(int fd);
void write_picker_result(int fd, const PickerResult& result);
PickerResult read_picker_result(int fd);

}  // namespace rummage::core
