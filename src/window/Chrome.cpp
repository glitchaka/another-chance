#include "window/Chrome.h"
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace ac::chrome {
namespace {
using namespace Gdiplus;

constexpr Color kTop(255, 226, 233, 244);
constexpr Color kInactiveTab(255, 230, 235, 244);
constexpr Color kSurface(255, 248, 250, 254);
constexpr Color kText(255, 35, 48, 68);
constexpr Color kMuted(255, 102, 116, 138);
constexpr Color kBorder(255, 221, 227, 237);
constexpr Color kAccent(255, 45, 120, 218);

bool Contains(RECT r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
RECT Rect(int x, int y, int width, int height) {
    return {x, y, x + width, y + height};
}
void Rounded(Graphics& g, RectF r, float radius, const Color& fill,
             const Color* border = nullptr) {
    GraphicsPath path;
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    if (border) {
        Pen edge(*border, 1);
        g.DrawPath(&edge, &path);
    }
}
void Stroke(Graphics& g, const Color& color, float size,
            PointF a, PointF b) {
    Pen pen(color, size);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, a, b);
}
void Label(Graphics& g, const std::wstring& label, float x, float y,
           float width, float height, float size,
           const Color& color, bool strong = false) {
    FontFamily family(L"Segoe UI");
    Font font(&family, size, strong ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label.c_str(), -1, &font,
                 RectF(x, y, std::max(1.0f, width), height), &format, &brush);
}
void Arrow(Graphics& g, int x, int y, bool left, const Color& color) {
    const float cx = static_cast<float>(x);
    const float cy = static_cast<float>(y);
    const float direction = left ? -1.0f : 1.0f;
    Stroke(g, color, 2.0f, PointF(cx + direction * 7, cy),
           PointF(cx - direction * 7, cy));
    Stroke(g, color, 2.0f, PointF(cx - direction * 7, cy),
           PointF(cx - direction * 1, cy - 6));
    Stroke(g, color, 2.0f, PointF(cx - direction * 7, cy),
           PointF(cx - direction * 1, cy + 6));
}
void Reload(Graphics& g, int x, int y, const Color& color) {
    Pen pen(color, 2);
    pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen, x - 9, y - 9, 18, 18, 50, 285);
    Stroke(g, color, 2, PointF(static_cast<float>(x + 9), static_cast<float>(y - 3)),
           PointF(static_cast<float>(x + 9), static_cast<float>(y + 4)));
    Stroke(g, color, 2, PointF(static_cast<float>(x + 9), static_cast<float>(y + 4)),
           PointF(static_cast<float>(x + 3), static_cast<float>(y + 1)));
}
void Home(Graphics& g, int x, int y, const Color& color) {
    Stroke(g, color, 1.9f, PointF(x - 9.0f, y - 1.0f), PointF(x, y - 9.0f));
    Stroke(g, color, 1.9f, PointF(x, y - 9.0f), PointF(x + 9.0f, y - 1.0f));
    Stroke(g, color, 1.9f, PointF(x - 7.0f, y - 2.0f), PointF(x - 7.0f, y + 8.0f));
    Stroke(g, color, 1.9f, PointF(x + 7.0f, y - 2.0f), PointF(x + 7.0f, y + 8.0f));
    Stroke(g, color, 1.9f, PointF(x - 7.0f, y + 8.0f), PointF(x + 7.0f, y + 8.0f));
}
void Star(Graphics& g, int x, int y, bool fill) {
    constexpr double pi = 3.141592653589793;
    std::array<PointF, 10> points{};
    for (int i = 0; i < 10; ++i) {
        double theta = -pi / 2 + i * pi / 5;
        double r = i % 2 == 0 ? 10.0 : 4.4;
        points[static_cast<size_t>(i)] = PointF(
            static_cast<REAL>(x + std::cos(theta) * r),
            static_cast<REAL>(y + std::sin(theta) * r));
    }
    Pen pen(kAccent, 1.8f);
    g.DrawPolygon(&pen, points.data(), static_cast<INT>(points.size()));
    if (fill) {
        SolidBrush brush(kAccent);
        g.FillPolygon(&brush, points.data(), static_cast<INT>(points.size()));
    }
}
void Menu(Graphics& g, int x, int y) {
    for (int i = -1; i <= 1; ++i) {
        Stroke(g, kMuted, 2, PointF(x - 8.0f, y + i * 6.0f),
               PointF(x + 8.0f, y + i * 6.0f));
    }
}
void Plus(Graphics& g, int x, int y, const Color& color) {
    Stroke(g, color, 2, PointF(x - 6.0f, y), PointF(x + 6.0f, y));
    Stroke(g, color, 2, PointF(x, y - 6.0f), PointF(x, y + 6.0f));
}
void Cross(Graphics& g, int x, int y) {
    Stroke(g, kMuted, 1.5f, PointF(x - 4.0f, y - 4.0f), PointF(x + 4.0f, y + 4.0f));
    Stroke(g, kMuted, 1.5f, PointF(x + 4.0f, y - 4.0f), PointF(x - 4.0f, y + 4.0f));
}
} // namespace

Geometry Measure(int width, size_t tabCount) {
    Geometry result;
    const int w = std::max(680, width);
    const int count = static_cast<int>(std::max<size_t>(1, tabCount));
    result.tabWidth = std::clamp((w - 164) / count, 116, 224);
    const int left = std::max(210, (w - 700) / 2);
    const int right = std::min(w - 142, (w + 700) / 2);
    result.address = {left, 56, std::max(left + 180, right), 96};
    result.bookmark = Rect(result.address.right - 33, 64, 24, 24);
    result.menu = Rect(w - 48, 64, 29, 28);
    result.plus = Rect(17 + result.tabWidth * count, 12, 29, 28);
    return result;
}

Hit HitTest(int x, int y, int width, size_t tabCount) {
    if (y < 0 || y >= kToolbarHeight) return {};
    const Geometry g = Measure(width, tabCount);
    if (y < kTabStripHeight) {
        const int count = static_cast<int>(std::max<size_t>(1, tabCount));
        if (Contains(g.plus, x, y)) return {Action::AddTab, 0};
        for (int i = 0; i < count; ++i) {
            const RECT area = Rect(10 + i * g.tabWidth, 8, g.tabWidth - 2, 38);
            if (Contains(area, x, y)) {
                const RECT close = Rect(area.right - 31, 14, 23, 23);
                if (Contains(close, x, y)) return {Action::CloseTab, static_cast<size_t>(i)};
                return {Action::SelectTab, static_cast<size_t>(i)};
            }
        }
        return {};
    }
    if (Contains(Rect(12, 57, 36, 37), x, y)) return {Action::Back, 0};
    if (Contains(Rect(49, 57, 36, 37), x, y)) return {Action::Forward, 0};
    if (Contains(Rect(89, 57, 36, 37), x, y)) return {Action::Reload, 0};
    if (Contains(Rect(128, 57, 36, 37), x, y)) return {Action::Home, 0};
    if (Contains(g.bookmark, x, y)) return {Action::Bookmark, 0};
    if (Contains(g.menu, x, y)) return {Action::Menu, 0};
    return {};
}

void Draw(HDC dc, int width, int height, const std::vector<Tab>& tabs,
          size_t activeTab, bool canBack, bool canForward,
          bool bookmarked, bool loading) {
    if (!dc) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const Geometry geo = Measure(width, tabs.size());
    SolidBrush top(kTop);
    SolidBrush surface(kSurface);
    g.FillRectangle(&top, 0, 0, width, kTabStripHeight);
    g.FillRectangle(&surface, 0, kTabStripHeight, width,
                    kToolbarHeight - kTabStripHeight);
    Pen line(kBorder, 1);
    g.DrawLine(&line, 0, kToolbarHeight - 1, width, kToolbarHeight - 1);

    for (size_t i = 0; i < tabs.size(); ++i) {
        const int x = 10 + static_cast<int>(i) * geo.tabWidth;
        const bool selected = i == activeTab;
        Rounded(g, RectF(static_cast<REAL>(x), selected ? 8.0f : 12.0f,
                         static_cast<REAL>(geo.tabWidth - 3), selected ? 41.0f : 34.0f),
                11.0f, selected ? kSurface : kInactiveTab);
        SolidBrush dot(selected ? kAccent : kMuted);
        g.FillEllipse(&dot, x + 12, 23, 8, 8);
        const auto& tab = tabs[i];
        Label(g, tab.title.empty() ? L"Nueva pestaña" : tab.title,
              static_cast<float>(x + 28), 11.0f,
              static_cast<float>(geo.tabWidth - 67), 31.0f,
              13.0f, selected ? kText : kMuted, selected);
        Cross(g, x + geo.tabWidth - 23, 27);
    }
    const int plusX = geo.plus.left + 14;
    const int plusY = geo.plus.top + 14;
    Plus(g, plusX, plusY, kMuted);

    const Color disabled(255, 177, 187, 200);
    Arrow(g, 29, 76, true, canBack ? kText : disabled);
    Arrow(g, 67, 76, false, canForward ? kText : disabled);
    Reload(g, 107, 76, kText);
    Home(g, 146, 76, kText);

    Rounded(g, RectF(static_cast<REAL>(geo.address.left), 56,
                     static_cast<REAL>(geo.address.right - geo.address.left), 40),
            19, Color(255, 255, 255, 255), &kBorder);
    // HTTPS marker: the URL bar itself shows the actual scheme; this icon does
    // not claim a verified TLS state for plain HTTP.
    Stroke(g, kMuted, 1.6f,
           PointF(static_cast<float>(geo.address.left + 19), 69.0f),
           PointF(static_cast<float>(geo.address.left + 19), 80.0f));
    Stroke(g, kMuted, 1.6f,
           PointF(static_cast<float>(geo.address.left + 19), 80.0f),
           PointF(static_cast<float>(geo.address.left + 29), 80.0f));
    Star(g, geo.bookmark.left + 12, geo.bookmark.top + 12, bookmarked);
    Menu(g, geo.menu.left + 14, geo.menu.top + 13);
    if (loading) {
        SolidBrush progress(kAccent);
        g.FillRectangle(&progress, 0, kToolbarHeight - 3,
                        std::max(30, width / 3), 3);
    }

    // Footer is intentionally quiet like the reference browser.
    SolidBrush footer(Color(255, 246, 248, 252));
    g.FillRectangle(&footer, 0, std::max(kToolbarHeight, height - kStatusHeight),
                    width, kStatusHeight);
    g.DrawLine(&line, 0, height - kStatusHeight, width, height - kStatusHeight);
}
} // namespace ac::chrome
