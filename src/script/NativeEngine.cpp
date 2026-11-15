#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include "util/Url.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ac::script {
namespace {
using html::Node;
using html::Document;
using js::Token;
using js::TokenKind;

struct Value {
    enum class Kind { Undefined, Number, Text, Boolean, Element, Document, Location, Window };
    Kind kind=Kind::Undefined;
    double number=0;
    std::string text;
    Node* element=nullptr;
    static Value Num(double n) {Value v;v.kind=Kind::Number;v.number=n;return v;}
    static Value Str(std::string s) {Value v;v.kind=Kind::Text;v.text=std::move(s);return v;}
    static Value Bool(bool b) {Value v;v.kind=Kind::Boolean;v.number=b?1:0;return v;}
    static Value NodeValue(Node* n) {Value v;v.kind=Kind::Element;v.element=n;return v;}
    static Value Special(Kind k) {Value v;v.kind=k;return v;}
    std::string String() const {
        if(kind==Kind::Text) return text;
        if(kind==Kind::Boolean) return number?"true":"false";
        if(kind==Kind::Number) {
            std::ostringstream s;s<<std::setprecision(15)<<number;return s.str();
        }
        if(kind==Kind::Undefined) return "undefined";
        return "[object Object]";
    }
    double Number() const {
        if(kind==Kind::Number || kind==Kind::Boolean) return number;
        if(kind==Kind::Text) {
            char* end=nullptr;
            double x=std::strtod(text.c_str(),&end);
            if(end!=text.c_str() && *end==0) return x;
            if(text.empty()) return 0;
        }
        return std::nan("");
    }
    bool Truth() const {
        if(kind==Kind::Undefined) return false;
        if(kind==Kind::Text) return !text.empty();
        if(kind==Kind::Boolean || kind==Kind::Number) return number!=0 && !std::isnan(number);
        return true;
    }
};
Node* FindTag(Node* root,std::string_view tag) {
    if(!root) return nullptr;
    if(root->type==Node::Type::Element && root->tag==tag) return root;
    for(auto& ch:root->children) if(auto* found=FindTag(ch.get(),tag)) return found;
    return nullptr;
}
bool HasClass(const Node& node,std::string_view name) {
    std::istringstream stream(node.Attribute("class"));
    std::string word;
    while(stream>>word) if(word==name) return true;
    return false;
}
Node* FindSelector(Node* root,std::string_view selector) {
    if(!root || selector.empty()) return nullptr;
    if(root->type==Node::Type::Element) {
        if(selector[0]=='#' && root->Attribute("id")==selector.substr(1)) return root;
        if(selector[0]=='.' && HasClass(*root,selector.substr(1))) return root;
        if(selector[0]!='#' && selector[0]!='.' && root->tag==selector) return root;
    }
    for(auto& ch:root->children)
        if(auto* found=FindSelector(ch.get(),selector)) return found;
    return nullptr;
}
void ReadText(const Node& node,std::string& output) {
    if(node.type==Node::Type::Text) output+=node.text;
    for(auto& ch:node.children) ReadText(*ch,output);
}
void SetNode(Document& doc,Node& node,const std::string& value,bool html,bool append=false) {
    if(value.size()>1024*1024) throw std::runtime_error("DOM write too large");
    if(!append) node.children.clear();
    if(html) {
        auto fragment=html::Parse(value);
        doc.embeddedCss+="\n"+fragment.embeddedCss;
        if(fragment.root) for(auto& ch:fragment.root->children) {
            ch->parent=&node;node.children.push_back(std::move(ch));
        }
    } else {
        auto child=std::make_unique<Node>();
        child->type=Node::Type::Text;child->text=value;child->parent=&node;
        node.children.push_back(std::move(child));
    }
}
void CollectScripts(const Node& node,std::wstring_view base,
    const std::unordered_map<std::string,std::string>& external,
    std::vector<std::string>& scripts,bool inert=false) {
    if(scripts.size()>=64) return;
    if(node.tag=="template" || node.tag=="noscript") inert=true;
    if(!inert && node.tag=="script") {
        const auto type=node.Attribute("type");
        if(type.empty() || type=="text/javascript" || type=="application/javascript") {
            const auto src=node.Attribute("src");
            if(src.empty()) {
                for(auto& ch:node.children)
                    if(ch->type==Node::Type::Text && ch->text.size()<1024*1024)
                        scripts.push_back(ch->text);
            } else {
                const auto url=ac::url::Resolve(base,ac::url::ToWide(src));
                auto it=external.find(ac::url::ToUtf8(url));
                if(it!=external.end() && it->second.size()<1024*1024)
                    scripts.push_back(it->second);
            }
        }
    }
    for(auto& ch:node.children) CollectScripts(*ch,base,external,scripts,inert);
}
class Interpreter {
    Document& doc_;
    std::wstring base_;
    Execution& stats_;
    std::unordered_map<std::string,Value> vars_;
    std::vector<Token> tokens_;
    size_t pos_=0;
    unsigned steps_=0;
    struct Result {Value value;std::string path;};
    const Token& Peek() const {return tokens_[pos_];}
    bool Match(std::string_view text) {
        if(Peek().text==text) {++pos_;return true;}
        return false;
    }
    void Expect(std::string_view text) {
        if(!Match(text)) throw std::runtime_error("Unexpected JavaScript token");
    }
    void Tick() {
        if(++steps_>120000) throw std::runtime_error("Script execution budget exhausted");
    }
    Value ReadProperty(const Value& base,const std::string& name) {
        if(base.kind==Value::Kind::Document) {
            if(name=="title") return Value::Str(doc_.title);
            if(name=="body") return Value::NodeValue(FindTag(doc_.root.get(),"body"));
            if(name=="location") return Value::Special(Value::Kind::Location);
        }
        if(base.kind==Value::Kind::Window) {
            if(name=="document") return Value::Special(Value::Kind::Document);
            if(name=="location") return Value::Special(Value::Kind::Location);
        }
        if(base.kind==Value::Kind::Location && name=="href")
            return Value::Str(ac::url::ToUtf8(base_));
        if(base.kind==Value::Kind::Text && name=="length") return Value::Num(base.text.size());
        if(base.kind==Value::Kind::Element && base.element) {
            if(name=="id" || name=="value" || name=="className")
                return Value::Str(base.element->Attribute(name=="className"?"class":name));
            if(name=="innerHTML" || name=="innerText" || name=="textContent") {
                std::string text;ReadText(*base.element,text);return Value::Str(std::move(text));
            }
        }
        return {};
    }
    void Navigate(const Value& value) {
        const auto url=ac::url::Resolve(base_,ac::url::ToWide(value.String()));
        if(ac::url::IsWeb(url)) {stats_.navigation=url;++stats_.operations;}
    }
    void Write(const Result& target,const Value& value) {
        const auto& path=target.path;
        if(path.starts_with("$")) {vars_[path.substr(1)]=value;return;}
        const auto split=path.rfind('|');
        if(split==std::string::npos) throw std::runtime_error("Invalid JavaScript assignment");
        const auto member=path.substr(split+1);
        const auto owner=path.substr(0,split);
        if(owner=="document" && member=="title") {
            doc_.title=value.String();++stats_.operations;return;
        }
        if(owner=="location" && (member=="href" || member=="location")) {
            Navigate(value);return;
        }
        Node* node=nullptr;
        if(owner=="body") node=FindTag(doc_.root.get(),"body");
        else if(owner.starts_with("id:")) node=FindSelector(doc_.root.get(),"#"+owner.substr(3));
        else if(owner.starts_with("selector:"))
            node=FindSelector(doc_.root.get(),owner.substr(9));
        if(!node) return;
        if(member=="innerHTML" || member=="textContent" || member=="innerText") {
            SetNode(doc_,*node,value.String(),member=="innerHTML");++stats_.operations;
        } else if(member=="value" || member=="id" || member=="className") {
            node->attributes[member=="className"?"class":member]=value.String();
            ++stats_.operations;
        }
    }
    Result Primary() {
        Tick();
        if(Match("(")) {auto v=Expression();Expect(")");return {v.value,{}};}
        if(Match("!")) return {Value::Bool(!Primary().value.Truth()),{}};
        if(Match("-")) return {Value::Num(-Primary().value.Number()),{}};
        if(Match("+")) return {Value::Num(Primary().value.Number()),{}};
        if(Match("++")) {
            auto r=Primary();auto v=Value::Num(r.value.Number()+1);Write(r,v);return {v,{}};
        }
        if(Peek().kind==TokenKind::Number) return {Value::Num(tokens_[pos_++].number),{}};
        if(Peek().kind==TokenKind::String) return {Value::Str(tokens_[pos_++].text),{}};
        if(Peek().kind==TokenKind::Identifier) {
            auto name=tokens_[pos_++].text;
            if(name=="true") return {Value::Bool(true),{}};
            if(name=="false") return {Value::Bool(false),{}};
            if(name=="null" || name=="undefined") return {};
            if(name=="document") return {Value::Special(Value::Kind::Document),"document"};
            if(name=="window") return {Value::Special(Value::Kind::Window),"window"};
            if(name=="location") return {Value::Special(Value::Kind::Location),"location"};
            if(name=="NaN") return {Value::Num(std::nan("")),{}};
            auto it=vars_.find(name);
            return {it==vars_.end()?Value{}:it->second,"$"+name};
        }
        throw std::runtime_error("Unsupported JavaScript primary expression");
    }
    Result Postfix() {
        auto result=Primary();
        while(true) {
            if(Match(".")) {
                if(Peek().kind!=TokenKind::Identifier) throw std::runtime_error("Expected member name");
                const auto member=tokens_[pos_++].text;
                if(Match("(")) {
                    std::vector<Value> args;
                    if(!Match(")")) {
                        do {args.push_back(Expression(2).value);} while(Match(","));
                        Expect(")");
                    }
                    if(result.value.kind==Value::Kind::Document &&
                      (member=="getElementById" || member=="querySelector")) {
                        const auto argument=args.empty()?std::string{}:args[0].String();
                        const auto selector=member=="getElementById"?"#"+argument:argument;
                        result={Value::NodeValue(FindSelector(doc_.root.get(),selector)),
                                (member=="getElementById"?"id:":"selector:")+argument};
                    } else if(result.value.kind==Value::Kind::Document && member=="write") {
                        auto* body=FindTag(doc_.root.get(),"body");
                        if(!body) body=doc_.root.get();
                        for(auto& arg:args) SetNode(doc_,*body,arg.String(),true,true);
                        ++stats_.operations;result={};
                    } else if(result.value.kind==Value::Kind::Location &&
                              (member=="assign" || member=="replace")) {
                        if(!args.empty()) Navigate(args[0]);
                        result={};
                    } else throw std::runtime_error("Unsupported native JavaScript method");
                } else {
                    const auto old=result;
                    result.value=ReadProperty(old.value,member);
                    if(old.value.kind==Value::Kind::Document) {
                        result.path=member=="body"?"body":"document|"+member;
                    } else if(old.value.kind==Value::Kind::Location) result.path="location|"+member;
                    else if(old.value.kind==Value::Kind::Window && member=="location")
                        result.path="location|location";
                    else if(old.value.kind==Value::Kind::Element) result.path=old.path+"|"+member;
                    else result.path.clear();
                }
            } else if(Match("++")) {
                auto old=result.value;
                Write(result,Value::Num(old.Number()+1));result={old,{}};
            } else if(Match("--")) {
                auto old=result.value;
                Write(result,Value::Num(old.Number()-1));result={old,{}};
            } else break;
        }
        return result;
    }
    static int Priority(std::string_view s) {
        if(s=="=" || s=="+=" || s=="-=") return 1;
        if(s=="||" || s=="??") return 2;
        if(s=="&&") return 3;
        if(s=="==" || s=="===" || s=="!=" || s=="!==") return 4;
        if(s=="<" || s==">" || s=="<=" || s==">=") return 5;
        if(s=="+" || s=="-") return 6;
        if(s=="*" || s=="/" || s=="%") return 7;
        return -1;
    }
    Result Expression(int min=1) {
        auto left=Postfix();
        while(true) {
            const auto op=Peek().text;
            const int rank=Priority(op);
            if(rank<min) break;
            ++pos_;
            const bool assignment=rank==1;
            auto right=Expression(rank+(assignment?0:1));
            auto a=left.value,b=right.value;
            if(assignment) {
                Value value=b;
                if(op=="+=") value=(a.kind==Value::Kind::Text||b.kind==Value::Kind::Text)
                    ?Value::Str(a.String()+b.String()):Value::Num(a.Number()+b.Number());
                else if(op=="-=") value=Value::Num(a.Number()-b.Number());
                Write(left,value);left={value,{}};continue;
            }
            if(op=="+") left.value=(a.kind==Value::Kind::Text||b.kind==Value::Kind::Text)
                ?Value::Str(a.String()+b.String()):Value::Num(a.Number()+b.Number());
            else if(op=="-") left.value=Value::Num(a.Number()-b.Number());
            else if(op=="*") left.value=Value::Num(a.Number()*b.Number());
            else if(op=="/") left.value=Value::Num(a.Number()/b.Number());
            else if(op=="%") left.value=Value::Num(std::fmod(a.Number(),b.Number()));
            else if(op=="<") left.value=Value::Bool(a.Number()<b.Number());
            else if(op==">") left.value=Value::Bool(a.Number()>b.Number());
            else if(op=="<=") left.value=Value::Bool(a.Number()<=b.Number());
            else if(op==">=") left.value=Value::Bool(a.Number()>=b.Number());
            else if(op=="==" || op=="===") left.value=Value::Bool(a.String()==b.String());
            else if(op=="!=" || op=="!==") left.value=Value::Bool(a.String()!=b.String());
            else if(op=="&&") left.value=a.Truth()?b:a;
            else if(op=="||") left.value=a.Truth()?a:b;
            else if(op=="??") left.value=a.kind==Value::Kind::Undefined?b:a;
            left.path.clear();
        }
        return left;
    }
    void SkipStatement() {
        if(Match("{")) {
            int depth=1;
            while(depth && Peek().kind!=TokenKind::End) {
                if(Match("{")) ++depth;
                else if(Match("}")) --depth;
                else ++pos_;
            }
            return;
        }
        while(Peek().kind!=TokenKind::End && Peek().text!=";" && Peek().text!="}") ++pos_;
        (void)Match(";");
    }
    void Statement() {
        Tick();
        if(Match(";")) return;
        if(Match("{")) {
            while(!Match("}")) {
                if(Peek().kind==TokenKind::End) throw std::runtime_error("Unterminated JS block");
                Statement();
            }
            return;
        }
        if(Match("let") || Match("const") || Match("var")) {
            do {
                if(Peek().kind!=TokenKind::Identifier) throw std::runtime_error("Missing variable");
                auto name=tokens_[pos_++].text;
                Value value;
                if(Match("=")) value=Expression(2).value;
                vars_[name]=value;
            } while(Match(","));
            (void)Match(";");
            return;
        }
        if(Match("if")) {
            Expect("(");const bool condition=Expression().value.Truth();Expect(")");
            if(condition) {
                Statement();
                if(Match("else")) SkipStatement();
            } else {
                SkipStatement();
                if(Match("else")) Statement();
            }
            return;
        }
        if(Match("for")) {
            Expect("(");
            if(!Match(";")) Statement();
            const size_t test=pos_;
            while(Peek().text!=";" && Peek().kind!=TokenKind::End) ++pos_;
            Expect(";");
            const size_t update=pos_;
            while(Peek().text!=")" && Peek().kind!=TokenKind::End) ++pos_;
            Expect(")");
            const size_t body=pos_;
            SkipStatement();
            const size_t after=pos_;
            for(unsigned n=0;n<20000;++n) {
                Tick();
                pos_=test;
                if(!Match(";") && !Expression(2).value.Truth()) break;
                pos_=body;Statement();
                pos_=update;if(Peek().text!=")") (void)Expression();
            }
            pos_=after;
            return;
        }
        Expression();
        (void)Match(";");
    }
public:
    Interpreter(Document& d,std::wstring_view base,Execution& stats):
        doc_(d),base_(base),stats_(stats) {}
    bool Run(const std::string& source) {
        try {
            tokens_=js::Tokenize(source);
            pos_=0;
            while(Peek().kind!=TokenKind::End && stats_.navigation.empty()) Statement();
            return true;
        } catch(const std::exception&) {
            return false;
        }
    }
};
} // namespace

Execution ExecuteNative(Document& document,std::wstring_view baseUrl,
    const std::unordered_map<std::string,std::string>& external) {
    Execution report;
    if(!document.root) return report;
    report.engineReady=true;
    std::vector<std::string> scripts;
    CollectScripts(*document.root,baseUrl,external,scripts);
    Interpreter interpreter(document,baseUrl,report);
    for(const auto& source:scripts) {
        ++report.scripts;
        if(!interpreter.Run(source)) ++report.unsupported;
        if(!report.navigation.empty()) break;
    }
    return report;
}
} // namespace ac::script
