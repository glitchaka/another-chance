#include "script/ScriptRuntime.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac::script {
namespace {
using html::Node;
using html::Document;

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
void Skip(std::string_view s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}
bool Consume(std::string_view s, size_t& i, std::string_view fragment) {
    Skip(s, i);
    if (s.substr(i, fragment.size()) != fragment) return false;
    i += fragment.size();
    return true;
}
bool Literal(std::string_view s, size_t& i, std::string& value) {
    Skip(s, i);
    if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return false;
    const char quote = s[i++];
    while (i < s.size()) {
        const char c = s[i++];
        if (c == quote) return true;
        if (c != '\\') { value.push_back(c); continue; }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(e); break;
        }
        if (value.size() > 256*1024) return false;
    }
    return false;
}
bool LiteralExpression(std::string_view s, size_t& i, std::string& value) {
    if (!Literal(s,i,value)) return false;
    while (Consume(s,i,"+")) {
        if (!Literal(s,i,value)) return false;
    }
    return value.size() <= 256*1024;
}
std::vector<std::string_view> Statements(std::string_view s) {
    std::vector<std::string_view> result;
    size_t start = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i=0; i<s.size(); ++i) {
        const char c=s[i];
        if (quote) {
            if (escaped) { escaped=false; continue; }
            if (c=='\\') { escaped=true; continue; }
            if (c==quote) quote=0;
        } else if (c=='"' || c=='\'') {
            quote=c;
        } else if (c==';') {
            result.push_back(Trim(s.substr(start,i-start)));
            start=i+1;
        }
    }
    if (start<s.size()) result.push_back(Trim(s.substr(start)));
    return result;
}
Node* FindTag(Node* parent, std::string_view name) {
    if (!parent) return nullptr;
    if (parent->type==Node::Type::Element && parent->tag==name) return parent;
    for (auto& child: parent->children)
        if (auto* found=FindTag(child.get(),name)) return found;
    return nullptr;
}
Node* FindId(Node* parent, std::string_view id) {
    if (!parent) return nullptr;
    if (parent->type==Node::Type::Element && parent->Attribute("id")==id) return parent;
    for (auto& child: parent->children)
        if (auto* found=FindId(child.get(),id)) return found;
    return nullptr;
}
void Insert(Document& d, Node& target, std::string_view text, bool replace, bool raw) {
    if (replace) target.children.clear();
    if (raw) {
        auto n=std::make_unique<Node>();
        n->type=Node::Type::Text;
        n->text=std::string(text);
        n->parent=&target;
        target.children.push_back(std::move(n));
        return;
    }
    auto fragment=html::Parse(text);
    d.embeddedCss.append("\n").append(fragment.embeddedCss);
    if (!fragment.root) return;
    for (auto& n: fragment.root->children) {
        n->parent=&target;
        target.children.push_back(std::move(n));
    }
}
void CollectScripts(const Node& node, std::vector<std::string>& scripts, bool inert=false) {
    if (node.type!=Node::Type::Element) return;
    if (node.tag=="template" || node.tag=="noscript") inert=true;
    if (!inert && node.tag=="script" && node.Attribute("src").empty()) {
        for (const auto& child: node.children)
            if (child->type==Node::Type::Text && child->text.size() < 1024*1024)
                scripts.push_back(child->text);
    }
    for (const auto& child: node.children) CollectScripts(*child,scripts,inert);
}
bool ExecuteStatement(Document& d, std::string_view statement) {
    size_t i=0;
    if (!Consume(statement,i,"document.")) return false;
    Node* node=nullptr;
    bool replace=true, raw=false, title=false;
    if (Consume(statement,i,"write")) {
        if (!Consume(statement,i,"(")) return false;
        node=FindTag(d.root.get(),"body");
        if (!node) node=d.root.get();
        replace=false;
    } else if (Consume(statement,i,"title")) {
        title=true;
        if (!Consume(statement,i,"=")) return false;
    } else {
        if (Consume(statement,i,"body")) {
            node=FindTag(d.root.get(),"body");
        } else if (Consume(statement,i,"getElementById")) {
            if (!Consume(statement,i,"(")) return false;
            std::string id;
            if (!Literal(statement,i,id) || !Consume(statement,i,")")) return false;
            node=FindId(d.root.get(),id);
        } else if (Consume(statement,i,"querySelector")) {
            if (!Consume(statement,i,"(")) return false;
            std::string id;
            if (!Literal(statement,i,id) || !Consume(statement,i,")") ||
                id.empty() || id.front()!='#') return false;
            node=FindId(d.root.get(),std::string_view(id).substr(1));
        } else return false;
        if (!node || !Consume(statement,i,".")) return false;
        if (Consume(statement,i,"innerHTML")) raw=false;
        else if (Consume(statement,i,"textContent") ||
                 Consume(statement,i,"innerText")) raw=true;
        else return false;
        if (!Consume(statement,i,"=")) return false;
    }
    std::string value;
    if (!LiteralExpression(statement,i,value)) return false;
    if (!replace && !Consume(statement,i,")")) return false;
    Skip(statement,i);
    if (i!=statement.size()) return false;
    if (title) d.title=std::move(value);
    else if (node) Insert(d,*node,value,replace,raw);
    else return false;
    return true;
}
} // namespace
Execution ExecuteInline(html::Document& d) {
    Execution out{};
    if (!d.root) return out;
    std::vector<std::string> sources;
    CollectScripts(*d.root,sources);
    for (const auto& code: sources) {
        ++out.scripts;
        bool handled=false;
        for (auto statement: Statements(code)) {
            if (out.operations>=256) break;
            if (ExecuteStatement(d,statement)) {
                handled=true;
                ++out.operations;
            }
        }
        if (!handled) ++out.unsupported;
    }
    return out;
}
} // namespace ac::script
