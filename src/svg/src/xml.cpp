#include <charconv>
#include <cstdint>

#include "internal.hpp"

namespace rummage::svg {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
           static_cast<unsigned char>(c) >= 128;
}

bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

std::string local(std::string_view name) {
    const auto colon = name.rfind(':');
    return std::string{name.substr(colon == std::string_view::npos ? 0 : colon + 1)};
}

void utf8(std::string& out, std::uint32_t cp) {
    require(cp == 9 || cp == 10 || cp == 13 ||
            (cp >= 32 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) && cp != 0xfffe &&
             cp != 0xffff));
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0U | (cp >> 6U));
        out += static_cast<char>(0x80U | (cp & 63U));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0U | (cp >> 12U));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 63U));
        out += static_cast<char>(0x80U | (cp & 63U));
    } else {
        out += static_cast<char>(0xf0U | (cp >> 18U));
        out += static_cast<char>(0x80U | ((cp >> 12U) & 63U));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 63U));
        out += static_cast<char>(0x80U | (cp & 63U));
    }
}

std::string entities(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            require(input[i] != 0);
            result += input[i];
            continue;
        }
        const auto end = input.find(';', i + 1);
        require(end != std::string_view::npos);
        auto entity = input.substr(i + 1, end - i - 1);
        if (entity == "amp") {
            result += '&';
        } else if (entity == "lt") {
            result += '<';
        } else if (entity == "gt") {
            result += '>';
        } else if (entity == "quot") {
            result += '"';
        } else if (entity == "apos") {
            result += '\'';
        } else {
            require(entity.starts_with('#'));
            entity.remove_prefix(1);
            const int base = entity.starts_with('x') ? 16 : 10;
            if (base == 16) {
                entity.remove_prefix(1);
            }
            std::uint32_t cp{};
            const auto parsed =
                std::from_chars(entity.data(), entity.data() + entity.size(), cp, base);
            require(parsed.ec == std::errc{} && parsed.ptr == entity.data() + entity.size());
            utf8(result, cp);
        }
        i = end;
    }
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::vector<Node> parse() {
        if (input_.starts_with("\xef\xbb\xbf")) {
            pos_ = 3;
        }
        while (pos_ < input_.size()) {
            if (input_[pos_] != '<') {
                const auto end = input_.find('<', pos_);
                const auto count = (end == std::string_view::npos ? input_.size() : end) - pos_;
                auto text = entities(input_.substr(pos_, count));
                if (stack_.empty()) {
                    for (char c : text) {
                        require(space(c));
                    }
                } else {
                    nodes_[stack_.back()].text += text;
                }
                pos_ += count;
            } else if (take("<!--")) {
                skip("-->");
            } else if (take("<?")) {
                skip("?>");
            } else if (take("<![CDATA[")) {
                require(!stack_.empty());
                const auto end = input_.find("]]>", pos_);
                require(end != std::string_view::npos);
                nodes_[stack_.back()].text += input_.substr(pos_, end - pos_);
                pos_ = end + 3;
            } else if (take("<!DOCTYPE")) {
                require(nodes_.empty());
                int brackets = 0;
                char quote = 0;
                bool done = false;
                while (pos_ < input_.size()) {
                    const char c = input_[pos_++];
                    if (quote) {
                        if (c == quote) {
                            quote = 0;
                        }
                    } else if (c == '\'' || c == '"') {
                        quote = c;
                    } else if (c == '[') {
                        ++brackets;
                    } else if (c == ']') {
                        require(brackets > 0);
                        --brackets;
                    } else if (c == '>' && brackets == 0) {
                        done = true;
                        break;
                    }
                }
                require(done);
            } else if (take("</")) {
                require(!stack_.empty());
                const auto closing = name();
                require(closing == names_.back());
                whitespace();
                require(take(">"));
                stack_.pop_back();
                names_.pop_back();
            } else {
                require(take("<"));
                require(nodes_.size() < kMaxNodes && stack_.size() < kMaxDepth);
                require(!stack_.empty() || nodes_.empty());
                const auto full_name = name();
                const std::size_t index = nodes_.size();
                nodes_.push_back(
                    Node{.name = local(full_name), .attributes = {}, .children = {}, .text = {}});
                if (!stack_.empty()) {
                    nodes_[index].parent = stack_.back();
                    nodes_[stack_.back()].children.push_back(index);
                }
                std::set<std::string_view> attribute_names;
                for (;;) {
                    const bool spaced = whitespace();
                    if (take("/>")) {
                        break;
                    }
                    if (take(">")) {
                        stack_.push_back(index);
                        names_.push_back(full_name);
                        break;
                    }
                    require(spaced);
                    const auto attribute_name = name();
                    require(attribute_names.insert(attribute_name).second);
                    const auto key = local(attribute_name);
                    whitespace();
                    require(take("="));
                    whitespace();
                    require(pos_ < input_.size());
                    const char quote = input_[pos_++];
                    require(quote == '\'' || quote == '"');
                    const auto end = input_.find(quote, pos_);
                    require(end != std::string_view::npos);
                    const auto value = input_.substr(pos_, end - pos_);
                    require(value.find('<') == std::string_view::npos);
                    // xmlns prefixes are declarations, not SVG properties.
                    if (key != "xmlns") {
                        nodes_[index].attributes.insert_or_assign(key, entities(value));
                    }
                    pos_ = end + 1;
                }
            }
        }
        require(stack_.empty() && !nodes_.empty() && nodes_[0].name == "svg");
        return std::move(nodes_);
    }

private:
    bool take(std::string_view text) {
        if (!input_.substr(pos_).starts_with(text)) {
            return false;
        }
        pos_ += text.size();
        return true;
    }

    void skip(std::string_view terminator) {
        const auto end = input_.find(terminator, pos_);
        require(end != std::string_view::npos);
        pos_ = end + terminator.size();
    }

    bool whitespace() {
        const auto start = pos_;
        while (pos_ < input_.size() && space(input_[pos_])) {
            ++pos_;
        }
        return pos_ != start;
    }

    std::string_view name() {
        require(pos_ < input_.size() && name_start(input_[pos_]));
        const auto start = pos_++;
        while (pos_ < input_.size() && name_char(input_[pos_])) {
            ++pos_;
        }
        return input_.substr(start, pos_ - start);
    }

    std::string_view input_;
    std::size_t pos_{};
    std::vector<Node> nodes_;
    std::vector<std::size_t> stack_;
    std::vector<std::string_view> names_;
};
}  // namespace

std::vector<Node> xml(std::string_view input) { return Parser{input}.parse(); }
}  // namespace rummage::svg
