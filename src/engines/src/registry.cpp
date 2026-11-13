#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/core/file.hpp"
#include "rummage/core/log.hpp"
#include "rummage/engines/engines.hpp"

namespace rummage::engines {
namespace {

std::mutex installed_mutex;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): set once at start-up
std::shared_ptr<const Registry> installed_registry;

// The directory of the running program; empty when it cannot be known.
std::filesystem::path program_directory() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : self.parent_path();
}

}  // namespace

Registry Registry::load(const std::filesystem::path& directory, const Locate& locate) {
    Registry registry;
    std::vector<std::filesystem::path> manifests;
    std::error_code error;
    for (std::filesystem::directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error)) {
        if (it->path().extension() == ".toml" && it->is_regular_file(error)) {
            manifests.push_back(it->path());
        }
    }
    std::ranges::sort(manifests);
    for (const std::filesystem::path& path : manifests) {
        const core::TextFile file = core::read_text_file(path);
        if (!file.error.empty()) {
            log::warn("engine {}: {}; skipped", path.string(), file.error);
            continue;
        }
        std::string why;
        std::optional<Manifest> manifest = parse_manifest(file.text, path.string(), why);
        if (!manifest) {
            log::warn("engine {}; skipped", why);
            continue;
        }
        std::filesystem::path program = locate(manifest->program);
        if (program.empty()) {
            log::warn("engine {}: its program {} is not installed; skipped", manifest->name,
                      manifest->program);
            continue;
        }
        const std::string name = manifest->name;
        if (!registry.add(Engine{.manifest = std::move(*manifest),
                                 .program = std::move(program),
                                 .manifest_path = path})) {
            log::warn("engine {} ({}) shows a type another engine shows; skipped", name,
                      path.string());
        }
    }
    return registry;
}

Registry Registry::load_installed() {
    Registry result = load(default_directory(), locate_program);
    // Explicit trusted installation paths, never paths taken from a previewed file.
    const char* value = std::getenv("RUMMAGE_ENGINE_PATH");
    std::string_view paths = value != nullptr ? value : "";
    while (!paths.empty()) {
        const auto colon = paths.find(':');
        const std::filesystem::path directory{paths.substr(0, colon)};
        if (directory.is_absolute()) {
            Registry extra = load(directory, [&](std::string_view program) {
                const auto candidate = directory.parent_path().parent_path().parent_path() /
                                       "libexec" / "rummage" / program;
                return ::access(candidate.c_str(), X_OK) == 0 ? candidate : std::filesystem::path{};
            });
            for (const auto& engine : extra.engines()) {
                static_cast<void>(result.add(engine));
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(colon + 1);
    }
    return result;
}

bool Registry::add(Engine engine) {
    for (const Family& family : engine.manifest.families) {
        for (const std::string& type : family.mime) {
            const bool taken =
                exact_.contains(type) || std::ranges::any_of(subclasses_, [&](const auto& entry) {
                    return entry.first == type;
                });
            if (taken) {
                return false;
            }
        }
    }
    const std::size_t index = engines_.size();
    for (std::size_t f = 0; f < engine.manifest.families.size(); ++f) {
        const Family& family = engine.manifest.families[f];
        for (const std::string& type : family.mime) {
            if (family.subclasses) {
                subclasses_.emplace_back(type, Place{.engine = index, .family = f});
            } else {
                exact_.emplace(type, Place{.engine = index, .family = f});
            }
        }
    }
    engines_.push_back(std::move(engine));
    return true;
}

const Family& Registry::at(Place place) const {
    return engines_[place.engine].manifest.families[place.family];
}

const Family* Registry::find(const fs::MimeDatabase& mime, fs::MimeId type) const {
    if (const auto it = exact_.find(std::string{mime.name(type)}); it != exact_.end()) {
        return &at(it->second);
    }
    for (const auto& [name, place] : subclasses_) {
        const fs::MimeId parent = mime.find(name);
        if (parent != fs::kUnknownMime && mime.is_a(type, parent)) {
            return &at(place);
        }
    }
    return nullptr;
}

const Engine* Registry::engine_of(const Family& family) const {
    for (const Engine& engine : engines_) {
        const auto& families = engine.manifest.families;
        if (!families.empty() && &family >= families.data() &&
            &family < families.data() + families.size()) {
            return &engine;
        }
    }
    return nullptr;
}

std::filesystem::path default_directory() {
    const std::filesystem::path here = program_directory();
    if (here.empty()) {
        return {};
    }
    for (const std::filesystem::path& candidate :
         {here / "engines", here.parent_path() / "share" / "rummage" / "engines"}) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path locate_program(std::string_view program) {
    if (program.empty() || program == "." || program == ".." ||
        program.find('/') != std::string_view::npos) {
        return {};
    }
    const std::filesystem::path here = program_directory();
    if (here.empty()) {
        return {};
    }
    for (const std::filesystem::path& candidate :
         {here / program, here.parent_path() / "libexec" / "rummage" / program}) {
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return {};
}

void set_installed(std::shared_ptr<const Registry> registry) {
    const std::scoped_lock lock{installed_mutex};
    installed_registry = std::move(registry);
}

std::shared_ptr<const Registry> installed() {
    static const auto kEmpty = std::make_shared<const Registry>();
    const std::scoped_lock lock{installed_mutex};
    return installed_registry ? installed_registry : kEmpty;
}

}  // namespace rummage::engines
