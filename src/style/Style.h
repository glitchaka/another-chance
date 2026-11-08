#pragma once
#include "html/HtmlParser.h"
#include <windows.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac::style {
struct Computed {
    COLORREF color = RGB(35, 40, 46);
    COLORREF background = CLR_INVALID;
    int fontSize = 16;
    int marginTop = 0;
    int marginBottom = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool block = false;
    bool hidden = false;
};

class Sheet {
public:
    void Parse(std::string_view css);
    [[nodiscard]] Computed Compute(const html::Node& node,
                                   const Computed& parent) const;
private:
    using Declaration = std::pair<std::string, std::string>;
    struct Rule {
        std::string selector;
        std::vector<Declaration> declarations;
    };
    std::vector<Rule> rules_;
    static void Apply(Computed& computed, std::string_view key,
                      std::string_view value);
    static bool Matches(const html::Node& node, std::string_view selector);
};
}
