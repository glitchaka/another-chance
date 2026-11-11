#pragma once
#include "html/HtmlParser.h"
#include "style/Style.h"
#include "image/ImageStore.h"
#include <windows.h>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ac::layout {
class PageLayout {
public:
    PageLayout() = default;
    ~PageLayout();
    PageLayout(const PageLayout&) = delete;
    PageLayout& operator=(const PageLayout&) = delete;

    void Reflow(HDC dc, const html::Document& document,
                const style::Sheet& sheet, const image::Store& images,
                const std::wstring& pageUrl,
                int viewportWidth);
    void Draw(HDC dc, int top, int scroll, const RECT& viewport);
    [[nodiscard]] std::wstring LinkAt(int x, int y, int top, int scroll) const;
    [[nodiscard]] int Height() const { return height_; }
    [[nodiscard]] bool HasVisibleContent() const;

    struct Control {
        enum class Kind { Text, Password, Textarea, Checkbox, Radio,
                          Submit, Button, Reset, Select };
        const html::Node* node = nullptr;
        RECT rect{};
        Kind kind = Kind::Text;
    };
    [[nodiscard]] const std::vector<Control>& Controls() const {
        return controls_;
    }

private:
    struct Item {
        enum class Kind { Text, Rule, Image, Background };
        Kind kind = Kind::Text;
        RECT rect{};
        std::wstring text;
        std::wstring link;
        std::wstring imageUrl;
        COLORREF color = RGB(40,40,40);
        int fontSize = 16;
        bool bold = false;
        bool italic = false;
        bool underline = false;
    };
    std::vector<Item> items_;
    std::vector<Control> controls_;
    std::map<std::tuple<int,bool,bool,bool>, HFONT> fonts_;
    const style::Sheet* sheet_ = nullptr;
    const image::Store* images_ = nullptr;
    std::wstring pageUrl_;
    int left_ = 22;
    int right_ = 600;
    int x_ = 22;
    int y_ = 18;
    int lineHeight_ = 24;
    int height_ = 0;
    int listDepth_ = 0;
    size_t lineBegin_ = 0;
    style::TextAlign lineAlign_ = style::TextAlign::Left;

    HFONT Font(int fontSize, bool bold, bool italic, bool underline);
    void AlignLine();
    void NewLine();
    void BeginBlock(const style::Computed& style);
    void EndBlock(const style::Computed& style);
    void AppendText(HDC dc, std::wstring_view text, const style::Computed& style,
                    const std::wstring& link);
    void AppendWord(HDC dc, std::wstring_view word, const style::Computed& style,
                    const std::wstring& link, bool precededBySpace);
    void Walk(HDC dc, const html::Node& node, const style::Computed& inherited,
              const std::wstring& parentLink);
};
}
