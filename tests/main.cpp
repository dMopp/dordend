#define DOCTEST_CONFIG_IMPLEMENT
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/i18n.hpp"
#include "nordend/engines/engines.hpp"
#include "nordend/formats/image/load.hpp"
#include "nordend/formats/image/photo.hpp"
#include "nordend/formats/text/read.hpp"
#include "nordend/fs/trash.hpp"
#include "nordend/picture/load.hpp"
#include "nordend/text/text.hpp"

int main(int argc, char** argv) {
    // The tests read the English interface, whatever language the machine speaks.
    nordend::i18n::set_language(nordend::i18n::Language::english);
    // No test reads or makes the user's trash: the trash folder lists one of its own, and no
    // disk's.
    std::string root = (std::filesystem::temp_directory_path() / "nordend-tests-XXXXXX").string();
    if (mkdtemp(root.data()) == nullptr) {
        return 1;
    }
    nordend::fs::set_trash_sources({.home = std::filesystem::path{root} / "Trash",
                                    .tops = std::vector<std::filesystem::path>{},
                                    .uid = geteuid()});
    // Nor the user's configuration, data, cache or state: a key that writes there (Ctrl+D adds a
    // bookmark to gtk-3.0/bookmarks) writes into the test root. Set before any thread starts.
    for (const auto& [name, folder] :
         {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
          std::pair{"XDG_CACHE_HOME", "cache"}, std::pair{"XDG_STATE_HOME", "state"}}) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread yet
        setenv(name, (std::filesystem::path{root} / folder).c_str(), 1);
    }
    // The engines Nordend ships, from their manifests in the source tree (their programs are not
    // started here).
    nordend::engines::set_installed(std::make_shared<const nordend::engines::Registry>(
        nordend::engines::Registry::load(NORDEND_ENGINES, [](std::string_view program) {
            return std::filesystem::path{program};
        })));
    // Pictures made in this process, by the image family itself (Nordend sends them to its
    // isolated helpers instead; tests/helpers_test.cpp covers that path).
    nordend::picture::set_loader([](const nordend::picture::LoadRequest& request) {
        return nordend::formats::image::load(request);
    });
    nordend::picture::set_info_loader([](const std::filesystem::path& path) {
        return nordend::formats::image::read_photo_info(path);
    });
    nordend::text::set_loader([](const nordend::text::LoadRequest& request) {
        return nordend::formats::text::load(request);
    });
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}
