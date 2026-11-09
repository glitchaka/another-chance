#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstddef>

namespace ac::chrome {

constexpr int kTabStripHeight = 45;
constexpr int kToolbarHeight = 105;
constexpr int kStatusHeight = 25;

struct Tab {
    std::wstring title;
    bool loading = false;
};

enum class Action {
    None, Back, Forward, Reload, Home,
    Bookmark, Menu, AddTab, SelectTab, CloseTab
};

struct Hit {
    Action action = Action::None;
    size_t tab = 0;
};

struct Geometry {
    RECT address{};
    RECT bookmark{};
    RECT menu{};
    RECT plus{};
    int tabWidth = 210;
};

[[nodiscard]] Geometry Measure(int width, size_t tabCount);
[[nodiscard]] Hit HitTest(int x, int y, int width, size_t tabCount);
void Draw(HDC dc, int width, int height, const std::vector<Tab>& tabs,
          size_t activeTab, bool canBack, bool canForward,
          bool bookmarked, bool loading);

} // namespace ac::chrome
