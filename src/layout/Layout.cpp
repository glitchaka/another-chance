#include "layout/Layout.h"
#include "util/Url.h"
#include <algorithm>
#include <cwctype>

namespace ac::layout {
namespace {
bool IsList(std::string_view tag) { return tag == "ul" || tag == "ol"; }
} // namespace

PageLayout::~PageLayout() {
    for (auto& entry : fonts_) DeleteObject(entry.second);
}

HFONT PageLayout::Font(int size, bool bold, bool italic, bool underline) {
    const auto key = std::make_tuple(size, bold, italic, underline);
    auto found = fonts_.find(key);
    if (found != fonts_.end()) return found->second;
    LOGFONTW info{};
    info.lfHeight = -MulDiv(size, 96, 96);
    info.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    info.lfItalic = italic ? TRUE : FALSE;
    info.lfUnderline = underline ? TRUE : FALSE;
    wcscpy_s(info.lfFaceName, L"Segoe UI");
    const HFONT font = CreateFontIndirectW(&info);
    fonts_.emplace(key, font);
    return font;
}

void PageLayout::NewLine() {
    x_ = left_ + listDepth_ * 22;
    y_ += std::max(20, lineHeight_);
    lineHeight_ = 24;
}

void PageLayout::BeginBlock(const style::Computed& style) {
    if (x_ > left_ + listDepth_ * 22) NewLine();
    y_ += style.marginTop;
}
void PageLayout::EndBlock(const style::Computed& style) {
    if (x_ > left_ + listDepth_ * 22) NewLine();
    y_ += style.marginBottom;
}

void PageLayout::AppendWord(HDC dc, std::wstring_view word,
                            const style::Computed& style,
                            const std::wstring& link, bool precededBySpace) {
    if (word.empty()) return;
    HFONT font = Font(style.fontSize, style.bold, style.italic, style.underline);
    HGDIOBJ previous = SelectObject(dc, font);
    auto measure = [dc](std::wstring_view text) -> SIZE {
        SIZE size{};
        if (!text.empty()) GetTextExtentPoint32W(dc, text.data(),
                                                  static_cast<int>(text.size()), &size);
        return size;
    };
    const SIZE space = measure(L" ");
    const int minX = left_ + listDepth_ * 22;
    if (precededBySpace && x_ > minX) x_ += space.cx;
    std::wstring remaining(word);
    while (!remaining.empty()) {
        SIZE extent = measure(remaining);
        if (x_ + extent.cx > right_ && x_ > minX) NewLine();
        extent = measure(remaining);
        size_t chars = remaining.size();
        if (extent.cx > right_ - x_) {
            chars = 1;
            while (chars < remaining.size() &&
                   measure(std::wstring_view(remaining).substr(0, chars + 1)).cx <= right_ - x_) {
                ++chars;
            }
            extent = measure(std::wstring_view(remaining).substr(0, chars));
        }
        Item item;
        item.kind = Item::Kind::Text;
        item.text = remaining.substr(0, chars);
        item.link = link;
        item.color = style.color;
        item.fontSize = style.fontSize;
        item.bold = style.bold;
        item.italic = style.italic;
        item.underline = style.underline;
        item.rect = {x_, y_, x_ + extent.cx, y_ + std::max(static_cast<int>(extent.cy), style.fontSize + 2)};
        items_.push_back(std::move(item));
        x_ += extent.cx;
        lineHeight_ = std::max(lineHeight_, std::max(static_cast<int>(extent.cy) + 4, style.fontSize + 7));
        remaining.erase(0, chars);
        if (!remaining.empty()) NewLine();
    }
    SelectObject(dc, previous);
}

void PageLayout::AppendText(HDC dc, std::wstring_view text,
                            const style::Computed& style,
                            const std::wstring& link) {
    std::wstring word;
    bool precedingSpace = false;
    for (wchar_t c : text) {
        if (std::iswspace(c)) {
            if (!word.empty()) {
                AppendWord(dc, word, style, link, precedingSpace);
                word.clear();
                precedingSpace = false;
            }
            precedingSpace = true;
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) AppendWord(dc, word, style, link, precedingSpace);
    else if (precedingSpace && x_ > left_ + listDepth_ * 22) {
        // Preserve an inter-element whitespace separator.
        HFONT font = Font(style.fontSize, style.bold, style.italic, style.underline);
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, L" ", 1, &size);
        if (x_ + size.cx < right_) x_ += size.cx;
        SelectObject(dc, previous);
    }
}

void PageLayout::Walk(HDC dc, const html::Node& node,
                      const style::Computed& inherited,
                      const std::wstring& parentLink) {
    if (node.type == html::Node::Type::Text) {
        AppendText(dc, ac::url::ToWide(node.text), inherited, parentLink);
        return;
    }
    const auto computed = sheet_->Compute(node, inherited);
    if (computed.hidden) return;
    const auto& tag = node.tag;
    if (tag == "br") { NewLine(); return; }
    if (tag == "hr") {
        if (x_ > left_ + listDepth_ * 22) NewLine();
        Item item;
        item.kind = Item::Kind::Rule;
        item.rect = {left_, y_ + 6, right_, y_ + 8};
        item.color = RGB(209, 214, 221);
        items_.push_back(std::move(item));
        y_ += 14;
        return;
    }

    std::wstring link = parentLink;
    if (tag == "a") {
        const auto href = node.Attribute("href");
        if (!href.empty()) link = ac::url::Resolve(pageUrl_, ac::url::ToWide(href));
    }
    if (tag == "img") {
        const auto alt = node.Attribute("alt");
        AppendText(dc, alt.empty() ? L"[Imagen]" :
            L"[Imagen: " + ac::url::ToWide(alt) + L"]", computed, link);
        return;
    }
    if (tag == "input" || tag == "button" || tag == "textarea") {
        const auto label = node.Attribute("placeholder");
        const auto value = node.Attribute("value");
        AppendText(dc, L"[ " + ac::url::ToWide(!value.empty() ? value :
                   (!label.empty() ? label : node.Attribute("type"))) + L" ]",
                   computed, link);
        return;
    }

    if (computed.block) BeginBlock(computed);
    if (IsList(tag)) ++listDepth_;
    if (tag == "li") {
        AppendText(dc, L"\u2022 ", computed, link);
    }
    for (const auto& child : node.children) Walk(dc, *child, computed, link);
    if (IsList(tag)) {
        listDepth_ = std::max(0, listDepth_ - 1);
    }
    if (computed.block) EndBlock(computed);
}

void PageLayout::Reflow(HDC dc, const html::Document& document,
                        const style::Sheet& sheet, const std::wstring& pageUrl,
                        int viewportWidth) {
    items_.clear();
    sheet_ = &sheet;
    pageUrl_ = pageUrl;
    left_ = 22;
    right_ = std::max(left_ + 80, viewportWidth - 24);
    x_ = left_;
    y_ = 18;
    lineHeight_ = 24;
    listDepth_ = 0;
    const style::Computed initial{};
    if (document.root) Walk(dc, *document.root, initial, {});
    height_ = y_ + lineHeight_ + 24;
}

void PageLayout::Draw(HDC dc, int top, int scroll, const RECT& viewport) {
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    SetBkMode(dc, TRANSPARENT);
    for (const Item& item : items_) {
        const int y = item.rect.top + top - scroll;
        const int bottom = item.rect.bottom + top - scroll;
        if (bottom < viewport.top || y > viewport.bottom) continue;
        if (item.kind == Item::Kind::Rule) {
            HPEN pen = CreatePen(PS_SOLID, 1, item.color);
            HGDIOBJ prior = SelectObject(dc, pen);
            MoveToEx(dc, item.rect.left, y, nullptr);
            LineTo(dc, item.rect.right, y);
            SelectObject(dc, prior);
            DeleteObject(pen);
        } else {
            HFONT font = Font(item.fontSize, item.bold, item.italic, item.underline);
            HGDIOBJ prior = SelectObject(dc, font);
            SetTextColor(dc, item.color);
            TextOutW(dc, item.rect.left, y, item.text.data(),
                     static_cast<int>(item.text.size()));
            SelectObject(dc, prior);
        }
    }
    RestoreDC(dc, saved);
}

std::wstring PageLayout::LinkAt(int x, int y, int top, int scroll) const {
    const int contentY = y - top + scroll;
    for (const auto& item : items_) {
        if (!item.link.empty() && x >= item.rect.left && x < item.rect.right &&
            contentY >= item.rect.top && contentY < item.rect.bottom) return item.link;
    }
    return {};
}
} // namespace ac::layout
