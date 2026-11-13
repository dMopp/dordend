#include <string>
#include <string_view>
#include <utility>

#include "rummage/picture/load.hpp"
#include "rummage/preview/preview.hpp"

namespace rummage::preview {

Summary summarize(const std::filesystem::path& path, std::string_view mime, Kind kind,
                  const core::CancelToken& cancel) {
    Summary summary;
    summary.info = read_info(path, cancel);
    if (kind == Kind::image && !mime.starts_with("video/")) {
        summary.photo = picture::load_photo_info(path);
    } else if (kind == Kind::text) {
        // A few kilobytes are enough for the lines the pane has room for.
        text::Loaded loaded = text::load_text({.path = path,
                                               .mime = std::string{mime},
                                               .limits = {.max_bytes = std::size_t{8} * 1024,
                                                          .max_line_bytes = 256,
                                                          .max_lines = kSummaryLines}});
        if (loaded.failure == text::Failure::none) {
            summary.text = std::move(loaded.text);
        }
    }
    return summary;
}

}  // namespace rummage::preview
