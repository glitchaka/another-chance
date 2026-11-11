#pragma once
#include "html/HtmlParser.h"
#include <windows.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac::style {
enum class TextAlign { Left, Center, Right };
enum class LayoutMode { Flow, FlexRow, FlexColumn, Grid };
struct Computed {
    COLORREF color = RGB(35, 40, 46);
    COLORREF background = CLR_INVALID;
    int fontSize = 16;
    TextAlign textAlign = TextAlign::Left;
    int marginTop = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    int marginRight = 0;
    int paddingTop = 0;
    int paddingBottom = 0;
    int paddingLeft = 0;
    int paddingRight = 0;
    int widthPixels = -1;
    int widthPercent = -1;
    int maxWidthPixels = -1;
    int minHeightPixels = 0;
    int gapPixels = 0;
    int gridColumns = 2;
    LayoutMode layoutMode = LayoutMode::Flow;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool block = false;
    bool hidden = false;
};

class Sheet {
public:
    using Declaration = std::pair<std::string, std::string>;
    void Parse(std::string_view css, int viewportWidth = 1024);
    [[nodiscard]] Computed Compute(const html::Node& node,
                                   const Computed& parent) const;
private:
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
