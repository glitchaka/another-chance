#include "style/Style.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <sstream>

namespace ac::style {
namespace {
std::string Trim(std::string_view input) {
    const size_t start = input.find_first_not_of(" \r\n\t");
    if (start == std::string_view::npos) return {};
    const size_t end = input.find_last_not_of(" \r\n\t");
    return std::string(input.substr(start, end - start + 1));
}
std::string Lower(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
bool IsBlock(std::string_view tag) {
    return tag == "html" || tag == "body" || tag == "main" || tag == "article" ||
        tag == "section" || tag == "header" || tag == "footer" || tag == "nav" ||
        tag == "div" || tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6" || tag == "ul" || tag == "ol" ||
        tag == "li" || tag == "blockquote" || tag == "pre" || tag == "table" ||
        tag == "tr" || tag == "form" || tag == "figure" || tag == "figcaption" ||
        tag == "address" || tag == "dl" || tag == "dt" || tag == "dd";
}
COLORREF ParseColor(std::string_view input, COLORREF fallback) {
    std::string value = Lower(Trim(input));
    if (value.size() == 7 && value[0] == '#') {
        char* end = nullptr;
        const auto result = std::strtoul(value.c_str() + 1, &end, 16);
        if (end && *end == '\0') {
            return RGB((result >> 16) & 255, (result >> 8) & 255, result & 255);
        }
    }
    if (value.size() == 4 && value[0] == '#') {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int r = nibble(value[1]), g = nibble(value[2]), b = nibble(value[3]);
        if (r >= 0 && g >= 0 && b >= 0) return RGB(r * 17, g * 17, b * 17);
    }
    if (value.starts_with("rgb(") || value.starts_with("rgba(")) {
        auto open = value.find('(');
        auto end = value.find(')', open);
        if (end != std::string::npos) {
            std::string numbers = value.substr(open + 1, end - open - 1);
            std::replace(numbers.begin(), numbers.end(), ',', ' ');
            std::istringstream stream(numbers);
            int r = 0, g = 0, b = 0;
            if (stream >> r >> g >> b) {
                return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255),
                           std::clamp(b, 0, 255));
            }
        }
    }
    if (value == "black") return RGB(0,0,0);
    if (value == "white") return RGB(255,255,255);
    if (value == "red") return RGB(215,35,35);
    if (value == "green") return RGB(0,128,0);
    if (value == "blue") return RGB(0,0,255);
    if (value == "navy") return RGB(0,0,128);
    if (value == "gray" || value == "grey") return RGB(128,128,128);
    if (value == "silver") return RGB(192,192,192);
    if (value == "purple") return RGB(128,0,128);
    if (value == "orange") return RGB(255,165,0);
    if (value == "teal") return RGB(0,128,128);
    if (value == "transparent") return CLR_INVALID;
    return fallback;
}
std::vector<Sheet::Declaration> ParseDeclarations(std::string_view declarations) {
    std::vector<Sheet::Declaration> output;
    size_t i = 0;
    while (i < declarations.size()) {
        const size_t colon = declarations.find(':', i);
        if (colon == std::string_view::npos) break;
        const size_t semicolon = declarations.find(';', colon + 1);
        const size_t end = semicolon == std::string_view::npos ? declarations.size() : semicolon;
        const std::string key = Lower(Trim(declarations.substr(i, colon - i)));
        const std::string value = Lower(Trim(declarations.substr(colon + 1, end - colon - 1)));
        if (!key.empty() && !value.empty()) output.emplace_back(key, value);
        i = end == declarations.size() ? end : end + 1;
    }
    return output;
}
} // namespace

void Sheet::Parse(std::string_view css) {
    rules_.clear();
    std::string cleaned(css);
    size_t comment = 0;
    while ((comment = cleaned.find("/*", comment)) != std::string::npos) {
        const size_t end = cleaned.find("*/", comment + 2);
        if (end == std::string::npos) { cleaned.erase(comment); break; }
        cleaned.erase(comment, end - comment + 2);
    }
    size_t i = 0;
    while (i < cleaned.size() && rules_.size() < 5000) {
        const auto opening = cleaned.find('{', i);
        if (opening == std::string::npos) break;
        const auto closing = cleaned.find('}', opening + 1);
        if (closing == std::string::npos) break;
        const auto decls = ParseDeclarations(
            std::string_view(cleaned).substr(opening + 1, closing - opening - 1));
        std::string selectors = cleaned.substr(i, opening - i);
        size_t s = 0;
        while (s < selectors.size()) {
            const size_t comma = selectors.find(',', s);
            const size_t end = comma == std::string::npos ? selectors.size() : comma;
            auto selector = Lower(Trim(std::string_view(selectors).substr(s, end - s)));
            if (!selector.empty() && selector[0] != '@') {
                rules_.push_back({std::move(selector), decls});
            }
            if (comma == std::string::npos) break;
            s = comma + 1;
        }
        i = closing + 1;
    }
}

void Sheet::Apply(Computed& style, std::string_view rawKey,
                  std::string_view rawValue) {
    const std::string key = Lower(Trim(rawKey));
    const std::string value = Lower(Trim(rawValue));
    if (key == "color") style.color = ParseColor(value, style.color);
    else if (key == "background" || key == "background-color") {
        if (value == "transparent") style.background = CLR_INVALID;
        else style.background = ParseColor(value, style.background);
    } else if (key == "font-size") {
        char* end = nullptr;
        const float amount = std::strtof(value.c_str(), &end);
        if (end != value.c_str() && amount > 0) {
            int size = style.fontSize;
            const std::string suffix(end);
            if (suffix.starts_with("px")) size = static_cast<int>(amount);
            else if (suffix.starts_with("em") || suffix.starts_with("rem"))
                size = static_cast<int>(amount * style.fontSize);
            else if (suffix.starts_with("%"))
                size = static_cast<int>(amount * style.fontSize / 100);
            style.fontSize = std::clamp(size, 8, 72);
        }
    } else if (key == "font-weight") {
        style.bold = value == "bold" || value == "bolder" ||
                     (!value.empty() && value[0] >= '6' && value[0] <= '9');
        if (value == "normal" || value == "400") style.bold = false;
    } else if (key == "font-style") {
        style.italic = value == "italic" || value == "oblique";
    } else if (key == "text-decoration" || key == "text-decoration-line") {
        style.underline = value.find("underline") != std::string::npos;
    } else if (key == "display") {
        if (value == "none") style.hidden = true;
        else if (value == "block" || value == "flex" || value == "grid") style.block = true;
        else if (value == "inline") style.block = false;
    } else if (key == "margin-top" || key == "margin-bottom") {
        char* end = nullptr;
        const long amount = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str()) {
            const int margin = static_cast<int>(std::clamp(amount, 0L, 120L));
            if (key == "margin-top") style.marginTop = margin;
            else style.marginBottom = margin;
        }
    }
}

bool Sheet::Matches(const html::Node& node, std::string_view fullSelector) {
    if (node.type != html::Node::Type::Element) return false;
    // Level 1: simple selectors only. Child/descendant selectors not yet supported.
    std::string selector = Trim(fullSelector);
    if (selector.find_first_of(" >+~[:") != std::string::npos) return false;
    if (selector == "*") return true;
    size_t pos = 0;
    while (pos < selector.size() && selector[pos] != '.' && selector[pos] != '#') ++pos;
    if (pos && node.tag != selector.substr(0, pos)) return false;
    while (pos < selector.size()) {
        const char sigil = selector[pos++];
        const size_t start = pos;
        while (pos < selector.size() && selector[pos] != '.' && selector[pos] != '#') ++pos;
        if (start == pos) return false;
        const std::string value = selector.substr(start, pos - start);
        if (sigil == '#') {
            if (node.Attribute("id") != value) return false;
        } else if (sigil == '.') {
            const std::string classes = " " + node.Attribute("class") + " ";
            if (classes.find(" " + value + " ") == std::string::npos) return false;
        }
    }
    return true;
}

Computed Sheet::Compute(const html::Node& node, const Computed& parent) const {
    Computed result = parent;
    result.block = false;
    result.marginTop = 0;
    result.marginBottom = 0;
    result.background = CLR_INVALID;
    if (node.type != html::Node::Type::Element) return result;
    const auto& tag = node.tag;
    result.block = IsBlock(tag);
    if (tag == "document") result.block = false;
    if (tag == "head" || tag == "title" || tag == "script" ||
        tag == "style" || tag == "meta" || tag == "link" ||
        tag == "noscript" || tag == "svg" || tag == "canvas" ||
        tag == "template") result.hidden = true;
    if (tag == "a") {
        result.color = RGB(20, 87, 180);
        result.underline = true;
    }
    if (tag == "b" || tag == "strong" || tag == "th" || tag == "dt") result.bold = true;
    if (tag == "em" || tag == "i" || tag == "cite") result.italic = true;
    if (tag == "h1") { result.fontSize = 32; result.bold = true; result.marginTop = 14; result.marginBottom = 12; }
    else if (tag == "h2") { result.fontSize = 27; result.bold = true; result.marginTop = 12; result.marginBottom = 9; }
    else if (tag == "h3") { result.fontSize = 22; result.bold = true; result.marginTop = 10; result.marginBottom = 8; }
    else if (tag == "h4" || tag == "h5" || tag == "h6") {
        result.fontSize = 18; result.bold = true; result.marginTop = 8; result.marginBottom = 6;
    } else if (tag == "p") { result.marginTop = 5; result.marginBottom = 11; }
    else if (tag == "li") { result.marginTop = 2; result.marginBottom = 3; }
    else if (tag == "ul" || tag == "ol") { result.marginTop = 7; result.marginBottom = 9; }
    else if (tag == "blockquote") { result.italic = true; result.marginTop = 10; result.marginBottom = 10; }
    else if (tag == "small") result.fontSize = std::max(9, result.fontSize - 2);
    else if (tag == "code" || tag == "pre") result.background = RGB(246,247,249);

    for (const auto& rule : rules_) {
        if (Matches(node, rule.selector)) {
            for (const auto& [key,value] : rule.declarations) Apply(result, key, value);
        }
    }
    const auto inlineCss = node.Attribute("style");
    if (!inlineCss.empty()) {
        for (const auto& [key,value] : ParseDeclarations(inlineCss)) {
            Apply(result, key, value);
        }
    }
    return result;
}
} // namespace ac::style
