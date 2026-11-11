#include "layout/Layout.h"
#include "tech/IdScreenRect.h"
#include "util/Url.h"
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <cmath>

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

void PageLayout::AlignLine() {
    if (lineBegin_ >= items_.size() ||
        lineAlign_ == style::TextAlign::Left) return;
    int lastRight = left_;
    for (size_t i = lineBegin_; i < items_.size(); ++i) {
        if (items_[i].kind == Item::Kind::Text && items_[i].rect.top == y_) {
            lastRight = std::max(lastRight, static_cast<int>(items_[i].rect.right));
        }
    }
    const int room = std::max(0, right_ - lastRight);
    const int dx = lineAlign_ == style::TextAlign::Center ? room / 2 : room;
    for (size_t i = lineBegin_; i < items_.size(); ++i) {
        if (items_[i].kind == Item::Kind::Text && items_[i].rect.top == y_) {
            items_[i].rect.left += dx;
            items_[i].rect.right += dx;
        }
    }
}

void PageLayout::NewLine() {
    AlignLine();
    x_ = left_ + listDepth_ * 22;
    y_ += std::max(20, lineHeight_);
    lineHeight_ = 24;
    lineBegin_ = items_.size();
    lineAlign_ = style::TextAlign::Left;
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
    if (x_ == minX) lineAlign_ = style.textAlign;
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
        lineBegin_ = items_.size();
        return;
    }

    std::wstring link = parentLink;
    if (tag == "a") {
        const auto href = node.Attribute("href");
        if (!href.empty()) link = ac::url::Resolve(pageUrl_, ac::url::ToWide(href));
    }
    if (tag == "img") {
        const auto alt = node.Attribute("alt");
        const auto source = ac::url::Resolve(pageUrl_,
            ac::url::ToWide(node.Attribute("src")));
        const auto* bitmap = images_ ? images_->Find(source) : nullptr;
        if (!bitmap) {
            AppendText(dc, alt.empty() ? L"[Imagen]" :
                L"[Imagen: " + ac::url::ToWide(alt) + L"]", computed, link);
            return;
        }
        if (x_ > left_ + listDepth_ * 22) NewLine();
        const int available = std::max(80, right_ - left_ - listDepth_ * 22);
        int desiredWidth = bitmap->width;
        const auto widthAttr = node.Attribute("width");
        if (!widthAttr.empty()) {
            const long proposed = std::strtol(widthAttr.c_str(), nullptr, 10);
            if (proposed > 0) desiredWidth = static_cast<int>(std::min(proposed, 4096L));
        }
        const int imageWidth = std::min(available, desiredWidth);
        const int imageHeight = std::max(1, static_cast<int>(
            std::round(static_cast<double>(bitmap->height) * imageWidth / bitmap->width)));
        Item image;
        image.kind = Item::Kind::Image;
        image.imageUrl = source;
        image.link = link;
        int imageX = x_;
        if (computed.textAlign == style::TextAlign::Center) {
            imageX = left_ + listDepth_ * 22 + (available - imageWidth) / 2;
        } else if (computed.textAlign == style::TextAlign::Right) {
            imageX = left_ + listDepth_ * 22 + available - imageWidth;
        }
        image.rect = {imageX, y_, imageX + imageWidth, y_ + imageHeight};
        items_.push_back(std::move(image));
        y_ += imageHeight + 12;
        x_ = left_ + listDepth_ * 22;
        lineBegin_ = items_.size();
        lineHeight_ = 24;
        return;
    }
    if (tag == "input" || tag == "textarea" || tag == "button" ||
        tag == "select") {
        const std::string type = node.Attribute("type");
        if (type == "hidden" || node.attributes.contains("hidden") ||
            node.attributes.contains("disabled")) return;

        Control control{};
        control.node = &node;
        if (tag == "textarea") control.kind = Control::Kind::Textarea;
        else if (tag == "select") control.kind = Control::Kind::Select;
        else if (tag == "button") {
            control.kind = type == "reset" ? Control::Kind::Reset :
                           type == "button" ? Control::Kind::Button :
                           Control::Kind::Submit;
        } else if (type == "submit" || type == "image") {
            control.kind = Control::Kind::Submit;
        } else if (type == "button") {
            control.kind = Control::Kind::Button;
        } else if (type == "reset") {
            control.kind = Control::Kind::Reset;
        } else if (type == "password") {
            control.kind = Control::Kind::Password;
        } else if (type == "checkbox") {
            control.kind = Control::Kind::Checkbox;
        } else if (type == "radio") {
            control.kind = Control::Kind::Radio;
        } else {
            control.kind = Control::Kind::Text;
        }

        const bool small = control.kind == Control::Kind::Checkbox ||
                           control.kind == Control::Kind::Radio;
        const bool button = control.kind == Control::Kind::Submit ||
                            control.kind == Control::Kind::Button ||
                            control.kind == Control::Kind::Reset;
        int width = small ? 26 : button ? 96 : 
                    control.kind == Control::Kind::Textarea ? 320 : 230;
        const std::string widthAttr = node.Attribute("size");
        if (!widthAttr.empty() && !small && !button) {
            const long chars = std::strtol(widthAttr.c_str(), nullptr, 10);
            if (chars > 0) width = static_cast<int>(std::clamp(chars, 4L, 80L)) * 9 + 20;
        }
        if (computed.widthPixels > 0) width = computed.widthPixels;
        else if (computed.widthPercent > 0)
            width = std::max(55, (right_-left_) * computed.widthPercent / 100);
        width = std::clamp(width, small ? 24 : 50, std::max(50,right_-left_));
        const int height = control.kind == Control::Kind::Textarea ? 84 :
                           control.kind == Control::Kind::Select ? 32 : 30;
        if (x_ + width > right_ && x_ > left_) NewLine();
        const int available = std::max(1,right_-x_);
        width = std::min(width,available);
        control.rect = {x_,y_,x_+width,y_+height};
        controls_.push_back(control);
        x_ += width + (small ? 3 : 9);
        lineHeight_ = std::max(lineHeight_,height+8);
        if (control.kind == Control::Kind::Textarea) NewLine();
        return;
    }

    if (computed.block) {
        if (x_ > left_ + listDepth_ * 22) NewLine();

        const int oldLeft = left_;
        const int oldRight = right_;
        const int usableLeft = left_ + listDepth_ * 22;
        const int usableWidth = std::max(1, right_ - usableLeft);
        const int marginL = std::max(0, computed.marginLeft);
        const int marginR = std::max(0, computed.marginRight);
        const int paddingL = computed.paddingLeft;
        const int paddingR = computed.paddingRight;
        const int paddingH = paddingL + paddingR;
        int contentWidth = std::max(1, usableWidth - marginL - marginR - paddingH);
        if (computed.widthPixels >= 0)
            contentWidth = std::min(contentWidth, computed.widthPixels);
        else if (computed.widthPercent >= 0)
            contentWidth = std::max(1, (usableWidth - paddingH) *
                               computed.widthPercent / 100);
        if (computed.maxWidthPixels >= 0)
            contentWidth = std::min(contentWidth, computed.maxWidthPixels);
        contentWidth = std::max(1, std::min(contentWidth, usableWidth));
        const int boxWidth = contentWidth + paddingH;
        const int freeRoom = std::max(0, usableWidth - marginL - marginR - boxWidth);
        int boxLeft = usableLeft + marginL;
        if (computed.marginLeft < 0 && computed.marginRight < 0)
            boxLeft += freeRoom / 2;
        else if (computed.marginLeft < 0)
            boxLeft += freeRoom;
        const int boxRight = boxLeft + boxWidth;

        y_ += computed.marginTop;
        const int boxTop = y_;
        const size_t backgroundIndex = items_.size();
        if (computed.background != CLR_INVALID) {
            Item background;
            background.kind = Item::Kind::Background;
            background.color = computed.background;
            items_.push_back(std::move(background));
        }

        left_ = boxLeft + paddingL;
        right_ = std::max(left_ + 1, boxRight - paddingR);
        x_ = left_;
        y_ += computed.paddingTop;
        lineBegin_ = items_.size();
        lineAlign_ = style::TextAlign::Left;

        const bool list = IsList(tag);
        if (list) ++listDepth_;
        if (tag == "li") AppendText(dc, L"\u2022 ", computed, link);
        if (computed.layoutMode == style::LayoutMode::FlexRow ||
            computed.layoutMode == style::LayoutMode::Grid) {
            std::vector<const html::Node*> children;
            for (const auto& child : node.children) {
                if (child->type == html::Node::Type::Text) {
                    bool whitespace = true;
                    for (unsigned char c : child->text)
                        if (c > 32) { whitespace = false; break; }
                    if (whitespace) continue;
                }
                children.push_back(child.get());
            }
            const int columns = computed.layoutMode == style::LayoutMode::Grid ?
                computed.gridColumns :
                std::max(1, static_cast<int>(children.size()));
            const int cols = std::clamp(columns, 1, 12);
            const int gap = std::max(0, computed.gapPixels);
            const int parentLeft = left_;
            const int parentRight = right_;
            const int rowStart = y_;
            int rowBottom = rowStart;
            const int trackWidth = std::max(1,
                (parentRight - parentLeft - gap * (cols - 1)) / cols);
            for (size_t i = 0; i < children.size(); ++i) {
                const int column = static_cast<int>(i % static_cast<size_t>(cols));
                if (column == 0 && i != 0) {
                    y_ = rowBottom + gap;
                    rowBottom = y_;
                }
                const int cellLeft = parentLeft + column * (trackWidth + gap);
                left_ = cellLeft;
                right_ = std::min(parentRight, cellLeft + trackWidth);
                x_ = left_;
                lineBegin_ = items_.size();
                lineAlign_ = style::TextAlign::Left;
                const int originalY = y_;
                Walk(dc, *children[i], computed, link);
                if (x_ > left_) NewLine();
                rowBottom = std::max(rowBottom, y_);
                y_ = originalY;
            }
            y_ = rowBottom;
            left_ = parentLeft;
            right_ = parentRight;
            x_ = left_;
            lineBegin_ = items_.size();
            lineHeight_ = 24;
        } else {
            for (const auto& child : node.children) Walk(dc, *child, computed, link);
        }
        if (list) listDepth_ = std::max(0, listDepth_ - 1);

        if (x_ > left_ + listDepth_ * 22) NewLine();
        AlignLine();
        y_ = std::max(y_ + computed.paddingBottom,
                      boxTop + computed.minHeightPixels);
        if (computed.background != CLR_INVALID)
            items_[backgroundIndex].rect = {boxLeft, boxTop, boxRight, y_};
        y_ += computed.marginBottom;

        left_ = oldLeft;
        right_ = oldRight;
        x_ = left_ + listDepth_ * 22;
        lineBegin_ = items_.size();
        lineHeight_ = 24;
        lineAlign_ = style::TextAlign::Left;
    } else {
        for (const auto& child : node.children) Walk(dc, *child, computed, link);
    }
}

void PageLayout::Reflow(HDC dc, const html::Document& document,
                        const style::Sheet& sheet, const image::Store& images,
                        const std::wstring& pageUrl, int viewportWidth) {
    items_.clear();
    controls_.clear();
    sheet_ = &sheet;
    images_ = &images;
    pageUrl_ = pageUrl;
    left_ = 22;
    right_ = std::max(left_ + 80, viewportWidth - 24);
    x_ = left_;
    y_ = 18;
    lineHeight_ = 24;
    listDepth_ = 0;
    lineBegin_ = 0;
    lineAlign_ = style::TextAlign::Left;
    const style::Computed initial{};
    if (document.root) Walk(dc, *document.root, initial, {});
    AlignLine();
    height_ = y_ + lineHeight_ + 24;
}

bool PageLayout::HasVisibleContent() const {
    if (!controls_.empty()) return true;
    for (const Item& item : items_) {
        if (item.kind == Item::Kind::Image ||
            item.kind == Item::Kind::Rule ||
            (item.kind == Item::Kind::Text && !item.text.empty())) {
            return true;
        }
    }
    return false;
}

void PageLayout::Draw(HDC dc, int top, int scroll, const RECT& viewport) {
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    SetBkMode(dc, TRANSPARENT);
    // idTech-derived screen rectangles now cull offscreen draw calls.
    // The GDI rasterizer remains provisional; this is not Doom's GPU backend.
    const tech::idScreenRect visible = tech::idScreenRect::FromHalfOpen(
        viewport.left, viewport.top, viewport.right, viewport.bottom);
    for (const Item& item : items_) {
        const int y = item.rect.top + top - scroll;
        const int bottom = item.rect.bottom + top - scroll;
        auto bounds = tech::idScreenRect::FromHalfOpen(
            item.rect.left, y, item.rect.right, bottom);
        bounds.Intersect(visible);
        if (bounds.IsEmpty()) continue;
        if (item.kind == Item::Kind::Background) {
            RECT fill{item.rect.left, y, item.rect.right, bottom};
            HBRUSH brush = CreateSolidBrush(item.color);
            FillRect(dc, &fill, brush);
            DeleteObject(brush);
        } else if (item.kind == Item::Kind::Image) {
            const auto* bitmap = images_ ? images_->Find(item.imageUrl) : nullptr;
            if (!bitmap) continue;
            HDC memory = CreateCompatibleDC(dc);
            if (!memory) continue;
            HGDIOBJ old = SelectObject(memory, bitmap->handle);
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, item.rect.left, y,
                item.rect.right - item.rect.left,
                item.rect.bottom - item.rect.top,
                memory, 0, 0, bitmap->width, bitmap->height, blend);
            SelectObject(memory, old);
            DeleteDC(memory);
        } else if (item.kind == Item::Kind::Rule) {
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
        const auto bounds = tech::idScreenRect::FromHalfOpen(
            item.rect.left, item.rect.top, item.rect.right, item.rect.bottom);
        if (!item.link.empty() && bounds.Contains(x, contentY)) return item.link;
    }
    return {};
}
} // namespace ac::layout
