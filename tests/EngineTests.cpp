#include "html/HtmlParser.h"
#include "html/TextEncoding.h"
#include "script/ScriptRuntime.h"
#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include <unordered_map>
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

    // CSS custom properties inherited from :root and overridden locally.
    auto variableDocument = ac::html::Parse(
        "<html><body><div class='panel'><p class='text'>value</p></div></body></html>");
    ac::style::Sheet variables;
    variables.Parse(":root { --brand: #123456; } "
                    ".panel { color: var(--brand); --brand: #334455; } "
                    ".text { color: var(--brand, red); }");
    const auto* variableRoot = Find(*variableDocument.root,"html");
    const auto* variablePanel = Find(*variableDocument.root,"div");
    const auto* variableText = Find(*variableDocument.root,"p");
    Check(variableRoot && variablePanel && variableText, "CSS variable fixture");
    const auto rootComputed = variables.Compute(*variableRoot,parent);
    const auto panelComputed = variables.Compute(*variablePanel,rootComputed);
    const auto textComputed = variables.Compute(*variableText,panelComputed);
    Check(rootComputed.variables.contains("--brand"),
          ":root custom properties are stored");
    Check(panelComputed.color == RGB(0x33,0x44,0x55) &&
          textComputed.color == RGB(0x33,0x44,0x55),
          "CSS variables cascade and inherit across nested nodes");

    // CSS important beats normal inline declarations; inline important wins.
    ac::style::Sheet importantSheet;
    importantSheet.Parse(".priority { color: #112233 !important; } "
                         ".priority { color: #aabbcc; }");
    auto importantDoc = ac::html::Parse(
        "<div class='priority' style='color:#445566'>Test</div>");
    const auto* priorityNode = Find(*importantDoc.root,"div");
    Check(priorityNode && importantSheet.Compute(*priorityNode,parent).color ==
          RGB(0x11,0x22,0x33), "stylesheet important overrides normal inline");
    auto inlineImportantDoc = ac::html::Parse(
        "<div class='priority' style='color:#445566 !important'>Test</div>");
    const auto* inlineImportant = Find(*inlineImportantDoc.root,"div");
    Check(inlineImportant && importantSheet.Compute(*inlineImportant,parent).color ==
          RGB(0x44,0x55,0x66), "important inline overrides stylesheet important");

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
    const auto* noscriptNode = Find(*noScript.root,"noscript");
    Check(noscriptNode && !sheet.Compute(*noscriptNode,ac::style::Computed{}).hidden,
          "noscript fallback visible with scripting disabled");
    sheet.SetScriptingEnabled(true);
    Check(sheet.Compute(*noscriptNode,ac::style::Computed{}).hidden,
          "noscript fallback hidden with scripting enabled");
    sheet.SetScriptingEnabled(false);
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
    auto external = ac::html::Parse(
        "<html><body><div id='root'></div>"
        "<script src='/bundle.js'></script></body></html>");
    const std::unordered_map<std::string,std::string> bundles{
        {"https://example.org/bundle.js",
         "document.getElementById('root').innerHTML='<p>Externo</p>';"
         "document.title='Script externo';"}
    };
    const auto extResult=ac::script::Execute(external,L"https://example.org/",bundles);
    Check(extResult.operations==2 && extResult.unsupported==0,
          "external script native DOM operations");
    Check(external.title=="Script externo", "external script title updated");
    layout.Reflow(dc,external,sheet,images,L"https://example.org/",480);
    Check(layout.HasVisibleContent(),"external script produces visible content");
    auto navigation = ac::html::Parse(
        "<html><body><script>window.location.replace('/next');</script></body></html>");
    const auto navResult = ac::script::Execute(navigation,L"https://example.org/start",{});
    Check(navResult.operations == 1 &&
          navResult.navigation == L"https://example.org/next",
          "literal script navigation resolves relative URLs");
    auto absoluteNavigation = ac::html::Parse(
        "<script>location.href='https://example.net/result';</script>");
    const auto absoluteResult = ac::script::Execute(
        absoluteNavigation,L"https://example.org/",{});
    Check(absoluteResult.navigation == L"https://example.net/result",
          "literal location href changes navigation");
    auto refused = ac::html::Parse(
        "<script>location.href='javascript:alert(1)';</script>");
    const auto refusedResult = ac::script::Execute(refused,L"https://example.org/",{});
    Check(refusedResult.navigation.empty(),
          "script location must not navigate to active URI schemes");
    DeleteDC(dc);
}

void TestNativeJavaScriptLexer() {
    const auto tokens = ac::script::js::Tokenize(
        "/* header */ const message = 'Hola\\u00f1'; // comment\n"
        "let number = 12.5; number += 2;");
    Check(tokens.size() >= 12, "native JavaScript tokenizer produces tokens");
    bool unicodeFound = false;
    for (const auto& token : tokens)
        if (token.kind == ac::script::js::TokenKind::String &&
            token.text == "Hola\xC3\xB1")
            unicodeFound = true;
    Check(unicodeFound, "native JavaScript tokenizer expands Unicode escapes");
}

void TestDomQuerySelectors() {
    auto doc = ac::html::Parse(
        "<html><body><section class='panel featured'><span class='label'>Old</span></section>"
        "<script>document.querySelector('.panel').innerHTML='<p id=\"dynamic\">Loaded</p>';"
        "document.querySelector('p').textContent='Updated';</script></body></html>");
    const auto status = ac::script::ExecuteNative(doc,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "class and tag querySelector scripts execute");
    const auto* p = Find(*doc.root,"p");
    Check(p && !p->children.empty() && p->children.front()->text == "Updated",
          "native querySelector DOM changes visible nodes");
}

void TestNativeExceptions() {
    auto document=ac::html::Parse(
        "<html><body><p id='result'></p><script>"
        "function evaluate() { try { throw 'caught'; } "
        "catch (reason) { return reason; } }"
        "document.getElementById('result').textContent = evaluate();"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported==0,
          "explicit JavaScript throw and catch execute inside functions");
    const auto* node=Find(*document.root,"p");
    Check(node && !node->children.empty() &&
          node->children.front()->text=="caught",
          "caught JavaScript exception returns its value");
}

void TestNativeLoopControl() {
    auto document=ac::html::Parse(
        "<html><body><p id='result'>Before</p><script>"
        "let count = 0; let score = 0;"
        "while (count < 9) { count++; if (count === 2) continue;"
        "if (count === 5) break; score += count; }"
        "for (let i=0; i<10; i++) { if (i===2) continue;"
        "if (i===4) break; score += i; }"
        "document.getElementById('result').textContent = String(score);"
        "</script></body></html>");
    const auto state=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(state.engineReady && state.unsupported==0,
          "native break/continue loops terminate correctly");
    const auto* node=Find(*document.root,"p");
    Check(node && !node->children.empty() && node->children.front()->text=="12",
          "while and for loop break/continue semantics");
}

void TestNativeStandardBuiltins() {
    auto document=ac::html::Parse(
        "<html><body><p id='result'></p><script>"
        "const amount = parseInt('21px') * 2;"
        "const total = Math.max(amount, Math.floor(41.9));"
        "const ok = typeof total === 'number' && Boolean(total);"
        "if (ok) document.getElementById('result').textContent = String(total);"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "native conversion functions and Math methods execute");
    const auto* p=Find(*document.root,"p");
    Check(p && !p->children.empty() && p->children.front()->text=="42",
          "Math.max and parseInt calculate correct result");
}

void TestNativePrimitiveSemantics() {
    auto document=ac::html::Parse(
        "<html><body><p id='result'></p><script>"
        "const strict = null === undefined;"
        "const loose = null == undefined;"
        "const different = '1' === 1;"
        "const missing = document.getElementById('does-not-exist');"
        "if (!strict && loose && !different && !missing) "
        "document.getElementById('result').textContent = 'PASS';"
        "else document.getElementById('result').textContent = 'FAIL';"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "JavaScript primitive identity fixture parses");
    const auto* node=Find(*document.root,"p");
    Check(node && !node->children.empty() &&
          node->children.front()->text=="PASS",
          "null and undefined equality distinguish strict and loose");
}

void TestNativeCollections() {
    auto document = ac::html::Parse(
        "<html><body><p class='item'>One</p><p class='item'>Two</p><script>"
        "const elements = document.querySelectorAll('.item');"
        "const numbers = [4, 5]; numbers.push(6);"
        "elements[1].textContent = numbers.join('-');"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "DOM node collections and mutable array operations execute");
    const auto* body=Find(*document.root,"body");
    Check(body && body->children.size()>=2, "DOM collection fixture body");
    const auto* second=body->children[1].get();
    Check(second->tag=="p" && !second->children.empty() &&
          second->children.front()->text=="4-5-6",
          "indexed DOM collection references update matching element");
}

void TestNativeObjects() {
    auto document = ac::html::Parse(
        "<html><body><p id='result'>Before</p><script>"
        "const config = { label: 'Score', points: 7 };"
        "config.points = config.points * 6;"
        "config['label'] = 'Answer';"
        "document.getElementById('result').textContent = "
        "config.label + ': ' + config.points;"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "native object literals, property access and indexed mutation");
    const auto* p=Find(*document.root,"p");
    Check(p && !p->children.empty() && p->children.front()->text=="Answer: 42",
          "computed native object fields update the DOM");
}

void TestNativeFunctions() {
    auto document = ac::html::Parse(
        "<html><body><p id='result'></p><script>"
        "function multiply(a, b) { return a * b; }"
        "function decorate(n) { return 'Product: ' + multiply(n, 7); }"
        "document.getElementById('result').textContent = decorate(6);"
        "</script></body></html>");
    const auto status = ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "native JavaScript executes named functions and nested calls");
    const auto* result = Find(*document.root,"p");
    Check(result && !result->children.empty() &&
          result->children.front()->text == "Product: 42",
          "JavaScript function return values update the DOM");
}

void TestNativeControlFlow() {
    auto doc = ac::html::Parse(
        "<html><body><p id='result'>Before</p><script>"
        "let total = 0; let count = 0;"
        "while (count < 4) { count++; total += count; }"
        "if (total === 10) document.getElementById('result').textContent = 'OK';"
        "else document.getElementById('result').textContent = 'FAIL';"
        "</script></body></html>");
    const auto state = ac::script::ExecuteNative(doc,L"https://example.org/",{});
    Check(state.engineReady && state.unsupported == 0, "native while and if executed");
    const auto* node = Find(*doc.root,"p");
    Check(node && !node->children.empty() && node->children.front()->text=="OK",
          "bounded JavaScript control flow updates the DOM");
}

void TestNativeDomInsertion() {
    auto doc = ac::html::Parse(
        "<html><body><main id='app'></main>"
        "<script>"
        "const parent = document.getElementById('app');"
        "const child = document.createElement('p');"
        "child.setAttribute('id', 'generated');"
        "child.textContent = 'DOM created natively';"
        "parent.appendChild(child);"
        "</script></body></html>");
    const auto status=ac::script::ExecuteNative(doc,L"https://example.org/",{});
    Check(status.engineReady && status.unsupported == 0,
          "native DOM creation script executes");
    const auto* child=Find(*doc.root,"p");
    Check(child && child->Attribute("id")=="generated" &&
          !child->children.empty() &&
          child->children.front()->text=="DOM created natively",
          "createElement and appendChild produce owned DOM nodes");
}

void TestDomElementReferences() {
    auto document = ac::html::Parse(
        "<html><body><div id='result'></div>"
        "<script>const element = document.getElementById('result');"
        "let answer = 6 * 7;"
        "element.innerHTML = '<p>Answer: ' + answer + '</p>';"
        "</script></body></html>");
    auto result = ac::script::ExecuteNative(document,L"https://example.org/",{});
    Check(result.engineReady && result.unsupported == 0,
          "native JavaScript supports DOM references held in variables");
    const auto* paragraph = Find(*document.root,"p");
    Check(paragraph && !paragraph->children.empty() &&
          paragraph->children.front()->text == "Answer: 42",
          "DOM property setter resolves element reference");
}

void TestEmbeddedJavascript() {
    auto document = ac::html::Parse(
        "<html><head><title>Initial</title></head><body>"
        "<div id='output'></div><script>"
        "let sum = 0; for (let i = 1; i <= 5; ++i) sum += i;"
        "document.getElementById('output').textContent = 'Total ' + sum;"
        "document.title = 'ECMAScript OK';"
        "</script></body></html>");
    auto result = ac::script::ExecuteNative(document, L"https://example.org/", {});
    Check(result.scripts == 1 && result.unsupported == 0,
          "embedded ECMAScript executes arithmetic, loops and DOM bindings");
    Check(result.engineReady, "embedded engine must initialize before scripts execute");
    Check(document.title == "ECMAScript OK",
          "embedded ECMAScript modifies document title");
    const auto* output = Find(*document.root, "div");
    Check(output && !output->children.empty() &&
          output->children.front()->text == "Total 15",
          "embedded ECMAScript populates DOM with computed data");
    auto next = ac::html::Parse(
        "<html><body><script>"
        "const paths = ['/old','/new'];"
        "window.location.assign(paths[1]);"
        "</script></body></html>");
    const auto navigation = ac::script::ExecuteNative(
        next, L"https://example.org/index", {});
    Check(navigation.navigation == L"https://example.org/new",
          "computed JavaScript location navigation resolves");
    auto malicious = ac::html::Parse(
        "<script>location.href = 'javascript:alert(1)'</script>");
    const auto blocked = ac::script::ExecuteNative(
        malicious, L"https://example.org/", {});
    Check(blocked.navigation.empty(),
          "embedded ECMAScript rejects non-web navigation schemes");
}

void TestHtmlEncoding() {
    const std::string latin1="<html><body>Espa\xF1ol</body></html>";
    const auto converted=ac::html::DecodeToUtf8(latin1,L"text/html; charset=ISO-8859-1");
    Check(converted.find("Espa\xC3\xB1ol")!=std::string::npos,
          "Latin1 HTML must decode to UTF-8");
    const std::string cp1252="<meta charset='windows-1252'>\x80";
    Check(ac::html::DecodeToUtf8(cp1252).find("\xE2\x82\xAC")!=std::string::npos,
          "meta charset windows-1252 resolves euro symbol");
    const std::string laterMeta="<meta name='viewport' content='width=device-width'>"
        "<meta charset='windows-1252'>\x80";
    Check(ac::html::DecodeToUtf8(laterMeta).find("\xE2\x82\xAC")!=std::string::npos,
          "charset sniff searches past unrelated meta tags");
    const std::string utf16(std::string("\xFF\xFE") + std::string("H\0i\0",4));
    Check(ac::html::DecodeToUtf8(utf16)=="Hi",
          "UTF-16LE BOM decoding");
    Check(ac::html::DecodeToUtf8("La ciudad")=="La ciudad",
          "UTF-8 no-op decoding");
}

void TestDisplayCascade() {
    auto document = ac::html::Parse(
        "<html><body><div id='one' class='show'>VISIBLE</div>"
        "<div id='two' class='no'>OCULTO</div></body></html>");
    ac::style::Sheet sheet;
    sheet.Parse("div { display:none; } div.show { display:block; }");
    const auto* one = Find(*document.root,"div");
    Check(one && one->parent, "display override fixture");
    ac::style::Computed parent;
    const auto computed=sheet.Compute(*one,parent);
    Check(!computed.hidden && computed.block,
          "later display:block must override earlier display:none");
    auto more = ac::html::Parse(
        "<html><body><div style='display:none;display:block'>Visible</div></body></html>");
    const auto* inlineNode=Find(*more.root,"div");
    const auto inlineStyle=sheet.Compute(*inlineNode,parent);
    Check(!inlineStyle.hidden,"later inline display declaration must win");
    auto concealed=ac::html::Parse(
        "<html><body><div style='display:none'>Hidden</div></body></html>");
    const auto* hidden=Find(*concealed.root,"div");
    Check(sheet.Compute(*hidden,parent).hidden,
          "display none remains hidden without an override");
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
        TestHtmlEncoding();
        TestDisplayCascade();
        TestUrls();
        TestScriptDom();
        TestEmbeddedJavascript();
        TestDomQuerySelectors();
        TestNativeJavaScriptLexer();
        TestDomElementReferences();
        TestNativeDomInsertion();
        TestNativeControlFlow();
        TestNativeFunctions();
        TestNativeObjects();
        TestNativeCollections();
        TestNativePrimitiveSemantics();
        TestNativeStandardBuiltins();
        TestNativeLoopControl();
        TestNativeExceptions();
        TestScreenRects();
        TestLayoutGeometry();
        std::cout << "HTML, CSS y URL: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fallo: " << error.what() << '\n';
        return 1;
    }
}
