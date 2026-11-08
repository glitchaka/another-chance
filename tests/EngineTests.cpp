#include "html/HtmlParser.h"
#include "style/Style.h"
#include "util/Url.h"
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
}
void TestUrls() {
    Check(ac::url::Normalize(L"example.org") == L"https://example.org",
          "address normalization");
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
        std::cout << "HTML, CSS y URL: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fallo: " << error.what() << '\n';
        return 1;
    }
}
