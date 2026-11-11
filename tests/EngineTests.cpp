#include "html/HtmlParser.h"
#include "script/ScriptRuntime.h"
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
    // A conforming HTML5 page may omit </head>; its body must still be visible.
    const auto implicitHead = ac::html::Parse(
        "<html><head><title>Sin cierre de head</title>"
        "<body><h1>Contenido real</h1></body></html>");
    const auto* body = Find(*implicitHead.root, "body");
    Check(body && body->parent && body->parent->tag == "html",
          "body implicitly closes head");
    const auto paragraphs = ac::html::Parse(
        "<html><body><p>Uno<p>Dos</body></html>");
    const auto* documentBody = Find(*paragraphs.root, "body");
    Check(documentBody && documentBody->children.size() == 2 &&
          documentBody->children[0]->tag == "p" &&
          documentBody->children[1]->tag == "p",
          "implicit paragraph closing");
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
    auto boxDocument = ac::html::Parse(
        "<main style='max-width:700px;margin:20px auto;"
        "padding:10px 25px;background:#123456'>"
        "<p style='width:50%;padding-left:6px'>text</p></main>");
    const auto* box = Find(*boxDocument.root, "main");
    const auto* child = Find(*boxDocument.root, "p");
    Check(box && child, "box model fixtures");
    const auto metrics = sheet.Compute(*box, parent);
    Check(metrics.maxWidthPixels == 700 && metrics.marginLeft == -1 &&
          metrics.marginRight == -1 && metrics.paddingLeft == 25 &&
          metrics.paddingRight == 25 && metrics.paddingTop == 10 &&
          metrics.background == RGB(0x12,0x34,0x56), "CSS box metrics");
    const auto inside = sheet.Compute(*child, metrics);
    Check(inside.widthPercent == 50 && inside.paddingLeft == 6 &&
          inside.maxWidthPixels == -1, "non inherited box metrics");
    auto cards = ac::html::Parse(
        "<nav style='display:flex;gap:12px'>"
        "<div><a href='/left'>LEFTLINK</a></div>"
        "<div><a href='/right'>RIGHTLINK</a></div></nav>");
    const auto* nav = Find(*cards.root, "nav");
    Check(nav != nullptr, "flex fixture exists");
    const auto flexStyle = sheet.Compute(*nav, parent);
    Check(flexStyle.layoutMode == ac::style::LayoutMode::FlexRow &&
          flexStyle.gapPixels == 12, "flex layout and gap parsed");
    ac::style::Sheet gridStyle;
    gridStyle.Parse("nav {display:grid; grid-template-columns:repeat(3,1fr);}");
    // The existing nav has inline display:flex; the inline style must win.
    Check(gridStyle.Compute(*nav, parent).layoutMode ==
          ac::style::LayoutMode::FlexRow,
          "inline display wins over stylesheet display");
    const auto gridDocument = ac::html::Parse("<nav><div>Card</div></nav>");
    const auto* gridNode = Find(*gridDocument.root, "nav");
    Check(gridNode != nullptr, "grid fixture exists");
    const auto grid = gridStyle.Compute(*gridNode, parent);
    Check(grid.layoutMode == ac::style::LayoutMode::Grid &&
          grid.gridColumns == 3, "grid track count parsed");

    auto nested = ac::html::Parse(
        "<main id='app'><div class='panel'><p class='title'>Hello</p></div></main>");
    const auto* inner = Find(*nested.root, "p");
    const auto* wrapper = Find(*nested.root, "div");
    Check(inner && wrapper, "nested selector nodes");
    ac::style::Sheet cascade;
    cascade.Parse("#app .title {color:#123456;} "
                  "main > div {font-weight:bold;} "
                  ".panel > p {text-align:center;} "
                  "p {color:red;} "
                  "main > p {display:none;}");
    const auto result = cascade.Compute(*inner, parent);
    Check(result.color == RGB(0x12,0x34,0x56),
          "higher specificity beats later tag rule");
    Check(result.textAlign == ac::style::TextAlign::Center,
          "direct-child selector works");
    Check(!result.hidden, "nonmatching direct child must not hide");
    Check(cascade.Compute(*wrapper, parent).bold,
          "direct-child style on div");
    ac::style::Sheet responsive;
    responsive.Parse("@media screen and (min-width:600px) {"
                     ".panel {color:#204060;} }"
                     "@media screen and (max-width:500px) {"
                     ".panel {color:#aabbcc;} }", 800);
    Check(responsive.Compute(*wrapper, parent).color ==
          RGB(0x20,0x40,0x60), "desktop media query");
    responsive.Parse("@media screen and (min-width:600px) {"
                     ".panel {color:#204060;} }"
                     "@media screen and (max-width:500px) {"
                     ".panel {color:#aabbcc;} }", 400);
    Check(responsive.Compute(*wrapper, parent).color ==
          RGB(0xaa,0xbb,0xcc), "mobile media query");
    responsive.Parse("@layer components { .panel {font-size:20px;} }", 800);
    Check(responsive.Compute(*wrapper, parent).fontSize == 20,
          "nested stylesheet layer");
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
    auto jsOnly = ac::html::Parse(
        "<html><head><title>JS Only</title></head>"
        "<body><div id='app'></div><script>window.render();</script></body></html>");
    layout.Reflow(dc, jsOnly, sheet, images, url, 400);
    Check(!layout.HasVisibleContent(),
          "JS-only empty document must be detected");
    auto visible = ac::html::Parse(
        "<html><body><h1>Contenido visible</h1></body></html>");
    layout.Reflow(dc, visible, sheet, images, url, 400);
    Check(layout.HasVisibleContent(),
          "visible HTML content must not be treated as blank");
    // HTML5 documents may omit </head> and <body>. The first actual body
    // element must not remain inside a hidden HEAD subtree.
    auto implicitBody = ac::html::Parse(
        "<!doctype html><html><head><title>Sin cerrar</title>"
        "<meta charset='utf-8'><main><h1>Texto visible</h1></main></html>");
    layout.Reflow(dc, implicitBody, sheet, images, url, 400);
    Check(layout.HasVisibleContent(),
          "implicit close of HEAD must preserve main content");
    auto malformedDiv = ac::html::Parse(
        "<html><head><title>Sin cerrar</title><div>Visible</div>");
    layout.Reflow(dc, malformedDiv, sheet, images, url, 400);
    Check(layout.HasVisibleContent(),
          "missing body/head close cannot hide all content");
    auto noScript = ac::html::Parse(
        "<html><body><noscript>Este sitio requiere JavaScript.</noscript>"
        "</body></html>");
    layout.Reflow(dc, noScript, sheet, images, url, 400);
    Check(layout.HasVisibleContent(),
          "noscript content must appear when JS is not supported");
    auto centeredBox = ac::html::Parse(
        "<main style='max-width:160px;margin:0 auto;padding:10px'>"
        "<a href='/box'>centrado</a></main>");
    layout.Reflow(dc, centeredBox, sheet, images, url, 400);
    Check(layout.LinkAt(124, 32, 0, 0) == L"https://example.org/box",
          "link inside max-width centered box is correctly positioned");
    Check(layout.LinkAt(25, 32, 0, 0).empty(),
          "centered box no longer occupies the full left viewport");

    auto empty = ac::html::Parse("<div></div>");
    layout.Reflow(dc, empty, sheet, images, url, 400);
    const int noPadding = layout.Height();
    auto padded = ac::html::Parse(
        "<div style='padding:20px 12px;min-height:90px'></div>");
    layout.Reflow(dc, padded, sheet, images, url, 400);
    Check(layout.Height() >= noPadding + 60,
          "block min-height and padding affect layout geometry");
    auto flex = ac::html::Parse(
        "<div style='display:flex;gap:12px'>"
        "<div><a href='/a'>PRIMER</a></div>"
        "<div><a href='/b'>SEGUNDO</a></div></div>");
    layout.Reflow(dc, flex, sheet, images, url, 400);
    Check(layout.LinkAt(28, 24, 0, 0) ==
          L"https://example.org/a", "flex first column link");
    Check(layout.LinkAt(213, 24, 0, 0) ==
          L"https://example.org/b", "flex second column link");
    DeleteDC(dc);
}


void TestScriptDom() {
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc!=nullptr,"memory DC for script DOM");
    ac::style::Sheet sheet;
    ac::image::Store images;
    ac::layout::PageLayout layout;
    auto doc=ac::html::Parse(
        "<html><head><title>Antes</title></head><body>"
        "<div id='root'></div><script>"
        "document.getElementById('root').innerHTML = '<a href=\"/js\">Enlace JS</a>';"
        "document.title = 'Despues';"
        "</script></body></html>");
    auto done=ac::script::ExecuteInline(doc);
    Check(done.operations==2 && done.unsupported==0,"DOM script operations");
    Check(doc.title=="Despues","script changed title");
    sheet.Parse(doc.embeddedCss);
    layout.Reflow(dc,doc,sheet,images,L"https://example.org/",480);
    Check(layout.HasVisibleContent(),"script-generated HTML must render");
    Check(layout.LinkAt(40,25,0,0)==L"https://example.org/js",
          "script-generated hyperlink must be clickable");
    auto unsupported=ac::html::Parse(
        "<html><body><div id='app'></div>"
        "<script>fetch('/api').then(()=>window.render())</script></body></html>");
    const auto skipped=ac::script::ExecuteInline(unsupported);
    Check(skipped.operations==0 && skipped.unsupported==1,
          "unsupported JavaScript is reported, not silently claimed supported");
    layout.Reflow(dc,unsupported,sheet,images,L"https://example.org/",480);
    Check(!layout.HasVisibleContent(),"unsupported JS cannot fabricate content");
    DeleteDC(dc);
}

void TestUrls() {
    Check(ac::url::Normalize(L"example.org") == L"https://example.org",
          "address normalization");
    Check(ac::url::Normalize(L"EXAMPLE.org/path") == L"https://EXAMPLE.org/path",
          "bare domain with path");
    Check(ac::url::Normalize(L"localhost:8080/start") ==
          L"http://localhost:8080/start", "local server address");
    Check(ac::url::Normalize(L"127.0.0.1:53997/start") ==
          L"http://127.0.0.1:53997/start", "local IPv4 address");
    Check(ac::url::Normalize(L"HTTPS://example.org/hello") ==
          L"https://example.org/hello", "case-insensitive HTTPS scheme");
    Check(ac::url::Normalize(L"example.org:70000").empty(),
          "reject invalid TCP port");
    Check(ac::url::ResolveInput(L"example.org", ac::url::SearchProvider::Google) ==
          L"https://example.org", "address input takes priority over search");
    Check(ac::url::ResolveInput(L"google", ac::url::SearchProvider::Google) ==
          L"https://www.google.com/search?q=google", "bare search term uses provider");
    Check(ac::url::ResolveInput(L"gatos negros", ac::url::SearchProvider::Bing) ==
          L"https://www.bing.com/search?q=gatos+negros", "multiword search");
    Check(ac::url::ResolveInput(L"gatos negros", ac::url::SearchProvider::Disabled).empty(),
          "disabled search never redirects");
    Check(ac::url::ResolveInput(L"file://documents", ac::url::SearchProvider::Google).empty(),
          "unsupported URI scheme is never sent to search");
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
        TestScriptDom();
        TestScreenRects();
        TestLayoutGeometry();
        std::cout << "HTML, CSS y URL: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fallo: " << error.what() << '\n';
        return 1;
    }
}
