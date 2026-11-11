#pragma once
#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace ac::chrome {

// Two independently laid-out rows, not a tall single toolbar.
// The top row holds actual tabs and native caption controls. The second row
// holds navigation, address input and the page actions.
constexpr int kTabRowHeight = 34;
constexpr int kToolbarHeight = 76;
// Status is retained as an internal accessibility/test value, not a
// permanent developer-style footer in the browsing surface.
constexpr int kStatusHeight = 0;
constexpr WORD kTabCommandBase = 300;

struct Tab {
    std::wstring title;
    bool loading = false;
};

enum class Action {
    None, Back, Forward, Reload, Home, Bookmark, NewTab,
    TabSelect, TabClose, TabMenu, Menu, Minimize, Maximize, Close
};
struct Hit {
    Action action = Action::None;
    size_t index = 0;
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
[[nodiscard]] RECT TabBounds(int width, size_t count, size_t index);
[[nodiscard]] RECT NewTabBounds(int width, size_t count);
[[nodiscard]] Hit HitTest(int x, int y, int width, size_t tabCount);
[[nodiscard]] bool IsDraggable(int x, int y, int width, size_t tabCount);
void Draw(HDC dc, int width, int height, const std::vector<Tab>& tabs,
          size_t activeTab, bool canBack, bool canForward,
          bool bookmarked, bool loading, bool maximized);
} // namespace ac::chrome
