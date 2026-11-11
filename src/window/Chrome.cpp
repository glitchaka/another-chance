#include "window/Chrome.h"
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace ac::chrome {
namespace {
using namespace Gdiplus;

// Custom chrome belongs entirely to Another Chance, not the OS or a web engine.
const Color kHeader(255, 232, 241, 247);
const Color kSurface(255, 251, 253, 255);
const Color kText(255, 31, 51, 71);
const Color kMuted(255, 99, 121, 138);
const Color kBorder(255, 209, 227, 235);
const Color kTeal(255, 19, 141, 144);
const Color kLightTeal(255, 225, 245, 241);
const Color kAmber(255, 218, 147, 59);
const Color kDanger(255, 207, 70, 77);

RECT Area(int x, int y, int w, int h) { return {x,y,x+w,y+h}; }
bool Contains(RECT r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
void Stroke(Graphics& g, const Color& color, float size, PointF a, PointF b) {
    Pen pen(color, size);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, a, b);
}
void Rounded(Graphics& g, RectF r, float radius, const Color& fill,
             const Color* border = nullptr) {
    GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X,r.Y,d,d,180.0f,90.0f);
    path.AddArc(r.GetRight()-d,r.Y,d,d,270.0f,90.0f);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0.0f,90.0f);
    path.AddArc(r.X,r.GetBottom()-d,d,d,90.0f,90.0f);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush,&path);
    if (border) {
        Pen edge(*border,1.0f);
        g.DrawPath(&edge,&path);
    }
}
void Label(Graphics& g, const wchar_t* text, float x, float y,
           float w, float h, float size, const Color& color, bool bold = false) {
    FontFamily family(L"Segoe UI");
    Font font(&family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text,-1,&font,RectF(x,y,w,h),&format,&brush);
}
void Back(Graphics& g,int x,int y,const Color& color) {
    Stroke(g,color,2.0f,PointF(x+7.0f,y),PointF(x-6.0f,y));
    Stroke(g,color,2.0f,PointF(x-6.0f,y),PointF(x+1.0f,y-7.0f));
    Stroke(g,color,2.0f,PointF(x-6.0f,y),PointF(x+1.0f,y+7.0f));
}
void Forward(Graphics& g,int x,int y,const Color& color) {
    Stroke(g,color,2.0f,PointF(x-7.0f,y),PointF(x+6.0f,y));
    Stroke(g,color,2.0f,PointF(x+6.0f,y),PointF(x-1.0f,y-7.0f));
    Stroke(g,color,2.0f,PointF(x+6.0f,y),PointF(x-1.0f,y+7.0f));
}
void Reload(Graphics& g,int x,int y) {
    Pen pen(kMuted,2.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen,x-9,y-9,18,18,65,285);
    Stroke(g,kMuted,2.0f,PointF(x+9.0f,y-4.0f),PointF(x+9.0f,y+3.0f));
    Stroke(g,kMuted,2.0f,PointF(x+9.0f,y+3.0f),PointF(x+3.0f,y));
}
void Star(Graphics& g,int x,int y,bool filled) {
    constexpr double pi = 3.141592653589793;
    std::array<PointF,10> pts{};
    for(int i=0;i<10;++i) {
        const double a = -pi/2.0+i*pi/5.0;
        const double radius = (i%2==0)?9.0:4.2;
        pts[static_cast<size_t>(i)] = PointF(
            static_cast<REAL>(x+std::cos(a)*radius),
            static_cast<REAL>(y+std::sin(a)*radius));
    }
    Pen pen(kAmber,1.8f);
    g.DrawPolygon(&pen,pts.data(),static_cast<INT>(pts.size()));
    if (filled) {
        SolidBrush brush(kAmber);
        g.FillPolygon(&brush,pts.data(),static_cast<INT>(pts.size()));
    }
}
void Cross(Graphics& g,int x,int y,const Color& c) {
    Stroke(g,c,1.6f,PointF(x-5.0f,y-5.0f),PointF(x+5.0f,y+5.0f));
    Stroke(g,c,1.6f,PointF(x+5.0f,y-5.0f),PointF(x-5.0f,y+5.0f));
}
void Minimize(Graphics& g,int x,int y) {
    Stroke(g,kText,1.7f,PointF(x-7.0f,y+4.0f),PointF(x+7.0f,y+4.0f));
}
void Maximize(Graphics& g,int x,int y,bool maximized) {
    Pen pen(kText,1.6f);
    if (maximized) {
        g.DrawRectangle(&pen,x-5,y-5,10,10);
        Stroke(g,kText,1.5f,PointF(x-2.0f,y-8.0f),PointF(x+8.0f,y-8.0f));
        Stroke(g,kText,1.5f,PointF(x+8.0f,y-8.0f),PointF(x+8.0f,y+2.0f));
    } else {
        g.DrawRectangle(&pen,x-6,y-6,12,12);
    }
}
} // namespace

Geometry Measure(int width) {
    const int w=std::max(860,width);
    Geometry g;
    g.back=Area(157,22,35,36);
    g.forward=Area(197,22,35,36);
    g.address=Area(247,17,std::max(200,w-247-361),44);
    g.reload=Area(w-346,22,34,36);
    g.bookmark=Area(w-308,22,34,36);
    g.tabs=Area(w-268,21,95,38);
    g.menu=Area(w-168,22,33,36);
    g.minimize=Area(w-132,0,44,49);
    g.maximize=Area(w-88,0,44,49);
    g.close=Area(w-44,0,44,49);
    g.home=Area(10,10,135,57);
    return g;
}

Hit HitTest(int x,int y,int width) {
    if (y<0 || y>=kToolbarHeight) return {};
    const Geometry g=Measure(width);
    if(Contains(g.close,x,y)) return {Action::Close};
    if(Contains(g.maximize,x,y)) return {Action::Maximize};
    if(Contains(g.minimize,x,y)) return {Action::Minimize};
    if(Contains(g.back,x,y)) return {Action::Back};
    if(Contains(g.forward,x,y)) return {Action::Forward};
    if(Contains(g.reload,x,y)) return {Action::Reload};
    if(Contains(g.bookmark,x,y)) return {Action::Bookmark};
    if(Contains(g.tabs,x,y)) return {Action::TabMenu};
    if(Contains(g.menu,x,y)) return {Action::Menu};
    if(Contains(g.home,x,y)) return {Action::Home};
    return {};
}

bool IsDraggable(int x,int y,int width) {
    if(y<0 || y>=kToolbarHeight) return false;
    const Geometry g=Measure(width);
    if(Contains(g.address,x,y)) return false;
    return HitTest(x,y,width).action==Action::None;
}

void Draw(HDC dc,int width,int height,const std::vector<Tab>& tabs,
          size_t activeTab,bool canBack,bool canForward,
          bool bookmarked,bool loading,bool maximized) {
    if(!dc) return;
    Graphics graphics(dc);
    Graphics& g=graphics;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const Geometry geometry=Measure(width);
    SolidBrush header(kHeader);
    g.FillRectangle(&header,0,0,width,kToolbarHeight);
    Pen boundary(kBorder,1.0f);
    g.DrawLine(&boundary,0,kToolbarHeight-1,width,kToolbarHeight-1);

    // Identity block: circular monogram + two-line wordmark, not a tab bar.
    Rounded(g,RectF(15,17,43,43),13,kTeal);
    Label(g,L"A",27,21,20,32,23,kSurface,true);
    SolidBrush gold(kAmber);
    g.FillEllipse(&gold,48,15,9,9);
    Label(g,L"ANOTHER",66,18,87,21,13,kText,true);
    Label(g,L"CHANCE",66,38,87,19,11,kMuted,true);

    const Color disabled(255,162,179,190);
    Back(g,175,40,canBack?kText:disabled);
    Forward(g,215,40,canForward?kText:disabled);

    const auto& a=geometry.address;
    Rounded(g,RectF(static_cast<REAL>(a.left),static_cast<REAL>(a.top),
                    static_cast<REAL>(a.right-a.left),static_cast<REAL>(a.bottom-a.top)),
            13,kSurface,&kBorder);
    // Custom compass marker; not the padlock/omnibox symbolism of Chromium.
    SolidBrush circle(kLightTeal);
    g.FillEllipse(&circle,a.left+12,27,23,23);
    Stroke(g,kTeal,1.6f,PointF(static_cast<REAL>(a.left+24),31),
           PointF(static_cast<REAL>(a.left+24),47));
    Stroke(g,kTeal,1.6f,PointF(static_cast<REAL>(a.left+17),39),
           PointF(static_cast<REAL>(a.left+31),39));

    Reload(g,geometry.reload.left+17,40);
    Star(g,geometry.bookmark.left+17,40,bookmarked);

    const auto& tb=geometry.tabs;
    Rounded(g,RectF(static_cast<REAL>(tb.left),static_cast<REAL>(tb.top),
                    static_cast<REAL>(tb.right-tb.left),static_cast<REAL>(tb.bottom-tb.top)),
            11,kLightTeal);
    const auto count=std::to_wstring(tabs.size());
    Label(g,L"VISTAS",tb.left+10.0f,tb.top+2.0f,45,32,10,kTeal,true);
    Label(g,count.c_str(),tb.left+58.0f,tb.top+2.0f,18,32,15,kText,true);
    Stroke(g,kTeal,1.5f,PointF(tb.right-14.0f,37),
           PointF(tb.right-10.0f,41));
    Stroke(g,kTeal,1.5f,PointF(tb.right-10.0f,41),
           PointF(tb.right-6.0f,37));

    const int mx=geometry.menu.left+17;
    for(int i=-1;i<=1;++i) {
        SolidBrush point(kMuted);
        g.FillEllipse(&point,mx-2,38+i*7,4,4);
    }
    Minimize(g,geometry.minimize.left+22,25);
    Maximize(g,geometry.maximize.left+22,25,maximized);
    // Distinctive custom close surface; no Windows caption is drawn.
    Rounded(g,RectF(static_cast<REAL>(geometry.close.left+6),10,31,32),
            9,Color(255,249,225,225));
    Cross(g,geometry.close.left+22,26,kDanger);

    if(loading) {
        SolidBrush progress(kTeal);
        g.FillRectangle(&progress,0,kToolbarHeight-3,std::max(30,width/3),3);
    }
    SolidBrush footer(Color(255,248,251,253));
    g.FillRectangle(&footer,0,std::max(kToolbarHeight,height-kStatusHeight),
                    width,kStatusHeight);
    g.DrawLine(&boundary,0,height-kStatusHeight,width,height-kStatusHeight);
    (void)activeTab;
}
} // namespace ac::chrome
