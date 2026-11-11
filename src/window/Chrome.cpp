#include "window/Chrome.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace ac::chrome {
namespace {
using namespace Gdiplus;

const Color kTabStrip(255, 236, 241, 247);
const Color kToolbar(255, 250, 251, 253);
const Color kPaper(255, 255, 255, 255);
const Color kInk(255, 44, 59, 77);
const Color kMuted(255, 112, 128, 148);
const Color kEdge(255, 207, 217, 230);
const Color kAccent(255, 35, 133, 158);
const Color kDanger(255, 193, 51, 61);
const Color kInactive(255, 94, 110, 126);

RECT Rect(int x,int y,int w,int h) { return {x,y,x+w,y+h}; }
bool Contains(const RECT& r,int x,int y) {
    return x>=r.left && x<r.right && y>=r.top && y<r.bottom;
}
void Line(Graphics& g,const Color& c,float thick,PointF a,PointF b) {
    Pen pen(c,thick);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen,a,b);
}
void Rounded(Graphics& g,RectF rect,float radius,
             const Color& fill,const Color* outline=nullptr) {
    GraphicsPath path;
    const float d=radius*2;
    path.AddArc(rect.X,rect.Y,d,d,180.0f,90.0f);
    path.AddArc(rect.GetRight()-d,rect.Y,d,d,270.0f,90.0f);
    path.AddArc(rect.GetRight()-d,rect.GetBottom()-d,d,d,0.0f,90.0f);
    path.AddArc(rect.X,rect.GetBottom()-d,d,d,90.0f,90.0f);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush,&path);
    if(outline) {
        Pen p(*outline,1.0f);
        g.DrawPath(&p,&path);
    }
}
void Label(Graphics& g,const std::wstring& value,RectF rect,
           float px,const Color& color,bool bold=false) {
    FontFamily family(L"Segoe UI");
    Font font(&family,px,bold?FontStyleBold:FontStyleRegular,UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt;
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetAlignment(StringAlignmentNear);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(value.c_str(),-1,&font,rect,&fmt,&brush);
}
void ChevronArrow(Graphics& g,int x,int y,bool backward,const Color& c) {
    const float direction=backward?-1.0f:1.0f;
    Line(g,c,1.7f,PointF(x-direction*6,y),PointF(x+direction*6,y));
    Line(g,c,1.7f,PointF(x-direction*6,y),PointF(x,y-6));
    Line(g,c,1.7f,PointF(x-direction*6,y),PointF(x,y+6));
}
void Refresh(Graphics& g,int x,int y) {
    Pen pen(kInk,1.6f);
    g.DrawArc(&pen,x-7,y-7,14,14,35,305);
    Line(g,kInk,1.5f,PointF(x+7.0f,y-4.0f),PointF(x+7.0f,y+3.0f));
    Line(g,kInk,1.5f,PointF(x+7.0f,y+3.0f),PointF(x+2.0f,y));
}
void Star(Graphics& g,int x,int y,bool filled) {
    constexpr double pi=3.14159265358979323846;
    std::array<PointF,10> points{};
    for(int i=0;i<10;++i) {
        const double a=-pi/2+i*pi/5;
        const double radius=i%2==0?8.0:3.6;
        points[static_cast<size_t>(i)]=PointF(
            static_cast<REAL>(x+cos(a)*radius),
            static_cast<REAL>(y+sin(a)*radius));
    }
    Pen pen(kMuted,1.4f);
    g.DrawPolygon(&pen,points.data(),static_cast<INT>(points.size()));
    if(filled) {
        SolidBrush brush(kAccent);
        g.FillPolygon(&brush,points.data(),static_cast<INT>(points.size()));
    }
}
void Dots(Graphics& g,int x,int y) {
    SolidBrush brush(kInk);
    for(int i=-1;i<=1;++i)
        g.FillEllipse(&brush,static_cast<REAL>(x-1.4f),
                      static_cast<REAL>(y+i*5.5f-1.4f),2.8f,2.8f);
}
void CaptionButton(Graphics& g,Action action,int x,int y,bool maximized) {
    if(action==Action::Minimize) {
        Line(g,kInk,1.5f,PointF(x-5.0f,y+4.0f),PointF(x+5.0f,y+4.0f));
    } else if(action==Action::Maximize) {
        Pen pen(kInk,1.3f);
        if(maximized) {
            g.DrawRectangle(&pen,x-5,y-3,9,8);
            Line(g,kInk,1.3f,PointF(x-2.0f,y-6.0f),PointF(x+6.0f,y-6.0f));
            Line(g,kInk,1.3f,PointF(x+6.0f,y-6.0f),PointF(x+6.0f,y+2.0f));
        } else {
            g.DrawRectangle(&pen,x-5,y-5,10,10);
        }
    } else if(action==Action::Close) {
        Line(g,kDanger,1.6f,PointF(x-4.5f,y-4.5f),PointF(x+4.5f,y+4.5f));
        Line(g,kDanger,1.6f,PointF(x-4.5f,y+4.5f),PointF(x+4.5f,y-4.5f));
    }
}
int AvailableTabWidth(int width,size_t count) {
    const int available=std::max(100,width-12-172);
    return std::clamp(available/std::max(1,static_cast<int>(count)),104,224);
}
} // namespace

Geometry Measure(int width) {
    const int w=std::max(width,480);
    Geometry geom;
    geom.back=Rect(12,39,32,32);
    geom.forward=Rect(48,39,32,32);
    geom.reload=Rect(84,39,32,32);
    geom.address=Rect(128,39,std::max(130,w-128-94),32);
    geom.bookmark=Rect(w-91,39,32,32);
    geom.menu=Rect(w-52,39,36,32);
    geom.minimize=Rect(w-133,0,43,kTabRowHeight);
    geom.maximize=Rect(w-89,0,43,kTabRowHeight);
    geom.close=Rect(w-45,0,45,kTabRowHeight);
    return geom;
}

RECT TabBounds(int width,size_t count,size_t index) {
    const int step=AvailableTabWidth(width,count);
    return Rect(8+step*static_cast<int>(index),4,step-3,29);
}
RECT NewTabBounds(int width,size_t count) {
    const int start=8+AvailableTabWidth(width,count)*static_cast<int>(count);
    const int right=std::max(start, width-139);
    return Rect(std::min(start,right),4,32,28);
}
Hit HitTest(int x,int y,int width,size_t count) {
    if(x<0||y<0||y>=kToolbarHeight) return {};
    const auto geom=Measure(width);
    if(y<kTabRowHeight) {
        if(Contains(geom.close,x,y)) return {Action::Close};
        if(Contains(geom.maximize,x,y)) return {Action::Maximize};
        if(Contains(geom.minimize,x,y)) return {Action::Minimize};
        for(size_t i=0;i<count;++i) {
            const RECT tab=TabBounds(width,count,i);
            if(Contains(tab,x,y)) {
                if(x>=tab.right-29) return {Action::TabClose,i};
                return {Action::TabSelect,i};
            }
        }
        if(count<5 && Contains(NewTabBounds(width,count),x,y))
            return {Action::NewTab};
        return {};
    }
    if(Contains(geom.back,x,y)) return {Action::Back};
    if(Contains(geom.forward,x,y)) return {Action::Forward};
    if(Contains(geom.reload,x,y)) return {Action::Reload};
    if(Contains(geom.bookmark,x,y)) return {Action::Bookmark};
    if(Contains(geom.menu,x,y)) return {Action::Menu};
    return {};
}
bool IsDraggable(int x,int y,int width,size_t count) {
    if(y<0||y>=kTabRowHeight) return false;
    return HitTest(x,y,width,count).action==Action::None;
}
void Draw(HDC dc,int width,int height,const std::vector<Tab>& tabs,
          size_t activeTab,bool canBack,bool canForward,
          bool bookmarked,bool loading,bool maximized) {
    if(!dc) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush strip(kTabStrip);
    SolidBrush toolbar(kToolbar);
    g.FillRectangle(&strip,0,0,width,kTabRowHeight);
    g.FillRectangle(&toolbar,0,kTabRowHeight,width,
                    kToolbarHeight-kTabRowHeight);
    Pen border(kEdge,1.0f);
    g.DrawLine(&border,0,kToolbarHeight-1,width,kToolbarHeight-1);

    const auto geometry=Measure(width);
    for(size_t i=0;i<tabs.size();++i) {
        const RECT tab=TabBounds(width,tabs.size(),i);
        const bool active=i==activeTab;
        if(active) {
            Rounded(g,RectF(static_cast<REAL>(tab.left),static_cast<REAL>(tab.top),
                static_cast<REAL>(tab.right-tab.left),
                static_cast<REAL>(tab.bottom-tab.top+4)),6,kPaper,&kEdge);
            SolidBrush accent(kAccent);
            g.FillRectangle(&accent,tab.left+9,tab.top,26,2);
        }
        SolidBrush dot(tabs[i].loading ? kAccent : kInactive);
        g.FillEllipse(&dot,tab.left+11,tab.top+12,7,7);
        const std::wstring name=tabs[i].title.empty() ?
            L"Nueva pestaña" : tabs[i].title;
        Label(g,name,RectF(static_cast<REAL>(tab.left+25),
                          static_cast<REAL>(tab.top+1),
                          static_cast<REAL>(std::max(22,tab.right-tab.left-57)),
                          28),12,active?kInk:kMuted,active);
        Line(g,kMuted,1.4f,PointF(static_cast<REAL>(tab.right-21),14),
                             PointF(static_cast<REAL>(tab.right-13),22));
        Line(g,kMuted,1.4f,PointF(static_cast<REAL>(tab.right-21),22),
                             PointF(static_cast<REAL>(tab.right-13),14));
    }
    if(tabs.size()<5) {
        const RECT add=NewTabBounds(width,tabs.size());
        const float cx=(add.left+add.right)/2.0f;
        Line(g,kInk,1.6f,PointF(cx-5,18),PointF(cx+5,18));
        Line(g,kInk,1.6f,PointF(cx,13),PointF(cx,23));
    }
    CaptionButton(g,Action::Minimize,geometry.minimize.left+21,17,maximized);
    CaptionButton(g,Action::Maximize,geometry.maximize.left+21,17,maximized);
    CaptionButton(g,Action::Close,geometry.close.left+21,17,maximized);

    ChevronArrow(g,28,55,true,canBack?kInk:kMuted);
    ChevronArrow(g,64,55,false,canForward?kInk:kMuted);
    Refresh(g,100,55);
    const RECT url=geometry.address;
    Rounded(g,RectF(static_cast<REAL>(url.left),
            static_cast<REAL>(url.top),
            static_cast<REAL>(url.right-url.left),
            static_cast<REAL>(url.bottom-url.top)),7,kPaper,&kEdge);
    Star(g,geometry.bookmark.left+16,55,bookmarked);
    Dots(g,geometry.menu.left+18,55);

    if(loading) {
        SolidBrush progress(kAccent);
        g.FillRectangle(&progress,0,kToolbarHeight-2,
                        std::max(20,width/5),2);
    }
    SolidBrush footer(Color(255,249,250,252));
    g.FillRectangle(&footer,0,std::max(kToolbarHeight,height-kStatusHeight),
                    width,kStatusHeight);
    g.DrawLine(&border,0,height-kStatusHeight,width,height-kStatusHeight);
}
} // namespace ac::chrome
