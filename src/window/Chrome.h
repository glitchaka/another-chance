#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstddef>

namespace ac::chrome {

// One integrated workspace header: no Chromium-style tab strip or system caption.
constexpr int kToolbarHeight = 56;
constexpr int kStatusHeight = 23;
constexpr WORD kTabCommandBase = 300;

struct Tab {
    std::wstring title;
    bool loading = false;
};

enum class Action {
    None, Back, Forward, Reload, Home, Bookmark,
    TabMenu, Menu, Minimize, Maximize, Close
};
struct Hit {
    Action action = Action::None;
};

struct Geometry {
    RECT address{};
    RECT back{};
    RECT forward{};
    RECT reload{};
    RECT home{};
    RECT bookmark{};
    RECT tabs{};
    RECT menu{};
    RECT minimize{};
    RECT maximize{};
    RECT close{};
};

[[nodiscard]] Geometry Measure(int width);
[[nodiscard]] Hit HitTest(int x, int y, int width);
[[nodiscard]] bool IsDraggable(int x, int y, int width);
void Draw(HDC dc, int width, int height, const std::vector<Tab>& tabs,
          size_t activeTab, bool canBack, bool canForward,
          bool bookmarked, bool loading, bool maximized);
} // namespace ac::chrome
