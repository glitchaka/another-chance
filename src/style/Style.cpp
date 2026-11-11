#include "style/Style.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
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
// Compact CSS length parser for the box model. This was previously referenced
// by the style engine without being defined, breaking all Windows builds.
int CssPixels(std::string_view input, int fallback) {
    const std::string text = Lower(Trim(input));
    if (text == "auto") return -1;
    if (text.empty()) return fallback;
    char* tail = nullptr;
    const float value = std::strtof(text.c_str(), &tail);
    if (tail == text.c_str() || !std::isfinite(value)) return fallback;
    const std::string_view unit(tail);
    float pixels = value;
    if (unit == "px" || unit.empty()) {
        // CSS numeric zero is valid without a unit.
        if (unit.empty() && value != 0.0f) return fallback;
    } else if (unit == "rem" || unit == "em") {
        pixels *= 16.0f;
    } else if (unit == "pt") {
        pixels *= 96.0f / 72.0f;
    } else if (unit == "pc") {
        pixels *= 16.0f;
    } else {
        return fallback;
    }
    return static_cast<int>(std::lround(std::clamp(pixels, -4096.0f, 8192.0f)));
}
void CssEdges(std::string_view shorthand, int& top, int& right,
              int& bottom, int& left) {
    std::istringstream input(std::string(shorthand));
    std::vector<int> values;
    std::string token;
    while (input >> token && values.size() < 4) {
        const int parsed = CssPixels(token, -8192);
        if (parsed == -8192) return;
        values.push_back(parsed);
    }
    if (values.empty()) return;
    top = values[0];
    right = values.size() > 1 ? values[1] : top;
    bottom = values.size() > 2 ? values[2] : top;
    left = values.size() > 3 ? values[3] : right;
}
bool IsBlock(std::string_view tag) {
    return tag == "html" || tag == "body" || tag == "center" ||
        tag == "main" || tag == "article" ||
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

void Sheet::Parse(std::string_view css, int viewportWidth) {
    rules_.clear();
    std::string cleaned(css);
    size_t comment = 0;
    while ((comment = cleaned.find("/*", comment)) != std::string::npos) {
        const size_t end = cleaned.find("*/", comment + 2);
        if (end == std::string::npos) { cleaned.erase(comment); break; }
        cleaned.erase(comment, end - comment + 2);
    }

    // Observe basic responsive breakpoints. Layout is recomputed on resize.
    auto mediaMatches = [viewportWidth](const std::string& header) {
        const std::string query = Lower(header);
        if (query.find("print") != std::string::npos &&
            query.find("screen") == std::string::npos) return false;
        if (query.find("prefers-color-scheme: dark") != std::string::npos ||
            query.find("prefers-color-scheme:dark") != std::string::npos)
            return false;
        auto checkBound = [&](std::string_view property, bool minimum) {
            size_t pos = 0;
            while ((pos = query.find(property, pos)) != std::string::npos) {
                pos += property.size();
                while (pos < query.size() &&
                       std::isspace(static_cast<unsigned char>(query[pos]))) ++pos;
                if (pos >= query.size() || query[pos] != ':') continue;
                ++pos;
                while (pos < query.size() &&
                       std::isspace(static_cast<unsigned char>(query[pos]))) ++pos;
                if (pos >= query.size() || !std::isdigit(
                        static_cast<unsigned char>(query[pos]))) continue;
                const int bound = std::atoi(query.c_str() + pos);
                if (minimum && viewportWidth < bound) return false;
                if (!minimum && viewportWidth > bound) return false;
            }
            return true;
        };
        return checkBound("min-width", true) && checkBound("max-width", false);
    };

    std::function<void(size_t, size_t)> visit;
    visit = [&](size_t start, size_t limit) {
        size_t pos = start;
        while (pos < limit && rules_.size() < 5000) {
            const size_t opening = cleaned.find('{', pos);
            if (opening == std::string::npos || opening >= limit) break;
            int depth = 1;
            size_t close = opening + 1;
            while (close < limit && depth > 0) {
                if (cleaned[close] == '{') ++depth;
                else if (cleaned[close] == '}') --depth;
                ++close;
            }
            if (depth != 0) break;
            const size_t braceEnd = close - 1;
            const std::string heading =
                Lower(Trim(std::string_view(cleaned).substr(pos, opening - pos)));
            if (!heading.empty() && heading[0] == '@') {
                if ((heading.starts_with("@media") && mediaMatches(heading)) ||
                    heading.starts_with("@layer")) {
                    visit(opening + 1, braceEnd);
                }
                // Skip @font-face, @keyframes, unsupported feature queries.
            } else {
                const auto declarations = ParseDeclarations(
                    std::string_view(cleaned).substr(opening + 1,
                                                    braceEnd - opening - 1));
                size_t selectorBegin = 0;
                while (selectorBegin < heading.size() && rules_.size() < 5000) {
                    const size_t comma = heading.find(',', selectorBegin);
                    const size_t selectorEnd =
                        comma == std::string::npos ? heading.size() : comma;
                    std::string selector =
                        Trim(std::string_view(heading).substr(selectorBegin,
                                      selectorEnd - selectorBegin));
                    if (!selector.empty()) rules_.push_back(
                        {std::move(selector), declarations});
                    if (comma == std::string::npos) break;
                    selectorBegin = comma + 1;
                }
            }
            pos = close;
        }
    };
    visit(0, cleaned.size());
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
        else if (value == "block") { style.block = true; style.layoutMode = LayoutMode::Flow; }
        else if (value == "flex" || value == "inline-flex") {
            style.block = true; style.layoutMode = LayoutMode::FlexRow;
        } else if (value == "grid" || value == "inline-grid") {
            style.block = true; style.layoutMode = LayoutMode::Grid;
        }
        else if (value == "inline") style.block = false;
    } else if (key == "flex-direction") {
        if (value == "column" || value == "column-reverse")
            style.layoutMode = LayoutMode::FlexColumn;
        else if (value == "row" || value == "row-reverse")
            style.layoutMode = LayoutMode::FlexRow;
    } else if (key == "gap" || key == "column-gap" || key == "row-gap") {
        style.gapPixels = CssPixels(value, 0);
    } else if (key == "grid-template-columns") {
        // Minimal equal-track grid support, including repeat(N, 1fr).
        int columns = 0;
        if (value.starts_with("repeat(")) {
            const char* start = value.c_str() + 7;
            columns = std::atoi(start);
        } else {
            std::istringstream stream(value);
            std::string track;
            while (stream >> track) ++columns;
        }
        style.gridColumns = std::clamp(columns, 1, 12);
    } else if (key == "visibility") {
        if (value == "hidden" || value == "collapse") style.hidden = true;
    } else if (key == "text-align") {
        if (value == "left" || value == "start") style.textAlign = TextAlign::Left;
        else if (value == "center") style.textAlign = TextAlign::Center;
        else if (value == "right" || value == "end") style.textAlign = TextAlign::Right;
    } else if (key == "width") {
        if (value == "auto") { style.widthPixels = -1; style.widthPercent = -1; }
        else if (value.ends_with("%")) {
            char* tail = nullptr;
            const float number = std::strtof(value.c_str(), &tail);
            if (tail != value.c_str() && *tail == '%') {
                style.widthPercent = std::clamp(static_cast<int>(number), 0, 100);
                style.widthPixels = -1;
            }
        } else {
            style.widthPixels = CssPixels(value, style.widthPixels);
            style.widthPercent = -1;
        }
    } else if (key == "max-width") {
        style.maxWidthPixels = value == "none" ? -1 : CssPixels(value, -1);
    } else if (key == "min-height") {
        style.minHeightPixels = CssPixels(value, 0);
    } else if (key == "padding") {
        CssEdges(value, style.paddingTop, style.paddingRight,
                 style.paddingBottom, style.paddingLeft);
    } else if (key == "padding-top") {
        style.paddingTop = CssPixels(value, style.paddingTop);
    } else if (key == "padding-right") {
        style.paddingRight = CssPixels(value, style.paddingRight);
    } else if (key == "padding-bottom") {
        style.paddingBottom = CssPixels(value, style.paddingBottom);
    } else if (key == "padding-left") {
        style.paddingLeft = CssPixels(value, style.paddingLeft);
    } else if (key == "margin") {
        CssEdges(value, style.marginTop, style.marginRight,
                 style.marginBottom, style.marginLeft);
        style.marginTop = std::max(0, style.marginTop);
        style.marginBottom = std::max(0, style.marginBottom);
    } else if (key == "margin-left") {
        style.marginLeft = value == "auto" ? -1 : CssPixels(value, 0);
    } else if (key == "margin-right") {
        style.marginRight = value == "auto" ? -1 : CssPixels(value, 0);
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

namespace {
// Limited, original CSS selector implementation: tag, class, id,
// descendant and direct-child combinators (not pseudoclasses).
bool MatchesSimple(const html::Node& node, std::string_view selector) {
    if (node.type != html::Node::Type::Element) return false;
    if (selector.empty() || selector.find_first_of("[:+~") != std::string_view::npos)
        return false;
    size_t pos = 0;
    if (selector.front() == '*') {
        ++pos;
    } else {
        while (pos < selector.size() && selector[pos] != '.' &&
               selector[pos] != '#') ++pos;
        if (pos && node.tag != selector.substr(0, pos)) return false;
    }
    while (pos < selector.size()) {
        const char sigil = selector[pos++];
        if (sigil != '.' && sigil != '#') return false;
        const size_t start = pos;
        while (pos < selector.size() && selector[pos] != '.' &&
               selector[pos] != '#') ++pos;
        if (pos == start) return false;
        const std::string value(selector.substr(start, pos - start));
        if (sigil == '#') {
            if (node.Attribute("id") != value) return false;
        } else {
            std::istringstream tokens(node.Attribute("class"));
            std::string token;
            bool found = false;
            while (tokens >> token) {
                if (token == value) { found = true; break; }
            }
            if (!found) return false;
        }
    }
    return true;
}

struct SelectorParts {
    std::vector<std::string> simple;
    std::vector<char> relation;
};
SelectorParts SplitSelector(std::string_view value) {
    SelectorParts parts;
    size_t i = 0;
    char relation = ' ';
    while (i < value.size()) {
        if (std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i; continue;
        }
        if (value[i] == '>') {
            relation = '>';
            ++i; continue;
        }
        const size_t start = i;
        while (i < value.size() &&
               value[i] != '>' &&
               !std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (start == i) { ++i; continue; }
        if (!parts.simple.empty()) parts.relation.push_back(relation);
        parts.simple.emplace_back(value.substr(start, i - start));
        relation = ' ';
    }
    return parts;
}
int Specificity(std::string_view selector) {
    const auto parts = SplitSelector(selector);
    int score = 0;
    for (const auto& item : parts.simple) {
        if (!item.empty() && item.front() != '*' &&
            item.front() != '.' && item.front() != '#') ++score;
        for (char c : item) {
            if (c == '.') score += 10;
            if (c == '#') score += 100;
        }
    }
    return score;
}
} // namespace

bool Sheet::Matches(const html::Node& node, std::string_view fullSelector) {
    const auto selector = SplitSelector(fullSelector);
    if (selector.simple.empty()) return false;
    const html::Node* current = &node;
    size_t i = selector.simple.size() - 1;
    if (!MatchesSimple(*current, selector.simple[i])) return false;
    while (i > 0) {
        const char relation = selector.relation[i - 1];
        current = current->parent;
        if (relation == '>') {
            if (!current || !MatchesSimple(*current, selector.simple[i - 1]))
                return false;
        } else {
            while (current && !MatchesSimple(*current, selector.simple[i - 1]))
                current = current->parent;
            if (!current) return false;
        }
        --i;
    }
    return true;
}

Computed Sheet::Compute(const html::Node& node, const Computed& parent) const {
    Computed result = parent;
    result.block = false;
    result.marginTop = 0;
    result.marginBottom = 0;
    result.marginLeft = 0;
    result.marginRight = 0;
    result.paddingTop = result.paddingRight = 0;
    result.paddingBottom = result.paddingLeft = 0;
    result.widthPixels = -1;
    result.widthPercent = -1;
    result.maxWidthPixels = -1;
    result.minHeightPixels = 0;
    result.gapPixels = 0;
    result.gridColumns = 2;
    result.layoutMode = LayoutMode::Flow;
    result.background = CLR_INVALID;
    if (node.type != html::Node::Type::Element) return result;
    const auto& tag = node.tag;
    result.block = IsBlock(tag);
    if (tag == "document") result.block = false;
    if (tag == "center") result.textAlign = TextAlign::Center;
    if (tag == "div" || tag == "p" || tag == "h1" || tag == "h2" || tag == "h3") {
        const auto htmlAlign = Lower(node.Attribute("align"));
        if (htmlAlign == "center") result.textAlign = TextAlign::Center;
        else if (htmlAlign == "right") result.textAlign = TextAlign::Right;
        else if (htmlAlign == "left") result.textAlign = TextAlign::Left;
    }
    if (tag == "head" || tag == "title" || tag == "script" ||
        tag == "style" || tag == "meta" || tag == "link" ||
        tag == "svg" || tag == "canvas" ||
        tag == "template" || tag == "base" || tag == "source")
        result.hidden = true;
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

    // Apply by CSS specificity first, preserving source order on ties.
    // This remains a subset: no !important, @media, pseudo-classes, or
    // full CSS cascade layers.
    struct MatchedRule { int score; size_t order; const Rule* rule; };
    std::vector<MatchedRule> matched;
    for (size_t i = 0; i < rules_.size(); ++i) {
        if (Matches(node, rules_[i].selector)) {
            matched.push_back({Specificity(rules_[i].selector), i, &rules_[i]});
        }
    }
    std::stable_sort(matched.begin(), matched.end(),
        [](const MatchedRule& a, const MatchedRule& b) {
            return a.score < b.score;
        });
    for (const auto& match : matched) {
        for (const auto& [key,value] : match.rule->declarations) {
            Apply(result, key, value);
        }
    }
    const auto inlineCss = node.Attribute("style");
    if (!inlineCss.empty()) {
        for (const auto& [key,value] : ParseDeclarations(inlineCss)) {
            Apply(result, key, value);
        }
    }
    // Boolean HTML "hidden" is a presence attribute. Hidden inputs must
    // not leak values or tokens into rendered text even if CSS overrides display.
    if (node.attributes.contains("hidden") ||
        (tag == "input" && Lower(node.Attribute("type")) == "hidden")) {
        result.hidden = true;
    }
    return result;
}
} // namespace ac::style
