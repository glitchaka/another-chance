#include "html/HtmlParser.h"
#include "tech/IdStack.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace ac::html {
namespace {
bool IsName(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == ':' || c == '_' || c == '-';
}
std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
void AppendCodepoint(std::string& result, unsigned long cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp <= 0x7F) result.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
std::string DecodeEntities(std::string_view input) {
    std::string result;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') { result += input[i]; continue; }
        const auto end = input.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 15) {
            result += '&'; continue;
        }
        const std::string name(input.substr(i + 1, end - i - 1));
        bool decoded = true;
        if (name == "amp") result += '&';
        else if (name == "lt") result += '<';
        else if (name == "gt") result += '>';
        else if (name == "quot") result += '"';
        else if (name == "apos" || name == "#39") result += '\'';
        else if (name == "nbsp") result += ' ';
        else if (name == "copy") AppendCodepoint(result, 0xA9);
        else if (name == "reg") AppendCodepoint(result, 0xAE);
        else if (name == "mdash") AppendCodepoint(result, 0x2014);
        else if (name == "ndash") AppendCodepoint(result, 0x2013);
        else if (name == "hellip") AppendCodepoint(result, 0x2026);
        else if (name == "euro") AppendCodepoint(result, 0x20AC);
        else if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 2 && (name[1] == 'x' || name[1] == 'X');
            const char* start = name.c_str() + (hex ? 2 : 1);
            char* tail = nullptr;
            const unsigned long code = std::strtoul(start, &tail, hex ? 16 : 10);
            if (start == tail || *tail != '\0' || code == 0) decoded = false;
            else AppendCodepoint(result, code);
        } else decoded = false;
        if (decoded) i = end;
        else result += '&';
    }
    return result;
}
bool IsVoid(std::string_view name) {
    return name == "area" || name == "base" || name == "br" ||
        name == "col" || name == "embed" || name == "hr" ||
        name == "img" || name == "input" || name == "link" ||
        name == "meta" || name == "param" || name == "source" ||
        name == "track" || name == "wbr";
}
void CollectTitle(const Node& node, std::string& title) {
    if (node.tag == "title") {
        for (const auto& child : node.children) {
            if (child->type == Node::Type::Text) title += child->text;
        }
    }
    for (const auto& child : node.children) CollectTitle(*child, title);
}
} // namespace

std::string Node::Attribute(std::string_view name) const {
    auto found = attributes.find(std::string(name));
    return found == attributes.end() ? std::string{} : found->second;
}

Document Parse(std::string_view input) {
    Document document;
    document.root = std::make_unique<Node>();
    document.root->tag = "document";

    const std::string lower = Lower(input);
    struct OpenElement {
        idStackNode<OpenElement> link;
        Node* node = nullptr;
        explicit OpenElement(Node* element) : node(element) {}
    };
    idStack<OpenElement, &OpenElement::link> open;
    std::vector<std::unique_ptr<OpenElement>> storage;
    auto push = [&](Node* element) {
        storage.push_back(std::make_unique<OpenElement>(element));
        open.Add(storage.back().get());
    };
    push(document.root.get());
    auto current = [&]() -> Node* { return open.Peek()->node; };

    size_t i = 0;
    while (i < input.size()) {
        if (input[i] != '<') {
            const auto end = input.find('<', i);
            const auto length = end == std::string_view::npos ? input.size() - i : end - i;
            if (length) {
                auto node = std::make_unique<Node>();
                node->type = Node::Type::Text;
                node->text = DecodeEntities(input.substr(i, length));
                node->parent = current();
                current()->children.push_back(std::move(node));
            }
            i += length;
            continue;
        }
        if (input.substr(i, 4) == "<!--") {
            auto end = input.find("-->", i + 4);
            i = end == std::string_view::npos ? input.size() : end + 3;
            continue;
        }
        if (input.substr(i, 2) == "<!" || input.substr(i, 2) == "<?") {
            auto end = input.find('>', i + 2);
            i = end == std::string_view::npos ? input.size() : end + 1;
            continue;
        }
        if (input.substr(i, 2) == "</") {
            size_t p = i + 2;
            while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
            const size_t nameStart = p;
            while (p < input.size() && IsName(input[p])) ++p;
            const std::string closeTag = Lower(input.substr(nameStart, p - nameStart));
            const auto end = input.find('>', p);
            i = end == std::string_view::npos ? input.size() : end + 1;
            OpenElement* match = open.Peek();
            while (match && match->node != document.root.get() &&
                   match->node->tag != closeTag) {
                match = match->link.GetNext();
            }
            if (match && match->node != document.root.get() &&
                match->node->tag == closeTag) {
                while (open.Peek() != match) open.Get();
                open.Get();
            }
            continue;
        }

        size_t p = i + 1;
        while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
        const auto nameStart = p;
        while (p < input.size() && IsName(input[p])) ++p;
        if (p == nameStart) {
            // Malformed less-than sign: treat as text, always advance.
            auto node = std::make_unique<Node>();
            node->type = Node::Type::Text;
            node->text = "<";
            node->parent = current();
            current()->children.push_back(std::move(node));
            ++i;
            continue;
        }
        auto node = std::make_unique<Node>();
        node->tag = Lower(input.substr(nameStart, p - nameStart));
        bool selfClosing = false;
        while (p < input.size() && input[p] != '>') {
            while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
            if (p >= input.size() || input[p] == '>') break;
            if (input[p] == '/' && p + 1 < input.size() && input[p + 1] == '>') {
                selfClosing = true; ++p; break;
            }
            const size_t attrStart = p;
            while (p < input.size() && IsName(input[p])) ++p;
            if (p == attrStart) { ++p; continue; }
            std::string key = Lower(input.substr(attrStart, p - attrStart));
            while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
            std::string value;
            if (p < input.size() && input[p] == '=') {
                ++p;
                while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
                if (p < input.size() && (input[p] == '"' || input[p] == '\'')) {
                    const char quote = input[p++];
                    const size_t from = p;
                    while (p < input.size() && input[p] != quote) ++p;
                    value = DecodeEntities(input.substr(from, p - from));
                    if (p < input.size()) ++p;
                } else {
                    const size_t from = p;
                    while (p < input.size() && !std::isspace(static_cast<unsigned char>(input[p]))
                           && input[p] != '>') ++p;
                    value = DecodeEntities(input.substr(from, p - from));
                }
            }
            node->attributes.insert_or_assign(std::move(key), std::move(value));
        }
        i = p < input.size() ? p + 1 : input.size();
        Node* element = node.get();
        // HTML error recovery: auto-close <head> before body content even
        // when the page omits an explicit <body> tag. Without this, all later
        // elements inherit the hidden HEAD style and the viewport stays white.
        const auto& tag = element->tag;
        const bool bodyContent = tag == "body" || tag == "main" ||
            tag == "header" || tag == "footer" || tag == "nav" ||
            tag == "section" || tag == "article" || tag == "aside" ||
            tag == "div" || tag == "p" || tag == "h1" || tag == "h2" ||
            tag == "h3" || tag == "h4" || tag == "h5" ||
            tag == "h6" || tag == "img" || tag == "form" ||
            tag == "table" || tag == "ul" || tag == "ol" ||
            tag == "a" || tag == "center";
        if (bodyContent) {
            OpenElement* head = open.Peek();
            while (head && head->node != document.root.get() &&
                   head->node->tag != "head") {
                head = head->link.GetNext();
            }
            if (head && head->node->tag == "head") {
                while (open.Peek() != head) open.Get();
                open.Get();
            }
        }
        if (element->tag == "p" || element->tag == "li") {
            // Sibling paragraphs and list items close their predecessor.
            // Preserve other elements until their matching close tags.
            OpenElement* top = open.Peek();
            if (top && top->node->tag == element->tag) open.Get();
        }
        element->parent = current();
        current()->children.push_back(std::move(node));

        if (element->tag == "script" || element->tag == "style") {
            const std::string closing = "</" + element->tag;
            const auto end = lower.find(closing, i);
            const auto limit = end == std::string::npos ? input.size() : end;
            const std::string raw(input.substr(i, limit - i));
            if (element->tag == "style") {
                document.embeddedCss += raw + "\n";
            } else {
                // Preserve inline script raw text as a non-rendered DOM text child.
                auto code = std::make_unique<Node>();
                code->type = Node::Type::Text;
                code->text = raw;
                code->parent = element;
                element->children.push_back(std::move(code));
            }
            i = limit;
            if (end == std::string::npos) break;
            // Closing tag is consumed in the next iteration.
            continue;
        }
        if (!selfClosing && !IsVoid(element->tag)) {
            push(element);
        }
    }
    CollectTitle(*document.root, document.title);
    return document;
}
} // namespace ac::html
