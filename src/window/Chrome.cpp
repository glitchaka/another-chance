#include "window/Chrome.h"
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

// Compact, original Win32 toolbar.
// Spacing and restrained color treatment informed by the public style sources
// for KDE Breeze, GNOME libadwaita and Linux Mint Mint-Y.
// No GTK, Qt, KDE, Cinnamon or third-party browser engine is linked or copied.

namespace ac::chrome {
namespace {
using namespace Gdiplus;
const Color kSurface(255, 249, 250, 252);
const Color kPaper(255, 255, 255, 255);
const Color kInk(255, 54, 62, 74);
const Color kDim(255, 129, 141, 153);
const Color kEdge(255, 219, 224, 231);
const Color kAccent(255, 60, 119, 185);
const Color kAccentSoft(255, 240, 246, 252);
const Color kDanger(255, 193, 67, 72);

RECT Area(int x, int y, int w, int h) { return {x, y, x+w, y+h}; }
bool Contains(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
void Stroke(Graphics& g, const Color& c, float thickness, PointF a, PointF b) {
    Pen pen(c, thickness);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, a, b);
}
void Rounded(Graphics& g, const RectF& r, float radius,
             const Color& fill, const Color* border=nullptr) {
    GraphicsPath path;
    const float d=radius*2;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight()-d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight()-d, r.GetBottom()-d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom()-d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    if(border){Pen pen(*border, 1.0f);g.DrawPath(&pen, &path);}
}
void Text(Graphics& g, const std::wstring& str, const RectF& r,
          float size, const Color& color, bool bold=false) {
    FontFamily family(L"Segoe UI");
    Font font(&family, size, bold?FontStyleBold:FontStyleRegular, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(str.c_str(), -1, &font, r, &format, &brush);
}
void Arrow(Graphics& g, int x, int y, bool backward, const Color& c) {
    const float d=backward?-1.0f:1.0f;
    Stroke(g,c,1.7f,PointF(x-d*6,y),PointF(x+d*6,y));
    Stroke(g,c,1.7f,PointF(x-d*6,y),PointF(x,y-6));
    Stroke(g,c,1.7f,PointF(x-d*6,y),PointF(x,y+6));
}
void Reload(Graphics& g, int x, int y) {
    Pen pen(kInk,1.7f);
    g.DrawArc(&pen,x-7,y-7,14,14,52,295);
    Stroke(g,kInk,1.7f,PointF(x+7.0f,y-4.0f),PointF(x+7.0f,y+3.0f));
    Stroke(g,kInk,1.7f,PointF(x+7.0f,y+3.0f),PointF(x+2.0f,y));
}
void Star(Graphics& g, int x, int y, bool filled) {
    constexpr double pi=3.14159265358979323846;
    std::array<PointF,10> pts{};
    for(int i=0;i<10;++i){
        const double a=-pi/2+i*pi/5;
        const double r=i%2==0?8.0:3.8;
        pts[static_cast<size_t>(i)]=PointF(
            static_cast<REAL>(x+cos(a)*r),
            static_cast<REAL>(y+sin(a)*r));
    }
    Pen pen(kInk,1.4f);
    g.DrawPolygon(&pen,pts.data(),static_cast<INT>(pts.size()));
    if(filled){SolidBrush b(kAccent);g.FillPolygon(&b,pts.data(),
                                      static_cast<INT>(pts.size()));}
}
void Dots(Graphics& g, int x, int y) {
    SolidBrush brush(kInk);
    for(int i=-1;i<=1;++i)g.FillEllipse(&brush,x-1.5f,y+i*5.5f-1.5f,3.0f,3.0f);
}
void WindowIcon(Graphics& g, Action a, int x, int y, bool maximized) {
    if(a==Action::Minimize){
        Stroke(g,kInk,1.5f,PointF(x-5.0f,y+4.0f),PointF(x+5.0f,y+4.0f));
    }else if(a==Action::Maximize){
        Pen pen(kInk,1.4f);
        if(maximized){
            g.DrawRectangle(&pen,x-5,y-4,10,9);
            Stroke(g,kInk,1.4f,PointF(x-2.0f,y-7.0f),PointF(x+7.0f,y-7.0f));
            Stroke(g,kInk,1.4f,PointF(x+7.0f,y-7.0f),PointF(x+7.0f,y+2.0f));
        } else g.DrawRectangle(&pen,x-5,y-5,10,10);
    }else if(a==Action::Close){
        Stroke(g,kDanger,1.7f,PointF(x-5.0f,y-5.0f),PointF(x+5.0f,y+5.0f));
        Stroke(g,kDanger,1.7f,PointF(x-5.0f,y+5.0f),PointF(x+5.0f,y-5.0f));
    }
}
} // namespace

Geometry Measure(int width){
    const int w=std::max(860,width);
    Geometry g;
    g.home=Area(10,8,162,40);
    g.back=Area(177,10,30,36);
    g.forward=Area(211,10,30,36);
    g.address=Area(251,10,std::max(150,w-251-309),36);
    g.reload=Area(w-302,10,31,36);
    g.bookmark=Area(w-268,10,31,36);
    g.tabs=Area(w-231,10,67,36);
    g.menu=Area(w-161,10,30,36);
    g.minimize=Area(w-123,5,39,42);
    g.maximize=Area(w-83,5,39,42);
    g.close=Area(w-43,5,39,42);
    return g;
}
Hit HitTest(int x,int y,int width){
    if(y<0||y>=kToolbarHeight)return {};
    const auto g=Measure(width);
    if(Contains(g.close,x,y))return {Action::Close};
    if(Contains(g.maximize,x,y))return {Action::Maximize};
    if(Contains(g.minimize,x,y))return {Action::Minimize};
    if(Contains(g.back,x,y))return {Action::Back};
    if(Contains(g.forward,x,y))return {Action::Forward};
    if(Contains(g.reload,x,y))return {Action::Reload};
    if(Contains(g.bookmark,x,y))return {Action::Bookmark};
    if(Contains(g.tabs,x,y))return {Action::TabMenu};
    if(Contains(g.menu,x,y))return {Action::Menu};
    if(Contains(g.home,x,y))return {Action::Home};
    return {};
}
bool IsDraggable(int x,int y,int width){
    if(y<0||y>=kToolbarHeight)return false;
    const auto g=Measure(width);
    return !Contains(g.address,x,y) && HitTest(x,y,width).action==Action::None;
}
void Draw(HDC dc,int width,int height,const std::vector<Tab>& tabs,
          size_t activeTab,bool canBack,bool canForward,
          bool bookmarked,bool loading,bool maximized){
    if(!dc)return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush surface(kSurface);
    g.FillRectangle(&surface,0,0,width,kToolbarHeight);
    Pen edge(kEdge,1);
    g.DrawLine(&edge,0,kToolbarHeight-1,width,kToolbarHeight-1);

    const Geometry geom=Measure(width);
    // Always draw the same bundled icon used by Explorer and the taskbar.
    const auto appIcon = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
        IMAGE_ICON, 28, 28, LR_SHARED));
    if (appIcon) {
        DrawIconEx(dc, 12, 14, appIcon, 28, 28, 0, nullptr, DI_NORMAL);
    }
    Text(g,L"Another Chance",RectF(48,11,121,34),12,kInk,true);

    Arrow(g,192,28,true,canBack?kInk:kDim);
    Arrow(g,226,28,false,canForward?kInk:kDim);

    const RECT a=geom.address;
    Rounded(g,RectF(static_cast<REAL>(a.left),static_cast<REAL>(a.top),
                    static_cast<REAL>(a.right-a.left),static_cast<REAL>(a.bottom-a.top)),
            8,kPaper,&kEdge);
    // Address text is an actual Win32 EDIT child. No decorative symbol inside.

    Reload(g,geom.reload.left+15,28);
    Star(g,geom.bookmark.left+15,28,bookmarked);

    const auto& tabsArea=geom.tabs;
    Rounded(g,RectF(static_cast<REAL>(tabsArea.left),static_cast<REAL>(tabsArea.top),
                    static_cast<REAL>(tabsArea.right-tabsArea.left),
                    static_cast<REAL>(tabsArea.bottom-tabsArea.top)),
            7,kAccentSoft);
    Text(g,std::to_wstring(tabs.size()),
         RectF(static_cast<REAL>(tabsArea.left+13),12,20,31),13,kInk,true);
    Stroke(g,kDim,1.4f,PointF(tabsArea.right-22.0f,24.0f),
                            PointF(tabsArea.right-17.0f,29.0f));
    Stroke(g,kDim,1.4f,PointF(tabsArea.right-17.0f,29.0f),
                            PointF(tabsArea.right-12.0f,24.0f));
    Dots(g,geom.menu.left+15,28);

    WindowIcon(g,Action::Minimize,geom.minimize.left+19,26,maximized);
    WindowIcon(g,Action::Maximize,geom.maximize.left+19,26,maximized);
    WindowIcon(g,Action::Close,geom.close.left+19,26,maximized);
    if(loading) {
        SolidBrush progress(kAccent);
        g.FillRectangle(&progress,0,kToolbarHeight-2,std::max(20,width/4),2);
    }
    SolidBrush footer(Color(255,250,251,252));
    g.FillRectangle(&footer,0,std::max(kToolbarHeight,height-kStatusHeight),
                    width,kStatusHeight);
    g.DrawLine(&edge,0,height-kStatusHeight,width,height-kStatusHeight);
    (void)tabs;(void)activeTab;
}
} // namespace ac::chrome
