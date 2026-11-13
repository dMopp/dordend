#include <charconv>

#include "internal.hpp"

namespace rummage::svg {
namespace {
void charge(std::size_t& work, std::size_t amount) {
    require(amount <= 10000000 - work);
    work += amount;
}

std::map<std::string_view, std::string_view> declarations(std::string_view text) {
    std::map<std::string_view, std::string_view> result;
    while (!text.empty()) {
        auto end = text.find(';');
        auto part = text.substr(0, end);
        auto colon = part.find(':');
        if (colon != part.npos) {
            auto key = trim(part.substr(0, colon)), value = trim(part.substr(colon + 1));
            if (!key.empty() && !value.empty()) {
                result[key] = value;
            }
        }
        if (end == text.npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return result;
}

bool matches(std::string_view selector, const Node& node, std::size_t& work) {
    charge(work, 1 + selector.size());
    if (selector == "*") {
        return true;
    }
    if (selector.starts_with('#')) {
        charge(work, node.attr("id").size());
        return selector.substr(1) == node.attr("id");
    }
    if (selector.starts_with('.')) {
        auto classes = node.attr("class");
        charge(work, classes.size());
        while (!(classes = trim(classes)).empty()) {
            auto end = classes.find_first_of(" \t\r\n");
            if (classes.substr(0, end) == selector.substr(1)) {
                return true;
            }
            if (end == classes.npos) {
                break;
            }
            classes.remove_prefix(end);
        }
        return false;
    }
    charge(work, node.name.size());
    return node.name == selector;
}
}  // namespace

Sheet stylesheet(const std::vector<Node>& nodes) {
    Sheet sheet;
    std::size_t declaration_count = 0;
    sheet.storage.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (node.name != "style") {
            continue;
        }
        std::string clean;
        auto text = std::string_view(node.text);
        while (!text.empty()) {
            auto start = text.find("/*");
            if (start == text.npos) {
                clean += text;
                break;
            }
            clean += text.substr(0, start);
            auto end = text.find("*/", start + 2);
            require(end != text.npos);
            text.remove_prefix(end + 2);
        }
        sheet.storage.push_back(std::move(clean));
        text = sheet.storage.back();
        while (!(text = trim(text)).empty()) {
            auto open = text.find('{'), close = text.find('}');
            if (open == text.npos || close == text.npos || open > close) {
                break;
            }
            auto selectors = trim(text.substr(0, open));
            auto props = declarations(text.substr(open + 1, close - open - 1));
            while (!selectors.empty()) {
                auto end = selectors.find(',');
                auto selector = trim(selectors.substr(0, end));
                if (!selector.empty() && selector.find_first_of(" \t\r\n>+[:@") == selector.npos) {
                    declaration_count += props.size();
                    require(declaration_count <= 100000);
                    require(sheet.rules.size() < 10000);
                    sheet.rules.push_back({selector, props,
                                           selector.front() == '#'   ? 100
                                           : selector.front() == '.' ? 10
                                                                     : 1});
                }
                if (end == selectors.npos) {
                    break;
                }
                selectors.remove_prefix(end + 1);
            }
            text.remove_prefix(close + 1);
        }
    }
    return sheet;
}

Style style(const Node& node, const Style& parent, const Sheet& sheet, std::size_t& work) {
    Style out = parent;
    out.opacity = 1;
    out.display = true;
    out.clip = {};
    out.mask = {};
    out.filter = {};
    out.stop_color = {0, 0, 0, 1};
    out.stop_opacity = 1;
    out.mask_type = "luminance";
    std::map<std::string_view, std::pair<int, std::string_view>> props;
    for (const auto& [key, value] : node.attributes) {
        charge(work, 1 + key.size() + value.size());
        props[key] = {0, value};
    }
    for (const auto& rule : sheet.rules) {
        if (!matches(rule.selector, node, work)) {
            continue;
        }
        for (const auto& [key, value] : rule.declarations) {
            charge(work, 1 + key.size() + value.size());
            if (!props.contains(key) || props[key].first <= rule.specificity) {
                props[key] = {rule.specificity, value};
            }
        }
    }
    for (const auto& [key, value] : declarations(node.attr("style"))) {
        props[key] = {1000, value};
    }
    // Resolve color before currentColor in other properties.
    if (auto it = props.find("color"); it != props.end() && it->second.second != "inherit") {
        out.color = colour(it->second.second, parent.color);
    }
    for (const auto& [key, ranked] : props) {
        auto value = trim(ranked.second);
        bool inherit = value == "inherit";
        if (key == "fill") {
            out.fill = inherit ? parent.fill : value;
        } else if (key == "stroke") {
            out.stroke = inherit ? parent.stroke : value;
        } else if (key == "fill-rule") {
            out.fill_rule = inherit ? parent.fill_rule : value;
        } else if (key == "clip-rule") {
            out.clip_rule = inherit ? parent.clip_rule : value;
        } else if (key == "stroke-width") {
            out.stroke_width = inherit ? parent.stroke_width : value;
        } else if (key == "stroke-linecap") {
            out.linecap = inherit ? parent.linecap : value;
        } else if (key == "stroke-linejoin") {
            out.linejoin = inherit ? parent.linejoin : value;
        } else if (key == "stroke-dasharray") {
            out.dasharray = inherit ? parent.dasharray : value;
        } else if (key == "stroke-dashoffset") {
            out.dashoffset = inherit ? parent.dashoffset : value;
        } else if (key == "opacity") {
            out.opacity = inherit ? parent.opacity : std::clamp(length(value, 1, 1), 0.0, 1.0);
        } else if (key == "fill-opacity") {
            out.fill_opacity =
                inherit ? parent.fill_opacity : std::clamp(length(value, 1, 1), 0.0, 1.0);
        } else if (key == "stroke-opacity") {
            out.stroke_opacity =
                inherit ? parent.stroke_opacity : std::clamp(length(value, 1, 1), 0.0, 1.0);
        } else if (key == "stroke-miterlimit") {
            out.miterlimit = inherit ? parent.miterlimit : std::max(1.0, number(value, 4));
        } else if (key == "display") {
            out.display = inherit ? parent.display : value != "none";
        } else if (key == "visibility") {
            out.visible = inherit ? parent.visible : value == "visible";
        } else if (key == "clip-path") {
            out.clip = inherit ? parent.clip : value;
        } else if (key == "mask") {
            out.mask = inherit ? parent.mask : value;
        } else if (key == "filter") {
            out.filter = inherit ? parent.filter : value;
        } else if (key == "color-interpolation-filters") {
            out.filter_space = inherit ? parent.filter_space : value;
        } else if (key == "mask-type") {
            out.mask_type = inherit ? parent.mask_type : value;
        } else if (key == "stop-color") {
            out.stop_color = inherit ? parent.stop_color : colour(value, out.color);
        } else if (key == "stop-opacity") {
            out.stop_opacity =
                inherit ? parent.stop_opacity : std::clamp(length(value, 1, 1), 0.0, 1.0);
        }
    }
    return out;
}

Colour colour(std::string_view text, Colour current) {
    text = trim(text);
    if (text == "currentColor") {
        return current;
    }
    std::string lower{text};
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    text = lower;
    if (text == "none" || text == "transparent") {
        return {};
    }
    unsigned rgb = 0;
    static constexpr std::pair<std::string_view, unsigned> named[] = {
        {"aliceblue", 0xf0f8ffU},
        {"antiquewhite", 0xfaebd7U},
        {"aqua", 0x00ffffU},
        {"aquamarine", 0x7fffd4U},
        {"azure", 0xf0ffffU},
        {"beige", 0xf5f5dcU},
        {"bisque", 0xffe4c4U},
        {"black", 0x000000U},
        {"blanchedalmond", 0xffebcdU},
        {"blue", 0x0000ffU},
        {"blueviolet", 0x8a2be2U},
        {"brown", 0xa52a2aU},
        {"burlywood", 0xdeb887U},
        {"cadetblue", 0x5f9ea0U},
        {"chartreuse", 0x7fff00U},
        {"chocolate", 0xd2691eU},
        {"coral", 0xff7f50U},
        {"cornflowerblue", 0x6495edU},
        {"cornsilk", 0xfff8dcU},
        {"crimson", 0xdc143cU},
        {"cyan", 0x00ffffU},
        {"darkblue", 0x00008bU},
        {"darkcyan", 0x008b8bU},
        {"darkgoldenrod", 0xb8860bU},
        {"darkgray", 0xa9a9a9U},
        {"darkgreen", 0x006400U},
        {"darkgrey", 0xa9a9a9U},
        {"darkkhaki", 0xbdb76bU},
        {"darkmagenta", 0x8b008bU},
        {"darkolivegreen", 0x556b2fU},
        {"darkorange", 0xff8c00U},
        {"darkorchid", 0x9932ccU},
        {"darkred", 0x8b0000U},
        {"darksalmon", 0xe9967aU},
        {"darkseagreen", 0x8fbc8fU},
        {"darkslateblue", 0x483d8bU},
        {"darkslategray", 0x2f4f4fU},
        {"darkslategrey", 0x2f4f4fU},
        {"darkturquoise", 0x00ced1U},
        {"darkviolet", 0x9400d3U},
        {"deeppink", 0xff1493U},
        {"deepskyblue", 0x00bfffU},
        {"dimgray", 0x696969U},
        {"dimgrey", 0x696969U},
        {"dodgerblue", 0x1e90ffU},
        {"firebrick", 0xb22222U},
        {"floralwhite", 0xfffaf0U},
        {"forestgreen", 0x228b22U},
        {"fuchsia", 0xff00ffU},
        {"gainsboro", 0xdcdcdcU},
        {"ghostwhite", 0xf8f8ffU},
        {"gold", 0xffd700U},
        {"goldenrod", 0xdaa520U},
        {"gray", 0x808080U},
        {"green", 0x008000U},
        {"greenyellow", 0xadff2fU},
        {"grey", 0x808080U},
        {"honeydew", 0xf0fff0U},
        {"hotpink", 0xff69b4U},
        {"indianred", 0xcd5c5cU},
        {"indigo", 0x4b0082U},
        {"ivory", 0xfffff0U},
        {"khaki", 0xf0e68cU},
        {"lavender", 0xe6e6faU},
        {"lavenderblush", 0xfff0f5U},
        {"lawngreen", 0x7cfc00U},
        {"lemonchiffon", 0xfffacdU},
        {"lightblue", 0xadd8e6U},
        {"lightcoral", 0xf08080U},
        {"lightcyan", 0xe0ffffU},
        {"lightgoldenrodyellow", 0xfafad2U},
        {"lightgray", 0xd3d3d3U},
        {"lightgreen", 0x90ee90U},
        {"lightgrey", 0xd3d3d3U},
        {"lightpink", 0xffb6c1U},
        {"lightsalmon", 0xffa07aU},
        {"lightseagreen", 0x20b2aaU},
        {"lightskyblue", 0x87cefaU},
        {"lightslategray", 0x778899U},
        {"lightslategrey", 0x778899U},
        {"lightsteelblue", 0xb0c4deU},
        {"lightyellow", 0xffffe0U},
        {"lime", 0x00ff00U},
        {"limegreen", 0x32cd32U},
        {"linen", 0xfaf0e6U},
        {"magenta", 0xff00ffU},
        {"maroon", 0x800000U},
        {"mediumaquamarine", 0x66cdaaU},
        {"mediumblue", 0x0000cdU},
        {"mediumorchid", 0xba55d3U},
        {"mediumpurple", 0x9370dbU},
        {"mediumseagreen", 0x3cb371U},
        {"mediumslateblue", 0x7b68eeU},
        {"mediumspringgreen", 0x00fa9aU},
        {"mediumturquoise", 0x48d1ccU},
        {"mediumvioletred", 0xc71585U},
        {"midnightblue", 0x191970U},
        {"mintcream", 0xf5fffaU},
        {"mistyrose", 0xffe4e1U},
        {"moccasin", 0xffe4b5U},
        {"navajowhite", 0xffdeadU},
        {"navy", 0x000080U},
        {"oldlace", 0xfdf5e6U},
        {"olive", 0x808000U},
        {"olivedrab", 0x6b8e23U},
        {"orange", 0xffa500U},
        {"orangered", 0xff4500U},
        {"orchid", 0xda70d6U},
        {"palegoldenrod", 0xeee8aaU},
        {"palegreen", 0x98fb98U},
        {"paleturquoise", 0xafeeeeU},
        {"palevioletred", 0xdb7093U},
        {"papayawhip", 0xffefd5U},
        {"peachpuff", 0xffdab9U},
        {"peru", 0xcd853fU},
        {"pink", 0xffc0cbU},
        {"plum", 0xdda0ddU},
        {"powderblue", 0xb0e0e6U},
        {"purple", 0x800080U},
        {"red", 0xff0000U},
        {"rosybrown", 0xbc8f8fU},
        {"royalblue", 0x4169e1U},
        {"saddlebrown", 0x8b4513U},
        {"salmon", 0xfa8072U},
        {"sandybrown", 0xf4a460U},
        {"seagreen", 0x2e8b57U},
        {"seashell", 0xfff5eeU},
        {"sienna", 0xa0522dU},
        {"silver", 0xc0c0c0U},
        {"skyblue", 0x87ceebU},
        {"slateblue", 0x6a5acdU},
        {"slategray", 0x708090U},
        {"slategrey", 0x708090U},
        {"snow", 0xfffafaU},
        {"springgreen", 0x00ff7fU},
        {"steelblue", 0x4682b4U},
        {"tan", 0xd2b48cU},
        {"teal", 0x008080U},
        {"thistle", 0xd8bfd8U},
        {"tomato", 0xff6347U},
        {"turquoise", 0x40e0d0U},
        {"violet", 0xee82eeU},
        {"wheat", 0xf5deb3U},
        {"white", 0xffffffU},
        {"whitesmoke", 0xf5f5f5U},
        {"yellow", 0xffff00U},
        {"yellowgreen", 0x9acd32U},
    };
    if (text.starts_with('#') && (text.size() == 4 || text.size() == 7)) {
        auto parsed = std::from_chars(text.data() + 1, text.data() + text.size(), rgb, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return {0, 0, 0, 1};
        }
        if (text.size() == 4) {
            rgb = ((rgb & 0xf00U) << 12U) | ((rgb & 0xf00U) << 8U) | ((rgb & 0xf0U) << 8U) |
                  ((rgb & 0xf0U) << 4U) | ((rgb & 0xfU) << 4U) | (rgb & 0xfU);
        }
    } else if ((text.starts_with("rgb(") || text.starts_with("rgba(")) && text.ends_with(')')) {
        const bool alpha = text.starts_with("rgba(");
        auto content = text.substr(alpha ? 5 : 4);
        content.remove_suffix(1);
        Numbers input{content};
        Colour out{0, 0, 0, 1};
        std::optional<bool> percent_rgb;
        for (std::size_t i = 0; i < (alpha ? 4U : 3U); ++i) {
            double v = input.get();
            bool percent = input.at < content.size() && content[input.at] == '%';
            if (i < 3) {
                if (percent_rgb && *percent_rgb != percent) {
                    return {0, 0, 0, 1};
                }
                percent_rgb = percent;
            }
            if (percent) {
                ++input.at;
            }
            out[i] = std::clamp(v / (percent ? 100 : i == 3 ? 1 : 255), 0.0, 1.0);
        }
        require(!input.more());
        return out;
    } else {
        auto it =
            std::lower_bound(std::begin(named), std::end(named), text,
                             [](auto entry, std::string_view name) { return entry.first < name; });
        if (it != std::end(named) && it->first == text) {
            rgb = it->second;
        }
    }
    return {static_cast<double>((rgb >> 16U) & 255U) / 255,
            static_cast<double>((rgb >> 8U) & 255U) / 255, static_cast<double>(rgb & 255U) / 255,
            1};
}
}  // namespace rummage::svg
