#include "html/HtmlParser.h"
#include "style/Style.h"
#include "util/Url.h"
#include "tech/IdScreenRect.h"
#include "layout/Layout.h"
#include "image/ImageStore.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
const ac::html::Node* Find(const ac::html::Node& node, std::string_view tag) {
    if (node.tag == tag) return &node;
    for (const auto& child : node.children) {
        if (const auto* result = Find(*child, tag)) return result;
    }
    return nullptr;
}
void TestHtml() {
    const auto document = ac::html::Parse(R"HTML(
        <!doctype html>
        <html><head><title>Prueba &amp; C++</title>
        <style>.destacado { color: #204060; }</style></head>
        <body><h1>Hola &#241;and&#250;</h1>
        <p><a href="/ruta">Ir</a><span class="destacado">Texto</span></p>
        <script>window.alert('no ejecutar');</script>
        </body></html>
    )HTML");
    Check(document.title == "Prueba & C++", "HTML title / entity decode");
    Check(document.embeddedCss.find(".destacado") != std::string::npos, "embedded CSS");
    const auto* link = Find(*document.root, "a");
    Check(link != nullptr, "anchor exists");
    Check(link->Attribute("href") == "/ruta", "anchor href");
    Check(link->parent != nullptr && link->parent->tag == "p", "open-tag stack");
    const auto* heading = Find(*document.root, "h1");
    Check(heading && !heading->children.empty(), "heading exists");
    Check(heading->children.front()->text == "Hola ñandú", "numeric UTF-8 entities");
    const auto* script = Find(*document.root, "script");
    Check(script != nullptr, "script is parsed but never executed");
}
void TestStyles() {
    auto document = ac::html::Parse(
        "<div><span class='destacado'>texto</span><a href='/x'>otro</a></div>");
    ac::style::Sheet sheet;
    sheet.Parse(".destacado { color: #204060; } a { font-weight: bold; }");
    const auto* span = Find(*document.root, "span");
    const auto* anchor = Find(*document.root, "a");
    Check(span && anchor, "style test nodes");
    const ac::style::Computed parent{};
    const auto styled = sheet.Compute(*span, parent);
    Check(styled.color == RGB(0x20,0x40,0x60), "simple CSS class selector");
    const auto link = sheet.Compute(*anchor, parent);
    Check(link.bold && link.underline, "link style and CSS font weight");

    auto hidden = ac::html::Parse(
        "<form><input type='hidden' value='INTERNAL_TOKEN'>"
        "<input type='HIDDEN' value='INTERNAL_TOKEN_2'>"
        "<div hidden>SECRET</div>"
        "<section style='display:none'>NOT_VISIBLE</section>"
        "<aside style='visibility:hidden'>NOT_VISIBLE_2</aside>"
        "<input type='text' placeholder='Buscar'></form>");
    auto* input = Find(*hidden.root, "input");
    auto* div = Find(*hidden.root, "div");
    auto* section = Find(*hidden.root, "section");
    auto* aside = Find(*hidden.root, "aside");
    Check(input && div && section && aside, "hidden fixture nodes");
    Check(sheet.Compute(*input, parent).hidden, "hidden input must not render");
    Check(sheet.Compute(*div, parent).hidden, "boolean hidden attribute must hide");
    Check(sheet.Compute(*section, parent).hidden, "CSS display:none must hide");
    Check(sheet.Compute(*aside, parent).hidden, "CSS visibility:hidden must hide");

    const auto secondInput = Find(*hidden.root->children.front(), "input");
    (void)secondInput;
    const auto form = Find(*hidden.root, "form");
    Check(form && form->children.size() == 6, "form fixture has six controls");
    Check(sheet.Compute(*form->children[1], parent).hidden,
          "input type case-insensitive hidden");
    Check(!sheet.Compute(*form->children[5], parent).hidden,
          "normal input remains visible");
    auto center = ac::html::Parse(
        "<center><p>Centered heading</p></center>");
    const auto* centered = Find(*center.root, "center");
    Check(centered != nullptr, "center element exists");
    const auto centerStyle = sheet.Compute(*centered, parent);
    Check(centerStyle.block && centerStyle.textAlign == ac::style::TextAlign::Center,
          "HTML center element produces a centered block");
    const auto* centerParagraph = Find(*center.root, "p");
    Check(centerParagraph &&
          sheet.Compute(*centerParagraph, centerStyle).textAlign ==
              ac::style::TextAlign::Center,
          "text alignment is inherited");
    auto styledCenter = ac::html::Parse("<p style='text-align:right'>Right</p>");
    const auto* p = Find(*styledCenter.root, "p");
    Check(p && sheet.Compute(*p, parent).textAlign ==
        ac::style::TextAlign::Right, "CSS text-align:right");
}
void TestScreenRects() {
    using ac::tech::idScreenRect;
    auto one = idScreenRect::FromHalfOpen(10, 20, 30, 40);
    Check(one.Width() == 20 && one.Height() == 20, "screen rectangle size");
    Check(one.Contains(10, 20) && one.Contains(29, 39), "inclusive pixel bounds");
    Check(!one.Contains(30, 40), "right and bottom boundaries exclusive");
    one.Intersect(idScreenRect::FromHalfOpen(20, 30, 40, 50));
    Check(one.Width() == 10 && one.Height() == 10, "renderer clipping");
    one.Intersect(idScreenRect::FromHalfOpen(45, 45, 60, 60));
    Check(one.IsEmpty(), "non-overlapping rectangles are empty");
}


void TestLayoutGeometry() {
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "create memory DC");
    ac::style::Sheet sheet;
    ac::image::Store images;
    ac::layout::PageLayout layout;
    constexpr wchar_t url[] = L"https://example.org/";
    auto aligned = ac::html::Parse(
        "<center><a href='/link'>ANCLACENTRADA</a></center>");
    layout.Reflow(dc, aligned, sheet, images, url, 400);
    Check(layout.LinkAt(200, 24, 0, 0) == L"https://example.org/link",
          "centered text hyperlink has correct hit rectangle");
    Check(layout.LinkAt(25, 24, 0, 0).empty(),
          "centered element is not incorrectly left aligned");

    auto onlyVisible = ac::html::Parse("<p>VISIBLE</p>");
    layout.Reflow(dc, onlyVisible, sheet, images, url, 400);
    const int visibleHeight = layout.Height();

    auto withHidden = ac::html::Parse(
        "<input type='hidden' value='DO_NOT_DISPLAY'>"
        "<div hidden>INTERNAL</div>"
        "<section style='display:none'>INTERNAL_2</section>"
        "<p>VISIBLE</p>");
    layout.Reflow(dc, withHidden, sheet, images, url, 400);
    Check(layout.Height() == visibleHeight,
          "hidden elements must not affect rendered height");
    DeleteDC(dc);
}

void TestUrls() {
    Check(ac::url::Normalize(L"example.org") == L"https://example.org",
          "address normalization");
    Check(ac::url::Normalize(L"mi búsqueda") == L"",
          "search text is not redirected to an external provider");
    Check(ac::url::Normalize(L"palabra") == L"",
          "single-word input is rejected instead of redirected");
    Check(ac::url::Normalize(L"javascript:alert(1)") == L"",
          "unsupported active URL scheme is rejected");
    Check(ac::url::Resolve(L"https://example.org/docs/index.html", L"../about") ==
          L"https://example.org/about", "relative URL resolution");
    Check(ac::url::IsWeb(L"https://example.org"), "HTTPS supported");
    Check(!ac::url::IsWeb(L"javascript:alert(1)"), "active URL scheme refused");
    Check(ac::url::ToWide(ac::url::ToUtf8(L"Puerto \u00C1mbar")) ==
          L"Puerto \u00C1mbar", "UTF-8 roundtrip");
}
} // namespace

int main() {
    try {
        TestHtml();
        TestStyles();
        TestUrls();
        TestScreenRects();
        TestLayoutGeometry();
        std::cout << "HTML, CSS y URL: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fallo: " << error.what() << '\n';
        return 1;
    }
}
