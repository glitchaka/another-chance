#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include "script/JsValue.h"
#include "script/JsJson.h"
#include "script/DomProperties.h"
#include "util/Url.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ac::script {
namespace {
using html::Node;
using html::Document;
using js::Token;
using js::TokenKind;

using js::Value;
std::uint32_t Uint32(double value) {
    if(!std::isfinite(value) || value==0) return 0;
    double bits=std::fmod(std::trunc(value),4294967296.0);
    if(bits<0) bits+=4294967296.0;
    return static_cast<std::uint32_t>(bits);
}
std::int32_t Int32(double value) {
    const std::uint32_t bits=Uint32(value);
    if(bits<=2147483647u) return static_cast<std::int32_t>(bits);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(bits)-4294967296LL);
}
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
void FindAll(Node* root,std::string_view selector,std::vector<Node*>& matches) {
    if(!root || matches.size()>=10000) return;
    if(root->type==Node::Type::Element && !selector.empty()) {
        bool found=false;
        if(selector.front()=='#') found=root->Attribute("id")==selector.substr(1);
        else if(selector.front()=='.') found=HasClass(*root,selector.substr(1));
        else found=root->tag==selector;
        if(found) matches.push_back(root);
    }
    for(auto& child:root->children) FindAll(child.get(),selector,matches);
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
void SetNode(Document& doc,Node& node,const std::string& value,bool html,
             bool append=false,
             std::vector<std::unique_ptr<Node>>* retained=nullptr) {
    if(value.size()>1024*1024) throw std::runtime_error("DOM write too large");
    if(!append) {
        if(retained) {
            for(auto& child:node.children) {
                child->parent=nullptr;
                retained->push_back(std::move(child));
            }
        }
        node.children.clear();
    }
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
struct ScriptSource {
    std::string url;
    std::string source;
};
void InsertAdjacent(Document& doc,Node& target,
                    std::string_view position,const std::string& markup,
                    bool textOnly=false) {
    if(markup.size()>1024*1024)
        throw std::runtime_error("Adjacent DOM insert exceeds size limit");
    Node* destination=&target;
    size_t index=0;
    if(position=="afterbegin") index=0;
    else if(position=="beforeend") index=target.children.size();
    else if(position=="beforebegin" || position=="afterend") {
        destination=target.parent;
        if(!destination)
            throw std::runtime_error("Adjacent DOM insert requires a parent");
        auto& siblings=destination->children;
        size_t offset=0;
        while(offset<siblings.size() && siblings[offset].get()!=&target) ++offset;
        if(offset==siblings.size())
            throw std::runtime_error("Adjacent DOM insert target not in parent");
        index=offset+(position=="afterend"?1:0);
    } else {
        throw std::runtime_error("Invalid insertAdjacentHTML position");
    }
    std::vector<std::unique_ptr<Node>> inserted;
    if(textOnly) {
        auto node=std::make_unique<Node>();
        node->type=Node::Type::Text;
        node->text=markup;
        inserted.push_back(std::move(node));
    } else {
        auto fragment=html::Parse(markup);
        doc.embeddedCss+="\n"+fragment.embeddedCss;
        if(fragment.root) inserted=std::move(fragment.root->children);
    }
    if(destination->children.size()+inserted.size()>100000)
        throw std::runtime_error("Adjacent DOM children limit");
    for(auto& node:inserted) node->parent=destination;
    auto& children=destination->children;
    children.insert(children.begin()+static_cast<std::ptrdiff_t>(index),
                    std::make_move_iterator(inserted.begin()),
                    std::make_move_iterator(inserted.end()));
}
void CollectScripts(const Node& node,std::wstring_view base,
    const std::unordered_map<std::string,std::string>& external,
    std::vector<ScriptSource>& scripts,bool inert=false) {
    if(scripts.size()>=64) return;
    if(node.tag=="template" || node.tag=="noscript") inert=true;
    if(!inert && node.tag=="script") {
        const auto type=node.Attribute("type");
        if(type.empty() || type=="text/javascript" || type=="application/javascript") {
            const auto src=node.Attribute("src");
            if(src.empty()) {
                for(auto& ch:node.children)
                    if(ch->type==Node::Type::Text && ch->text.size()<1024*1024)
                        scripts.push_back({"<inline>",ch->text});
            } else {
                const auto url=ac::url::Resolve(base,ac::url::ToWide(src));
                auto it=external.find(ac::url::ToUtf8(url));
                if(it!=external.end() && it->second.size()<1024*1024)
                    scripts.push_back({ac::url::ToUtf8(url),it->second});
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
    struct Function {
        std::vector<std::string> parameters;
        std::vector<Token> body;
        std::shared_ptr<std::unordered_map<std::string,Value>> captured;
        bool arrow=false;
        Value lexicalThis;
    };
    std::unordered_map<std::string,Function> functions_;
    struct ReturnSignal { Value value; };
    struct ThrowSignal { Value value; };
    struct BreakSignal {};
    struct LabelBreak { std::string label; };
    struct ContinueSignal {};
    unsigned loopDepth_=0;
    unsigned switchDepth_=0;
    unsigned lambdaCounter_=0;
    unsigned callDepth_=0;
    std::unordered_map<std::string,std::vector<Value>> eventListeners_;
    std::vector<Value> scheduledCallbacks_;
    Value thisValue_=Value::Special(Value::Kind::Window);
    std::mt19937_64 rng_{std::random_device{}()};
    std::shared_ptr<std::unordered_map<std::string,Value>> globalProperties_ =
        std::make_shared<std::unordered_map<std::string,Value>>();
    std::unordered_map<const void*,Value> objectPrototypes_;
    std::unordered_set<const void*> frozenObjects_;
    std::unordered_map<const void*,std::unordered_map<std::string,Value>> getters_;
    std::vector<std::unique_ptr<Node>> detached_;
    std::vector<Token> tokens_;
    size_t pos_=0;
    unsigned steps_=0;
    struct Result {
        Value value;
        std::string path;
        Node* propertyNode=nullptr;
        std::shared_ptr<std::unordered_map<std::string,Value>> propertyObject;
        Result() = default;
        Result(Value initial,std::string target={},
               Node* node=nullptr,
               std::shared_ptr<std::unordered_map<std::string,Value>> object=nullptr)
            :value(std::move(initial)),path(std::move(target)),propertyNode(node),
             propertyObject(std::move(object)) {}
    };
    const Token& Peek() const {return tokens_[pos_];}
    bool Match(std::string_view text) {
        // A string literal may contain "+" / "-" / ";" / "if" etc.
        // Token text alone is not sufficient to recognize grammar tokens.
        if((Peek().kind==TokenKind::Symbol ||
            Peek().kind==TokenKind::Identifier) &&
           Peek().text==text) {++pos_;return true;}
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
            if(name=="head") return Value::NodeValue(FindTag(doc_.root.get(),"head"));
            if(name=="documentElement") return Value::NodeValue(FindTag(doc_.root.get(),"html"));
            if(name=="readyState") return Value::Str("complete");
            if(name=="location") return Value::Special(Value::Kind::Location);
        }
        if(base.kind==Value::Kind::Window) {
            if(name=="document") return Value::Special(Value::Kind::Document);
            if(name=="Math") return Value::Special(Value::Kind::Math);
            if(name=="JSON") return Value::Special(Value::Kind::Json);
            if(name=="location") return Value::Special(Value::Kind::Location);
            if(name=="window" || name=="self" || name=="globalThis")
                return Value::Special(Value::Kind::Window);
            auto global=globalProperties_->find(name);
            if(global!=globalProperties_->end()) return global->second;
        }
        if(base.kind==Value::Kind::Location && name=="href")
            return Value::Str(ac::url::ToUtf8(base_));
        if(base.kind==Value::Kind::Object && base.object) {
            const void* origin=base.object.get();
            auto getters=getters_.find(origin);
            if(getters!=getters_.end()) {
                auto getter=getters->second.find(name);
                if(getter!=getters->second.end() &&
                   getter->second.kind==Value::Kind::Function)
                    return CallFunction(getter->second,{});
            }
            auto property=base.object->find(name);
            if(property!=base.object->end()) return property->second;
            std::unordered_set<const void*> seen{origin};
            auto prototype=objectPrototypes_.find(origin);
            for(unsigned depth=0;depth<32 &&
                prototype!=objectPrototypes_.end();++depth) {
                const Value& inherited=prototype->second;
                if(inherited.kind!=Value::Kind::Object || !inherited.object) break;
                if(!seen.insert(inherited.object.get()).second) break;
                auto next=inherited.object->find(name);
                if(next!=inherited.object->end()) return next->second;
                prototype=objectPrototypes_.find(inherited.object.get());
            }
            return {};
        }
        if(base.kind==Value::Kind::RegExp) {
            if(name=="source") {
                const auto lastSlash=base.text.rfind('/');
                return Value::Str(base.text.substr(1,lastSlash-1));
            }
            if(name=="global") return Value::Bool(base.regexpGlobal);
        }
        if(base.kind==Value::Kind::Text && name=="length") return Value::Num(base.text.size());
        if(base.kind==Value::Kind::Array && base.array && name=="length")
            return Value::Num(base.array->size());
        if(base.kind==Value::Kind::Style && base.element) {
            if(name=="cssText") return Value::Str(base.element->Attribute("style"));
            return Value::Str(dom::GetStyle(*base.element,name));
        }
        if(base.kind==Value::Kind::ClassList && base.element) {
            if(name=="value") return Value::Str(base.element->Attribute("class"));
            if(name=="length") {
                std::istringstream input(base.element->Attribute("class"));
                std::string token;
                size_t count=0;
                while(input>>token) ++count;
                return Value::Num(count);
            }
        }
        if(base.kind==Value::Kind::Element && base.element) {
            if(name=="style") return Value::NodeFacade(Value::Kind::Style,base.element);
            if(name=="classList") return Value::NodeFacade(Value::Kind::ClassList,base.element);
            if(name=="id" || name=="value" || name=="className")
                return Value::Str(base.element->Attribute(name=="className"?"class":name));
            if(name=="innerHTML")
                return Value::Str(dom::SerializeChildren(*base.element));
            if(name=="innerText" || name=="textContent") {
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
        if(target.propertyObject) {
            const auto delimiter=target.path.rfind('|');
            if(delimiter==std::string::npos) throw std::runtime_error("Invalid object property");
            if(!frozenObjects_.contains(target.propertyObject.get()))
                (*target.propertyObject)[target.path.substr(delimiter+1)]=value;
            return;
        }
        if(target.propertyNode) {
            const auto delimiter=target.path.rfind('|');
            if(delimiter==std::string::npos) throw std::runtime_error("Invalid element property");
            const auto member=target.path.substr(delimiter+1);
            if(target.path.starts_with("style|")) {
                if(member=="cssText")
                    target.propertyNode->attributes["style"]=value.String();
                else dom::SetStyle(*target.propertyNode,member,value.String());
                ++stats_.operations;
                return;
            }
            if(target.path.starts_with("classList|") && member=="value") {
                target.propertyNode->attributes["class"]=value.String();
                ++stats_.operations;
                return;
            }
            if(member=="innerHTML" || member=="textContent" || member=="innerText") {
                SetNode(doc_,*target.propertyNode,value.String(),member=="innerHTML",false,&detached_);
                ++stats_.operations;
            } else if(member=="value" || member=="id" || member=="className") {
                target.propertyNode->attributes[member=="className"?"class":member]=value.String();
                ++stats_.operations;
            }
            return;
        }
        const auto& path=target.path;
        if(path.starts_with("$")) {
            auto name=path.substr(1);
            if(!vars_.contains(name) && globalProperties_->contains(name))
                (*globalProperties_)[name]=value;
            else vars_[name]=value;
            return;
        }
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
            SetNode(doc_,*node,value.String(),member=="innerHTML",false,&detached_);++stats_.operations;
        } else if(member=="value" || member=="id" || member=="className") {
            node->attributes[member=="className"?"class":member]=value.String();
            ++stats_.operations;
        }
    }
    Result ArrowFunction(std::vector<std::string> parameters) {
        Expect("=>");
        Function fn;
        fn.parameters=std::move(parameters);
        fn.arrow=true;
        fn.lexicalThis=thisValue_;
        if(callDepth_>0)
            fn.captured=std::make_shared<std::unordered_map<std::string,Value>>(vars_);
        if(Peek().text=="{") {
            const size_t start=pos_;
            int nesting=0;
            do {
                if(Peek().kind==TokenKind::End)
                    throw std::runtime_error("Unterminated arrow function block");
                if(Peek().text=="{") ++nesting;
                else if(Peek().text=="}") --nesting;
                ++pos_;
            } while(nesting>0);
            fn.body.assign(tokens_.begin()+static_cast<std::ptrdiff_t>(start),
                           tokens_.begin()+static_cast<std::ptrdiff_t>(pos_));
        } else {
            fn.body.push_back(Token{TokenKind::Symbol,"{",0,0});
            fn.body.push_back(Token{TokenKind::Identifier,"return",0,0});
            const size_t start=pos_;
            unsigned parens=0,brackets=0,braces=0;
            while(Peek().kind!=TokenKind::End) {
                const auto token=Peek().text;
                if(!parens && !brackets && !braces &&
                   (token==";" || token=="," || token==")" ||
                    token=="]" || token=="}")) break;
                if(token=="(") ++parens;
                else if(token=="[") ++brackets;
                else if(token=="{") ++braces;
                else if(token==")" && parens) --parens;
                else if(token=="]" && brackets) --brackets;
                else if(token=="}" && braces) --braces;
                ++pos_;
            }
            if(pos_==start) throw std::runtime_error("Missing arrow function expression");
            fn.body.insert(fn.body.end(),
                           tokens_.begin()+static_cast<std::ptrdiff_t>(start),
                           tokens_.begin()+static_cast<std::ptrdiff_t>(pos_));
            fn.body.push_back(Token{TokenKind::Symbol,";",0,0});
            fn.body.push_back(Token{TokenKind::Symbol,"}",0,0});
        }
        fn.body.push_back(Token{TokenKind::End,"",0,0});
        const auto name="@arrow:"+std::to_string(++lambdaCounter_);
        functions_.insert_or_assign(name,std::move(fn));
        auto value=Value::Special(Value::Kind::Function);
        value.text=name;
        return {value,{}};
    }
    Result Primary() {
        Tick();
        if(Match("new")) {
            auto target=Primary();
            while(Match(".")) {
                if(Peek().kind!=TokenKind::Identifier)
                    throw std::runtime_error("Missing constructor property name");
                target.value=ReadProperty(target.value,tokens_[pos_++].text);
            }
            std::vector<Value> args;
            if(Match("(")) {
                if(!Match(")")) {
                    do {args.push_back(Expression(2).value);} while(Match(","));
                    Expect(")");
                }
            }
            if(target.value.kind!=Value::Kind::Function)
                throw std::runtime_error("Constructor is not a function");
            auto instance=Value::ObjectValue({});
            const auto returned=CallFunction(target.value,args,instance);
            return {returned.kind==Value::Kind::Object ||
                    returned.kind==Value::Kind::Array ||
                    returned.kind==Value::Kind::RegExp?returned:instance,{}};
        }
        if(Match("function")) {
            std::string declaredName;
            if(Peek().kind==TokenKind::Identifier)
                declaredName=tokens_[pos_++].text;
            Expect("(");
            Function fn;
            if(callDepth_>0)
                fn.captured=std::make_shared<std::unordered_map<std::string,Value>>(vars_);
            if(!Match(")")) {
                do {
                    if(Peek().kind!=TokenKind::Identifier)
                        throw std::runtime_error("Function parameter name expected");
                    fn.parameters.push_back(tokens_[pos_++].text);
                } while(Match(","));
                Expect(")");
            }
            if(Peek().text!="{") throw std::runtime_error("Function body expected");
            const size_t begin=pos_;
            int depth=0;
            do {
                if(Peek().kind==TokenKind::End)
                    throw std::runtime_error("Unterminated function expression");
                if(Peek().text=="{") ++depth;
                else if(Peek().text=="}") --depth;
                ++pos_;
            } while(depth>0);
            fn.body.assign(tokens_.begin()+static_cast<std::ptrdiff_t>(begin),
                           tokens_.begin()+static_cast<std::ptrdiff_t>(pos_));
            fn.body.push_back(Token{TokenKind::End,"",0,0});
            const std::string internal="@function:"+std::to_string(++lambdaCounter_);
            functions_.insert_or_assign(internal,std::move(fn));
            auto value=Value::Special(Value::Kind::Function);
            value.text=internal;
            if(!declaredName.empty()) vars_[declaredName]=value;
            return {value,{}};
        }
        if(Peek().kind==TokenKind::Identifier && pos_+1<tokens_.size() &&
           tokens_[pos_+1].text=="=>") {
            std::vector<std::string> parameters{tokens_[pos_++].text};
            return ArrowFunction(std::move(parameters));
        }
        if(Match("(")) {
            const size_t saved=pos_;
            std::vector<std::string> parameters;
            bool valid=true;
            if(Peek().text!=")") {
                while(true) {
                    if(Peek().kind!=TokenKind::Identifier) {valid=false;break;}
                    parameters.push_back(tokens_[pos_++].text);
                    if(!Match(",")) break;
                }
            }
            if(valid && Match(")") && Peek().text=="=>")
                return ArrowFunction(std::move(parameters));
            pos_=saved;
            auto value=Expression(0);
            Expect(")");
            return {value.value,{}};
        }
        if(Match("{")) {
            std::unordered_map<std::string,Value> properties;
            if(!Match("}")) {
                do {
                    if(Peek().kind!=TokenKind::Identifier &&
                       Peek().kind!=TokenKind::String &&
                       Peek().kind!=TokenKind::Number)
                        throw std::runtime_error("Invalid JavaScript object key");
                    const auto key=tokens_[pos_++].text;
                    Value value;
                    if(Match(":")) value=Expression(2).value;
                    else {
                        auto existing=vars_.find(key);
                        value=existing==vars_.end()?Value{}:existing->second;
                    }
                    properties.insert_or_assign(key,std::move(value));
                } while(Match(","));
                Expect("}");
            }
            return {Value::ObjectValue(std::move(properties)),{}};
        }
        if(Match("[")) {
            std::vector<Value> elements;
            if(!Match("]")) {
                do {elements.push_back(Expression(2).value);} while(Match(","));
                Expect("]");
            }
            return {Value::ArrayValue(std::move(elements)),{}};
        }
        if(Match("!")) return {Value::Bool(!Postfix().value.Truth()),{}};
        if(Match("typeof")) {
            const auto value=Postfix().value;
            switch(value.kind) {
                case Value::Kind::Undefined: return {Value::Str("undefined"),{}};
                case Value::Kind::Number: return {Value::Str("number"),{}};
                case Value::Kind::Text: return {Value::Str("string"),{}};
                case Value::Kind::Boolean: return {Value::Str("boolean"),{}};
                case Value::Kind::Function: return {Value::Str("function"),{}};
                default: return {Value::Str("object"),{}};
            }
        }
        if(Match("~")) return {Value::Num(static_cast<double>(~Int32(Postfix().value.Number()))),{}};
        if(Match("-")) return {Value::Num(-Postfix().value.Number()),{}};
        if(Match("+")) return {Value::Num(Postfix().value.Number()),{}};
        if(Match("++")) {
            auto r=Postfix();auto v=Value::Num(r.value.Number()+1);Write(r,v);return {v,{}};
        }
        if(Peek().kind==TokenKind::RegExp) {
            const auto expression=tokens_[pos_++].text;
            const auto lastSlash=expression.rfind('/');
            if(lastSlash==std::string::npos || lastSlash==0)
                throw std::runtime_error("Invalid regexp literal");
            try {
                return {Value::RegExpValue(
                    expression.substr(1,lastSlash-1),
                    expression.substr(lastSlash+1)),{}};
            } catch(const std::regex_error&) {
                throw std::runtime_error("Unsupported regexp literal");
            }
        }
        if(Peek().kind==TokenKind::Number) return {Value::Num(tokens_[pos_++].number),{}};
        if(Peek().kind==TokenKind::String) return {Value::Str(tokens_[pos_++].text),{}};
        if(Peek().kind==TokenKind::Identifier) {
            auto name=tokens_[pos_++].text;
            if(name=="true") return {Value::Bool(true),{}};
            if(name=="false") return {Value::Bool(false),{}};
            if(name=="null") return {Value::Special(Value::Kind::Null),{}};
            if(name=="undefined") return {};
            if(name=="document") return {Value::Special(Value::Kind::Document),"document"};
            if(name=="this") return {thisValue_,{}};
            if(name=="window" || name=="self" || name=="globalThis")
                return {Value::Special(Value::Kind::Window),"window"};
            if(name=="location") return {Value::Special(Value::Kind::Location),"location"};
            if(name=="NaN") return {Value::Num(std::nan("")),{}};
            if(name=="Infinity") return {Value::Num(INFINITY),{}};
            if(name=="Math") return {Value::Special(Value::Kind::Math),{}};
            if(name=="JSON") return {Value::Special(Value::Kind::Json),{}};
            if(name=="String" || name=="Number" || name=="Boolean" ||
               name=="parseInt" || name=="parseFloat" || name=="isNaN" ||
               name=="eval" || name=="Array" || name=="Object" ||
               name=="RegExp" || name=="setTimeout" || name=="clearTimeout" ||
               name=="queueMicrotask" || name=="requestAnimationFrame") {
                auto builtin=Value::Special(Value::Kind::Function);
                builtin.text="@builtin:"+name;
                return {builtin,{}};
            }
            auto it=vars_.find(name);
            if(it!=vars_.end()) return {it->second,"$"+name};
            auto global=globalProperties_->find(name);
            if(global!=globalProperties_->end())
                return {global->second,"$"+name};
            if(functions_.contains(name)) {
                auto callable=Value::Special(Value::Kind::Function);
                callable.text=name;
                return {callable,{}};
            }
            return {Value{},"$"+name};
        }
        throw std::runtime_error("Unsupported JavaScript primary near token: " + Peek().text + " at " + std::to_string(Peek().offset));
    }
    Value CallFunction(const Value& value,const std::vector<Value>& args,
                       Value receiver=Value{}) {
        const auto first = args.empty()?Value{}:args.front();
        if(value.text=="@builtin:String") return Value::Str(first.String());
        if(value.text=="@builtin:Number") return Value::Num(args.empty()?0:first.Number());
        if(value.text=="@builtin:Boolean") return Value::Bool(first.Truth());
        if(value.text=="@builtin:Array") {
            if(args.size()==1 && args[0].kind==Value::Kind::Number) {
                const double size=args[0].number;
                if(!std::isfinite(size) || size<0 || std::floor(size)!=size || size>100000)
                    throw std::runtime_error("Invalid JavaScript Array length");
                return Value::ArrayValue(std::vector<Value>(static_cast<size_t>(size)));
            }
            return Value::ArrayValue(args);
        }
        if(value.text=="@builtin:setTimeout" ||
           value.text=="@builtin:queueMicrotask" ||
           value.text=="@builtin:requestAnimationFrame") {
            if(!args.empty() && args[0].kind==Value::Kind::Function) {
                if(scheduledCallbacks_.size()>=128)
                    throw std::runtime_error("Event callback limit exceeded");
                scheduledCallbacks_.push_back(args[0]);
                return Value::Num(scheduledCallbacks_.size());
            }
            return Value::Num(0);
        }
        if(value.text=="@builtin:clearTimeout") {
            if(!args.empty()) {
                const double id=args[0].Number();
                if(std::isfinite(id) && id>=1 &&
                   id<=static_cast<double>(scheduledCallbacks_.size()))
                    scheduledCallbacks_[static_cast<size_t>(id)-1]=Value{};
            }
            return {};
        }
        if(value.text=="@builtin:RegExp") {
            if(first.kind==Value::Kind::RegExp && args.size()<2) return first;
            try {
                return Value::RegExpValue(args.empty()?"":first.String(),
                    args.size()>1?args[1].String():"");
            } catch(const std::regex_error&) {
                throw std::runtime_error("Unsupported RegExp pattern");
            }
        }
        if(value.text=="@builtin:Object") {
            if(!args.empty() && (first.kind==Value::Kind::Object ||
                                 first.kind==Value::Kind::Array)) return first;
            return Value::ObjectValue({});
        }
        if(value.text=="@builtin:parseInt") {
            const auto input=first.String();
            const int radix=args.size()>1?static_cast<int>(args[1].Number()):10;
            if(radix<2 || radix>36) return Value::Num(std::nan(""));
            char* end=nullptr;
            const long long value=std::strtoll(input.c_str(),&end,radix);
            return Value::Num(end==input.c_str()?std::nan(""):static_cast<double>(value));
        }
        if(value.text=="@builtin:parseFloat") {
            const auto input=first.String();
            char* end=nullptr;
            const double number=std::strtod(input.c_str(),&end);
            return Value::Num(end==input.c_str()?std::nan(""):number);
        }
        if(value.text=="@builtin:isNaN") return Value::Bool(std::isnan(first.Number()));
        if(value.text=="@builtin:eval") {
            if(first.kind!=Value::Kind::Text) return first;
            if(callDepth_>=32) throw std::runtime_error("JavaScript eval recursion limit");
            ++callDepth_;
            auto originalTokens=std::move(tokens_);
            const size_t originalPosition=pos_;
            Value completed;
            try {
                tokens_=js::Tokenize(first.text);
                pos_=0;
                while(Peek().kind!=TokenKind::End && stats_.navigation.empty()) {
                    if(Peek().text==";") {++pos_;continue;}
                    // For expression-only input return its completion value.
                    if(tokens_.size()>1 && tokens_.back().kind==TokenKind::End &&
                       tokens_.size()<5 && Peek().kind!=TokenKind::Identifier) {
                        completed=Expression().value;
                        (void)Match(";");
                    } else Statement();
                }
            } catch(...) {
                tokens_=std::move(originalTokens);
                pos_=originalPosition;
                --callDepth_;
                throw;
            }
            tokens_=std::move(originalTokens);
            pos_=originalPosition;
            --callDepth_;
            return completed;
        }
        const auto found=functions_.find(value.text);
        if(found==functions_.end()) throw std::runtime_error("Unknown JavaScript function");
        if(callDepth_>=32) throw std::runtime_error("JavaScript recursion depth exceeded");
        ++callDepth_;
        const unsigned outerLoopDepth=loopDepth_;
        const Value previousThis=thisValue_;
        loopDepth_=0;
        const Function function=found->second;
        thisValue_=function.arrow?function.lexicalThis:
            (receiver.kind==Value::Kind::Undefined ||
             receiver.kind==Value::Kind::Null
                 ?Value::Special(Value::Kind::Window):receiver);
        auto savedTokens=std::move(tokens_);
        const size_t savedPosition=pos_;
        auto savedVariables=vars_;
        if(function.captured)
            for(const auto& [key,val]:*function.captured)
                vars_.insert_or_assign(key,val);
        for(size_t i=0;i<function.parameters.size();++i)
            vars_[function.parameters[i]]=i<args.size()?args[i]:Value{};
        tokens_=function.body;
        pos_=0;
        Value returned;
        try {
            Statement();
        } catch(const ReturnSignal& signal) {
            returned=signal.value;
        } catch(...) {
            vars_=std::move(savedVariables);
            tokens_=std::move(savedTokens);
            pos_=savedPosition;
            --callDepth_;
            loopDepth_=outerLoopDepth;
            thisValue_=previousThis;
            throw;
        }
        // A returned nested function retains its lexical variables.
        if(function.captured)
            for(auto& [key,old]:*function.captured) {
                bool isParameter=false;
                for(const auto& parameter:function.parameters)
                    if(key==parameter) {isParameter=true;break;}
                if(!isParameter) {
                    const auto updated=vars_.find(key);
                    if(updated!=vars_.end()) old=updated->second;
                }
            }
        // Update preexisting outer bindings without leaking function-local names.
        for(auto& [name,old]:savedVariables) {
            bool parameter=false;
            for(const auto& argument:function.parameters)
                if(argument==name) {parameter=true;break;}
            if(!parameter) {
                auto changed=vars_.find(name);
                if(changed!=vars_.end()) old=changed->second;
            }
        }
        vars_=std::move(savedVariables);
        tokens_=std::move(savedTokens);
        pos_=savedPosition;
        --callDepth_;
        loopDepth_=outerLoopDepth;
        thisValue_=previousThis;
        return returned;
    }
    Result Postfix() {
        auto result=Primary();
        while(true) {
            if(Match("(")) {
                std::vector<Value> args;
                if(!Match(")")) {
                    do { args.push_back(Expression(2).value); } while(Match(","));
                    Expect(")");
                }
                if(result.value.kind!=Value::Kind::Function) {
                    const auto offset=pos_ ? tokens_[pos_-1].offset:0;
                    const auto referent=result.path.empty()?"<expression>":result.path;
                    throw std::runtime_error(
                        "Cannot call " + referent + " (" + result.value.String() +
                        ") near script byte " + std::to_string(offset));
                }
                result={CallFunction(result.value,args),{}};
            } else if(Match("[")) {
                const auto key=Expression().value;
                Expect("]");
                if(result.value.kind==Value::Kind::Array && result.value.array) {
                    const auto index=key.Number();
                    const auto count=result.value.array->size();
                    if(std::isfinite(index) && index>=0 && index<count &&
                       std::floor(index)==index)
                        result.value=(*result.value.array)[static_cast<size_t>(index)];
                    else result.value={};
                } else if(result.value.kind==Value::Kind::Object && result.value.object) {
                    auto owner=result.value.object;
                    auto found=owner->find(key.String());
                    result.value=found==owner->end()?Value{}:found->second;
                    result.propertyObject=std::move(owner);
                    result.path="object|"+key.String();
                } else if(result.value.kind==Value::Kind::Text) {
                    const auto index=key.Number();
                    if(std::isfinite(index) && index>=0 && index<result.value.text.size() &&
                       std::floor(index)==index)
                        result.value=Value::Str(std::string(1,result.value.text[static_cast<size_t>(index)]));
                    else result.value={};
                } else result.value={};
                if(!result.propertyObject) result.path.clear();
                result.propertyNode=nullptr;
            } else if(Peek().text=="." || Peek().text=="?.") {
                const bool optional=Match("?.");
                if(!optional) Expect(".");
                if(Peek().kind!=TokenKind::Identifier)
                    throw std::runtime_error("Expected member name");
                const auto member=tokens_[pos_++].text;
                if(optional &&
                   (result.value.kind==Value::Kind::Null ||
                    result.value.kind==Value::Kind::Undefined)) {
                    if(Match("(")) {
                        unsigned nesting=1;
                        while(nesting>0 && Peek().kind!=TokenKind::End) {
                            if(Match("(")) ++nesting;
                            else if(Match(")")) --nesting;
                            else ++pos_;
                        }
                        if(nesting) throw std::runtime_error("Unterminated optional call");
                    }
                    result={};
                    continue;
                }
                if(Match("(")) {
                    std::vector<Value> args;
                    if(!Match(")")) {
                        do {args.push_back(Expression(2).value);} while(Match(","));
                        Expect(")");
                    }
                    if(result.value.kind==Value::Kind::Style &&
                       result.value.element &&
                       (member=="setProperty" || member=="getPropertyValue" ||
                        member=="removeProperty")) {
                        const std::string key=args.empty()?"":args[0].String();
                        const std::string old=dom::GetStyle(*result.value.element,key);
                        if(member=="setProperty") {
                            dom::SetStyle(*result.value.element,key,
                                args.size()>1?args[1].String():"");
                            ++stats_.operations;
                            result={};
                        } else if(member=="removeProperty") {
                            dom::SetStyle(*result.value.element,key,"");
                            ++stats_.operations;
                            result={Value::Str(old),{}};
                        } else result={Value::Str(old),{}};
                    } else if(result.value.kind==Value::Kind::ClassList &&
                              result.value.element &&
                              (member=="add" || member=="remove" ||
                               member=="toggle" || member=="contains" ||
                               member=="replace")) {
                        auto* element=result.value.element;
                        const auto key=args.empty()?"":args[0].String();
                        if(member=="contains") {
                            result={Value::Bool(dom::ContainsClass(*element,key)),{}};
                        } else if(member=="toggle") {
                            if(args.size()>1) {
                                dom::SetClass(*element,key,args[1].Truth());
                                result={Value::Bool(args[1].Truth()),{}};
                            } else result={Value::Bool(dom::ToggleClass(*element,key)),{}};
                            ++stats_.operations;
                        } else if(member=="replace") {
                            const bool exists=dom::ContainsClass(*element,key);
                            if(exists && args.size()>1) {
                                dom::SetClass(*element,key,false);
                                dom::SetClass(*element,args[1].String(),true);
                                ++stats_.operations;
                            }
                            result={Value::Bool(exists),{}};
                        } else {
                            for(const auto& arg:args)
                                dom::SetClass(*element,arg.String(),member=="add");
                            ++stats_.operations;
                            result={};
                        }
                    } else if((result.value.kind==Value::Kind::Document ||
                        result.value.kind==Value::Kind::Window ||
                        result.value.kind==Value::Kind::Element) &&
                       (member=="addEventListener" || member=="removeEventListener")) {
                        if(args.size()>=2 && args[1].kind==Value::Kind::Function) {
                            auto& callbacks=eventListeners_[args[0].String()];
                            if(member=="addEventListener") {
                                if(callbacks.size()>=128)
                                    throw std::runtime_error("Event listener limit exceeded");
                                callbacks.push_back(args[1]);
                            } else {
                                callbacks.erase(std::remove_if(
                                    callbacks.begin(),callbacks.end(),
                                    [&](const Value& old) {
                                        return old.kind==Value::Kind::Function &&
                                               old.text==args[1].text;
                                    }),callbacks.end());
                            }
                        }
                        result={};
                    } else if(result.value.kind==Value::Kind::RegExp &&
                       (member=="test" || member=="exec")) {
                        if(!result.value.regexp)
                            throw std::runtime_error("Uncompiled regexp");
                        const std::string input=args.empty()?"undefined":args[0].String();
                        std::smatch match;
                        const bool found=std::regex_search(input,match,*result.value.regexp);
                        if(member=="test") {
                            result={Value::Bool(found),{}};
                        } else if(!found) {
                            result={Value::Special(Value::Kind::Null),{}};
                        } else {
                            std::vector<Value> captures;
                            for(const auto& item:match)
                                captures.push_back(Value::Str(item.str()));
                            result={Value::ArrayValue(std::move(captures)),{}};
                        }
                    } else if(result.value.kind==Value::Kind::Function &&
                       result.value.text=="@builtin:Object" &&
                       (member=="create" || member=="assign" ||
                        member=="keys" || member=="values" ||
                        member=="hasOwn" || member=="freeze" ||
                        member=="isFrozen" || member=="defineProperty" ||
                        member=="defineProperties" ||
                        member=="getPrototypeOf" || member=="setPrototypeOf" ||
                        member=="getOwnPropertyDescriptor")) {
                        const Value target=args.empty()?Value{}:args[0];
                        if(member=="create") {
                            if(target.kind!=Value::Kind::Object &&
                               target.kind!=Value::Kind::Null)
                                throw std::runtime_error("Object.create requires object or null prototype");
                            auto created=Value::ObjectValue({});
                            if(target.kind==Value::Kind::Object)
                                objectPrototypes_[created.object.get()]=target;
                            result={created,{}};
                        } else if(member=="keys" || member=="values") {
                            if(target.kind!=Value::Kind::Object || !target.object)
                                throw std::runtime_error("Object.keys requires an object");
                            std::vector<Value> members;
                            for(const auto& [key,val]:*target.object)
                                members.push_back(member=="keys"?Value::Str(key):val);
                            result={Value::ArrayValue(std::move(members)),{}};
                        } else if(member=="hasOwn") {
                            const bool owns=target.kind==Value::Kind::Object &&
                                target.object && args.size()>1 &&
                                (target.object->contains(args[1].String()) ||
                                 (getters_.contains(target.object.get()) &&
                                  getters_[target.object.get()].contains(args[1].String())));
                            result={Value::Bool(owns),{}};
                        } else if(member=="isFrozen") {
                            result={Value::Bool(target.kind==Value::Kind::Object &&
                                target.object && frozenObjects_.contains(target.object.get())),{}};
                        } else if(member=="freeze") {
                            if(target.kind==Value::Kind::Object && target.object)
                                frozenObjects_.insert(target.object.get());
                            result={target,{}};
                        } else if(member=="getPrototypeOf") {
                            if(target.kind!=Value::Kind::Object || !target.object)
                                throw std::runtime_error("Object.getPrototypeOf requires object");
                            const auto found=objectPrototypes_.find(target.object.get());
                            result={found==objectPrototypes_.end()
                                ?Value::Special(Value::Kind::Null):found->second,{}};
                        } else if(member=="setPrototypeOf") {
                            if(target.kind!=Value::Kind::Object || !target.object ||
                               args.size()<2 ||
                               (args[1].kind!=Value::Kind::Object &&
                                args[1].kind!=Value::Kind::Null))
                                throw std::runtime_error("Object.setPrototypeOf invalid arguments");
                            objectPrototypes_[target.object.get()]=args[1];
                            result={target,{}};
                        } else if(member=="assign") {
                            if(target.kind!=Value::Kind::Object || !target.object)
                                throw std::runtime_error("Object.assign target must be object");
                            if(!frozenObjects_.contains(target.object.get()))
                                for(size_t index=1;index<args.size();++index) {
                                    const auto& source=args[index];
                                    if(source.kind!=Value::Kind::Object || !source.object) continue;
                                    for(const auto& [key,val]:*source.object)
                                        target.object->insert_or_assign(key,val);
                                }
                            result={target,{}};
                        } else if(member=="getOwnPropertyDescriptor") {
                            if(target.kind!=Value::Kind::Object || !target.object ||
                               args.size()<2 || !target.object->contains(args[1].String())) {
                                result={};
                            } else {
                                const auto key=args[1].String();
                                auto descriptor=Value::ObjectValue({
                                    {"value",target.object->at(key)},
                                    {"writable",Value::Bool(!frozenObjects_.contains(target.object.get()))},
                                    {"enumerable",Value::Bool(true)},
                                    {"configurable",Value::Bool(!frozenObjects_.contains(target.object.get()))}});
                                result={descriptor,{}};
                            }
                        } else if(member=="defineProperty" || member=="defineProperties") {
                            if(target.kind!=Value::Kind::Object || !target.object ||
                               frozenObjects_.contains(target.object.get()))
                                throw std::runtime_error("Object.defineProperty target not extensible");
                            auto define=[&](const std::string& name,const Value& desc) {
                                if(desc.kind!=Value::Kind::Object || !desc.object)
                                    throw std::runtime_error("Invalid property descriptor");
                                auto found=desc.object->find("value");
                                if(found!=desc.object->end())
                                    target.object->insert_or_assign(name,found->second);
                                auto getter=desc.object->find("get");
                                if(getter!=desc.object->end() &&
                                   getter->second.kind==Value::Kind::Function)
                                    getters_[target.object.get()][name]=getter->second;
                            };
                            if(member=="defineProperty") {
                                if(args.size()<3) throw std::runtime_error("Missing property descriptor");
                                define(args[1].String(),args[2]);
                            } else {
                                if(args.size()<2 || args[1].kind!=Value::Kind::Object ||
                                   !args[1].object)
                                    throw std::runtime_error("Missing property descriptors");
                                for(const auto& [key,desc]:*args[1].object)
                                    define(key,desc);
                            }
                            result={target,{}};
                        }
                    } else if(result.value.kind==Value::Kind::Function &&
                       result.value.text=="@builtin:Array" && member=="isArray") {
                        result={Value::Bool(!args.empty() &&
                            args[0].kind==Value::Kind::Array),{}};
                    } else if(result.value.kind==Value::Kind::Function &&
                              result.value.text=="@builtin:String" &&
                              member=="fromCharCode") {
                        std::string characters;
                        for(const auto& arg:args) {
                            const unsigned cp=Uint32(arg.Number())&65535u;
                            if(cp<0x80) characters+=static_cast<char>(cp);
                            else if(cp<0x800) {
                                characters+=static_cast<char>(0xc0|(cp>>6));
                                characters+=static_cast<char>(0x80|(cp&63));
                            } else {
                                characters+=static_cast<char>(0xe0|(cp>>12));
                                characters+=static_cast<char>(0x80|((cp>>6)&63));
                                characters+=static_cast<char>(0x80|(cp&63));
                            }
                        }
                        result={Value::Str(std::move(characters)),{}};
                    } else if(result.value.kind==Value::Kind::Text &&
                       (member=="includes" || member=="startsWith" ||
                        member=="endsWith" || member=="indexOf" ||
                        member=="slice" || member=="substring" ||
                        member=="trim" || member=="toLowerCase" ||
                        member=="toUpperCase" || member=="split" ||
                        member=="replace" || member=="match" ||
                        member=="search")) {
                        const std::string input=result.value.text;
                        const std::string needle=args.empty()?"undefined":args[0].String();
                        if(member=="replace") {
                            if(args.empty()) {
                                result={Value::Str(input),{}};
                            } else if(args[0].kind==Value::Kind::RegExp &&
                                      args[0].regexp) {
                                const auto replacement=args.size()>1?args[1].String():"undefined";
                                const auto flags=args[0].regexpGlobal
                                    ?std::regex_constants::format_default
                                    :std::regex_constants::format_first_only;
                                result={Value::Str(std::regex_replace(input,
                                    *args[0].regexp,replacement,flags)),{}};
                            } else {
                                std::string changed=input;
                                const auto found=changed.find(args[0].String());
                                if(found!=std::string::npos)
                                    changed.replace(found,args[0].String().size(),
                                        args.size()>1?args[1].String():"undefined");
                                result={Value::Str(std::move(changed)),{}};
                            }
                        } else if(member=="match" || member=="search") {
                            const auto regexp=!args.empty() &&
                                args[0].kind==Value::Kind::RegExp?args[0]:
                                Value::RegExpValue(args.empty()?"":args[0].String(),"");
                            std::smatch matches;
                            const bool found=regexp.regexp &&
                                std::regex_search(input,matches,*regexp.regexp);
                            if(member=="search") result={
                                Value::Num(found?static_cast<double>(matches.position()):-1),{}};
                            else if(!found) result={
                                Value::Special(Value::Kind::Null),{}};
                            else {
                                std::vector<Value> pieces;
                                for(const auto& m:matches) pieces.push_back(Value::Str(m.str()));
                                result={Value::ArrayValue(std::move(pieces)),{}};
                            }
                        } else if(member=="includes") {
                            result={Value::Bool(input.find(needle)!=std::string::npos),{}};
                        } else if(member=="startsWith") {
                            result={Value::Bool(input.starts_with(needle)),{}};
                        } else if(member=="endsWith") {
                            result={Value::Bool(input.ends_with(needle)),{}};
                        } else if(member=="indexOf") {
                            const auto position=input.find(needle);
                            result={Value::Num(position==std::string::npos?-1.0:
                                               static_cast<double>(position)),{}};
                        } else if(member=="trim") {
                            auto begin=input.find_first_not_of(" \t\r\n");
                            if(begin==std::string::npos) result={Value::Str(""),{}};
                            else {
                                const auto end=input.find_last_not_of(" \t\r\n");
                                result={Value::Str(input.substr(begin,end-begin+1)),{}};
                            }
                        } else if(member=="toLowerCase" || member=="toUpperCase") {
                            auto transformed=input;
                            for(auto& character:transformed) {
                                const auto byte=static_cast<unsigned char>(character);
                                character=static_cast<char>(
                                    member=="toLowerCase"?std::tolower(byte):std::toupper(byte));
                            }
                            result={Value::Str(std::move(transformed)),{}};
                        } else if(member=="slice" || member=="substring") {
                            const auto length=static_cast<long long>(input.size());
                            auto normalized=[&](size_t argument,long long fallback) {
                                if(args.size()<=argument) return fallback;
                                double n=args[argument].Number();
                                if(std::isnan(n)) return 0LL;
                                if(n>=static_cast<double>(length)) return length;
                                if(n<=-static_cast<double>(length))
                                    return member=="slice"?0LL:0LL;
                                auto index=static_cast<long long>(n);
                                if(member=="slice" && index<0) index+=length;
                                return std::clamp(index,0LL,length);
                            };
                            long long begin=normalized(0,0);
                            long long end=normalized(1,length);
                            if(member=="substring" && begin>end) std::swap(begin,end);
                            result={Value::Str(input.substr(static_cast<size_t>(begin),
                                static_cast<size_t>(std::max(0LL,end-begin)))),{}};
                        } else if(member=="split") {
                            std::vector<Value> pieces;
                            if(args.empty()) {
                                pieces.push_back(Value::Str(input));
                            } else if(needle.empty()) {
                                for(char character:input)
                                    pieces.push_back(Value::Str(std::string(1,character)));
                            } else {
                                size_t start=0;
                                while(start<=input.size() && pieces.size()<10000) {
                                    const auto found=input.find(needle,start);
                                    pieces.push_back(Value::Str(input.substr(start,
                                        found==std::string::npos?std::string::npos:found-start)));
                                    if(found==std::string::npos) break;
                                    start=found+needle.size();
                                }
                            }
                            if(args.size()>1) {
                                const auto limit=args[1].Number();
                                if(std::isfinite(limit) && limit>=0 &&
                                   limit<static_cast<double>(pieces.size()))
                                    pieces.resize(static_cast<size_t>(limit));
                            }
                            result={Value::ArrayValue(std::move(pieces)),{}};
                        }
                    } else if(result.value.kind==Value::Kind::Function &&
                       (member=="call" || member=="apply")) {
                        std::vector<Value> forwarded;
                        if(member=="call") {
                            if(args.size()>1)
                                forwarded.assign(args.begin()+1,args.end());
                        } else if(args.size()>1 &&
                                  args[1].kind==Value::Kind::Array && args[1].array) {
                            forwarded=*args[1].array;
                        }
                        result={CallFunction(result.value,forwarded,
                            args.empty()?Value{}:args[0]),{}};
                    } else if(result.value.kind==Value::Kind::Json &&
                       member=="parse") {
                        if(args.empty()) throw std::runtime_error("JSON.parse requires input");
                        result={js::ParseJson(args[0].String()),{}};
                    } else if(result.value.kind==Value::Kind::Json &&
                              member=="stringify") {
                        result={args.empty()?Value{}:
                                    Value::Str(js::StringifyJson(args[0])),{}};
                    } else if(result.value.kind==Value::Kind::Math &&
                       (member=="abs" || member=="floor" || member=="ceil" ||
                        member=="round" || member=="min" || member=="max" ||
                        member=="pow" || member=="sqrt" || member=="trunc" ||
                        member=="random")) {
                        double value=args.empty()?std::nan(""):args.front().Number();
                        if(member=="random")
                            value=std::generate_canonical<double,53>(rng_);
                        else if(member=="abs") value=std::fabs(value);
                        else if(member=="floor") value=std::floor(value);
                        else if(member=="ceil") value=std::ceil(value);
                        else if(member=="trunc") value=std::trunc(value);
                        else if(member=="round") value=std::floor(value+0.5);
                        else if(member=="sqrt") value=std::sqrt(value);
                        else if(member=="pow") value=args.size()>1?std::pow(value,args[1].Number()):std::nan("");
                        else if(member=="min" || member=="max") {
                            if(args.empty()) value=member=="min"?INFINITY:-INFINITY;
                            for(const auto& argument:args) {
                                const auto next=argument.Number();
                                value=std::isnan(next)?next:
                                    member=="min"?std::min(value,next):std::max(value,next);
                            }
                        }
                        result={Value::Num(value),{}};
                    } else if(result.value.kind==Value::Kind::Document &&
                      member=="createElement") {
                        const auto name=args.empty()?std::string{}:args[0].String();
                        if(name.empty() || name.size()>128)
                            throw std::runtime_error("Invalid element tag");
                        for(unsigned char c:name)
                            if(!std::isalnum(c) && c!='-' && c!=':')
                                throw std::runtime_error("Invalid element tag");
                        auto created=std::make_unique<Node>();
                        created->tag=name;
                        auto* pointer=created.get();
                        detached_.push_back(std::move(created));
                        result={Value::NodeValue(pointer),{}};
                    } else if(result.value.kind==Value::Kind::Document &&
                              member=="createTextNode") {
                        auto created=std::make_unique<Node>();
                        created->type=Node::Type::Text;
                        created->text=args.empty()?"":args[0].String();
                        auto* pointer=created.get();
                        detached_.push_back(std::move(created));
                        result={Value::NodeValue(pointer),{}};
                    } else if(result.value.kind==Value::Kind::Document &&
                      (member=="getElementById" || member=="querySelector")) {
                        const auto argument=args.empty()?std::string{}:args[0].String();
                        const auto selector=member=="getElementById"?"#"+argument:argument;
                        result={Value::NodeValue(FindSelector(doc_.root.get(),selector)),
                                (member=="getElementById"?"id:":"selector:")+argument};
                    } else if(result.value.kind==Value::Kind::Document &&
                       (member=="querySelectorAll" ||
                        member=="getElementsByTagName" ||
                        member=="getElementsByClassName")) {
                        const auto argument=args.empty()?std::string{}:args[0].String();
                        const auto selector=member=="getElementsByClassName"
                            ? "."+argument:argument;
                        std::vector<Node*> matches;
                        FindAll(doc_.root.get(),selector,matches);
                        std::vector<Value> nodes;
                        nodes.reserve(matches.size());
                        for(Node* node:matches) nodes.push_back(Value::NodeValue(node));
                        result={Value::ArrayValue(std::move(nodes)),{}};
                    } else if(result.value.kind==Value::Kind::Array &&
                              result.value.array &&
                              (member=="map" || member=="filter" ||
                               member=="forEach" || member=="reduce")) {
                        if(args.empty() || args[0].kind!=Value::Kind::Function)
                            throw std::runtime_error("Array callback must be a function");
                        const auto source=result.value.array;
                        if(source->size()>10000)
                            throw std::runtime_error("Array callback element limit");
                        std::vector<Value> mapped;
                        mapped.reserve(source->size());
                        Value accumulator;
                        size_t begin=0;
                        if(member=="reduce") {
                            if(args.size()>1) accumulator=args[1];
                            else if(!source->empty()) accumulator=(*source)[begin++];
                            else throw std::runtime_error("Reduce of empty array");
                        }
                        for(size_t index=begin; index<source->size(); ++index) {
                            Tick();
                            const auto& item=(*source)[index];
                            std::vector<Value> callbackArguments;
                            if(member=="reduce") {
                                callbackArguments={accumulator,item,Value::Num(index),
                                                   Value::ArrayValue(*source)};
                            } else {
                                callbackArguments={item,Value::Num(index),
                                                   Value::ArrayValue(*source)};
                            }
                            const auto outcome=CallFunction(args[0],callbackArguments);
                            if(member=="reduce") accumulator=outcome;
                            else if(member=="map") mapped.push_back(outcome);
                            else if(member=="filter" && outcome.Truth())
                                mapped.push_back(item);
                        }
                        if(member=="reduce") result={accumulator,{}};
                        else if(member=="forEach") result={};
                        else result={Value::ArrayValue(std::move(mapped)),{}};
                    } else if(result.value.kind==Value::Kind::Array &&
                              result.value.array &&
                              (member=="includes" || member=="indexOf" ||
                               member=="at" || member=="slice" ||
                               member=="concat" || member=="reverse")) {
                        const auto array=result.value.array;
                        if(member=="includes" || member=="indexOf") {
                            if(args.empty()) throw std::runtime_error("Array lookup missing argument");
                            double found=-1;
                            size_t index=0;
                            if(args.size()>1 && std::isfinite(args[1].Number())) {
                                const auto start=static_cast<long long>(args[1].Number());
                                if(start<0) {
                                    const auto relative=static_cast<long long>(array->size())+start;
                                    index=relative>0?static_cast<size_t>(relative):0;
                                } else index=static_cast<size_t>(start);
                            }
                            for(;index<array->size();++index) {
                                const auto& item=(*array)[index];
                                bool equal=item.kind==args.front().kind &&
                                   (item.kind==Value::Kind::Number || item.kind==Value::Kind::Boolean
                                       ? item.number==args.front().number
                                       : item.kind==Value::Kind::Text
                                           ? item.text==args.front().text
                                           : item.kind==Value::Kind::Object
                                               ? item.object==args.front().object
                                               : item.kind==Value::Kind::Array
                                                   ? item.array==args.front().array
                                                   : item.kind==Value::Kind::Null ||
                                                     item.kind==Value::Kind::Undefined);
                                if(equal) {found=static_cast<double>(index);break;}
                            }
                            result=member=="includes"
                               ? Result{Value::Bool(found>=0),{}}
                               : Result{Value::Num(found),{}};
                        } else if(member=="at") {
                            const auto requested=args.empty()?0.0:args[0].Number();
                            const auto count=static_cast<long long>(array->size());
                            auto index=std::isfinite(requested)
                                ?static_cast<long long>(requested):count;
                            if(index<0) index+=count;
                            result={index>=0 && index<count
                                ?(*array)[static_cast<size_t>(index)]:Value{},{}};
                        } else if(member=="reverse") {
                            std::reverse(array->begin(),array->end());
                            result={Value::ArrayValue(*array),{}};
                        } else {
                            std::vector<Value> output;
                            if(member=="concat") {
                                output=*array;
                                for(const auto& addition:args) {
                                    if(addition.kind==Value::Kind::Array && addition.array)
                                        output.insert(output.end(),addition.array->begin(),addition.array->end());
                                    else output.push_back(addition);
                                }
                            } else {
                                const auto count=static_cast<long long>(array->size());
                                auto offset=[&](size_t argument,long long fallback) {
                                    if(args.size()<=argument) return fallback;
                                    const double v=args[argument].Number();
                                    if(std::isnan(v)) return 0LL;
                                    if(v>=static_cast<double>(count)) return count;
                                    if(v<=-static_cast<double>(count)) return 0LL;
                                    auto index=static_cast<long long>(v);
                                    if(index<0) index+=count;
                                    return std::clamp(index,0LL,count);
                                };
                                const auto begin=offset(0,0);
                                const auto end=offset(1,count);
                                if(end>begin)
                                    output.assign(array->begin()+begin,array->begin()+end);
                            }
                            if(output.size()>100000)
                                throw std::runtime_error("Array result exceeds element limit");
                            result={Value::ArrayValue(std::move(output)),{}};
                        }
                    } else if(result.value.kind==Value::Kind::Array &&
                              result.value.array && member=="push") {
                        for(const auto& argument:args) result.value.array->push_back(argument);
                        result={Value::Num(result.value.array->size()),{}};
                    } else if(result.value.kind==Value::Kind::Array &&
                              result.value.array && member=="pop") {
                        Value popped;
                        if(!result.value.array->empty()) {
                            popped=result.value.array->back();
                            result.value.array->pop_back();
                        }
                        result={popped,{}};
                    } else if(result.value.kind==Value::Kind::Array &&
                              result.value.array && member=="join") {
                        const auto delimiter=args.empty()?std::string(","):args[0].String();
                        std::string joined;
                        for(size_t index=0; index<result.value.array->size(); ++index) {
                            if(index) joined+=delimiter;
                            joined+=(*result.value.array)[index].String();
                        }
                        result={Value::Str(std::move(joined)),{}};
                    } else if(result.value.kind==Value::Kind::Document && member=="write") {
                        auto* body=FindTag(doc_.root.get(),"body");
                        if(!body) body=doc_.root.get();
                        for(auto& arg:args) SetNode(doc_,*body,arg.String(),true,true,&detached_);
                        ++stats_.operations;result={};
                    } else if(result.value.kind==Value::Kind::Element &&
                              result.value.element &&
                              (member=="insertAdjacentHTML" ||
                               member=="insertAdjacentText")) {
                        if(args.size()<2)
                            throw std::runtime_error("insertAdjacentHTML requires position and markup");
                        InsertAdjacent(doc_,*result.value.element,
                            args[0].String(),args[1].String(),
                            member=="insertAdjacentText");
                        ++stats_.operations;
                        result={};
                    } else if(result.value.kind==Value::Kind::Element &&
                              result.value.element && member=="setAttribute") {
                        if(args.size()>=2) {
                            result.value.element->attributes[args[0].String()]=args[1].String();
                            ++stats_.operations;
                        }
                        result={};
                    } else if(result.value.kind==Value::Kind::Element &&
                              result.value.element && member=="getAttribute") {
                        result={Value::Str(args.empty()?std::string{}:
                            result.value.element->Attribute(args[0].String())),{}};
                    } else if(result.value.kind==Value::Kind::Element &&
                              result.value.element &&
                              (member=="remove" || member=="removeChild")) {
                        Node* child=member=="remove"
                            ?result.value.element
                            :(!args.empty() && args[0].kind==Value::Kind::Element
                                ?args[0].element:nullptr);
                        if(child && child->parent &&
                           (member=="remove" ||
                            child->parent==result.value.element)) {
                            auto& siblings=child->parent->children;
                            for(auto it=siblings.begin();it!=siblings.end();++it) {
                                if(it->get()!=child) continue;
                                child->parent=nullptr;
                                detached_.push_back(std::move(*it));
                                siblings.erase(it);
                                ++stats_.operations;
                                break;
                            }
                        }
                        result=member=="removeChild"?Result{Value::NodeValue(child),{}}:
                            Result{};
                    } else if(result.value.kind==Value::Kind::Element &&
                              result.value.element && member=="appendChild") {
                        if(!args.empty() && args[0].kind==Value::Kind::Element &&
                           args[0].element) {
                            Node* child=args[0].element;
                            if(child==result.value.element)
                                throw std::runtime_error("DOM cycle");
                            for(Node* parent=result.value.element;parent;parent=parent->parent)
                                if(parent==child) throw std::runtime_error("DOM cycle");
                            std::unique_ptr<Node> owner;
                            for(auto it=detached_.begin();it!=detached_.end();++it)
                                if(it->get()==child) {
                                    owner=std::move(*it);
                                    detached_.erase(it);
                                    break;
                                }
                            if(!owner && child->parent) {
                                auto& children=child->parent->children;
                                for(auto it=children.begin();it!=children.end();++it)
                                    if(it->get()==child) {
                                        owner=std::move(*it);
                                        children.erase(it);
                                        break;
                                    }
                            }
                            if(owner) {
                                owner->parent=result.value.element;
                                result.value.element->children.push_back(std::move(owner));
                                ++stats_.operations;
                            }
                        }
                        result={};
                    } else if(result.value.kind==Value::Kind::Location &&
                              (member=="assign" || member=="replace")) {
                        if(!args.empty()) Navigate(args[0]);
                        result={};
                    } else throw std::runtime_error("Unsupported native JavaScript method: "+member+" on kind "+std::to_string(static_cast<int>(result.value.kind))+" value "+result.value.String());
                } else {
                    const auto old=result;
                    result.value=ReadProperty(old.value,member);
                    if(old.value.kind==Value::Kind::Document) {
                        result.path=member=="body"?"body":"document|"+member;
                    } else if(old.value.kind==Value::Kind::Location) result.path="location|"+member;
                    else if(old.value.kind==Value::Kind::Window && member=="location")
                        result.path="location|location";
                    else if(old.value.kind==Value::Kind::Window) {
                        result.path="object|"+member;
                        result.propertyObject=globalProperties_;
                        result.propertyNode=nullptr;
                    }
                    else if(old.value.kind==Value::Kind::Element) {
                        result.path=old.path+"|"+member;
                        result.propertyNode=old.value.element;
                        result.propertyObject.reset();
                    } else if(old.value.kind==Value::Kind::Style ||
                              old.value.kind==Value::Kind::ClassList) {
                        result.path=(old.value.kind==Value::Kind::Style
                                      ?"style|":"classList|")+member;
                        result.propertyNode=old.value.element;
                        result.propertyObject.reset();
                    } else if(old.value.kind==Value::Kind::Object) {
                        result.path="object|"+member;
                        result.propertyObject=old.value.object;
                        result.propertyNode=nullptr;
                    } else {
                        result.path.clear();
                        result.propertyNode=nullptr;
                        result.propertyObject.reset();
                    }
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
        if(s==",") return 0;
        if(s=="=" || s=="+=" || s=="-=" || s=="*=" ||
           s=="/=" || s=="%=" || s=="&=" || s=="|=" ||
           s=="^=" || s=="<<=" || s==">>=" || s==">>>=") return 1;
        if(s=="||" || s=="??") return 2;
        if(s=="&&") return 3;
        if(s=="|") return 4;
        if(s=="^") return 5;
        if(s=="&") return 6;
        if(s=="==" || s=="===" || s=="!=" || s=="!==") return 7;
        if(s=="<" || s==">" || s=="<=" || s==">=") return 8;
        if(s=="<<" || s==">>" || s==">>>") return 9;
        if(s=="+" || s=="-") return 10;
        if(s=="*" || s=="/" || s=="%") return 11;
        if(s=="**") return 12;
        return -1;
    }
    void SkipOperand(int precedence) {
        unsigned parens=0,brackets=0,braces=0;
        while(Peek().kind!=TokenKind::End) {
            const auto& token=Peek().text;
            if(!parens && !brackets && !braces) {
                if(token==";" || token=="," || token==")" ||
                   token=="]" || token=="}" || token==":" || token=="?")
                    return;
                const int rank=Priority(token);
                if(rank>=0 && rank<precedence) return;
            }
            if(token=="(") ++parens;
            else if(token=="[") ++brackets;
            else if(token=="{") ++braces;
            else if(token==")") {
                if(!parens) return;
                --parens;
            } else if(token=="]") {
                if(!brackets) return;
                --brackets;
            } else if(token=="}") {
                if(!braces) return;
                --braces;
            }
            ++pos_;
        }
    }
    void SkipConditionalBranch(bool untilColon) {
        unsigned parens=0,brackets=0,braces=0,ternaries=0;
        while(Peek().kind!=TokenKind::End) {
            const auto& token=Peek().text;
            if(!parens && !brackets && !braces) {
                if(token=="?") ++ternaries;
                else if(token==":") {
                    if(ternaries) --ternaries;
                    else if(untilColon) return;
                }
                if(!untilColon && !ternaries &&
                   (token==";" || token=="," || token==")" ||
                    token=="]" || token=="}" || token==":")) return;
            }
            if(token=="(") ++parens;
            else if(token=="[") ++brackets;
            else if(token=="{") ++braces;
            else if(token==")") {
                if(!parens) return;
                --parens;
            } else if(token=="]") {
                if(!brackets) return;
                --brackets;
            } else if(token=="}") {
                if(!braces) return;
                --braces;
            }
            ++pos_;
        }
    }
    Result Expression(int min=1) {
        auto left=Postfix();
        while(true) {
            const auto op=Peek().text;
            const int rank=Priority(op);
            if(rank<min) break;
            ++pos_;
            const bool assignment=rank==1;
            const auto a=left.value;
            const bool skip=(op=="&&" && !a.Truth()) ||
                            (op=="||" && a.Truth()) ||
                            (op=="??" && a.kind!=Value::Kind::Undefined &&
                                          a.kind!=Value::Kind::Null);
            if(skip) {
                SkipOperand(rank+1);
                left={a,{}};
                continue;
            }
            auto right=Expression(rank+((assignment || op=="**")?0:1));
            auto b=right.value;
            if(assignment) {
                Value value=b;
                if(op=="+=") value=(a.kind==Value::Kind::Text||b.kind==Value::Kind::Text)
                    ?Value::Str(a.String()+b.String()):Value::Num(a.Number()+b.Number());
                else if(op=="-=") value=Value::Num(a.Number()-b.Number());
                else if(op=="*=") value=Value::Num(a.Number()*b.Number());
                else if(op=="/=") value=Value::Num(a.Number()/b.Number());
                else if(op=="%=") value=Value::Num(std::fmod(a.Number(),b.Number()));
                else if(op=="&=") value=Value::Num(Int32(a.Number()) & Int32(b.Number()));
                else if(op=="|=") value=Value::Num(Int32(a.Number()) | Int32(b.Number()));
                else if(op=="^=") value=Value::Num(Int32(a.Number()) ^ Int32(b.Number()));
                else if(op=="<<=") value=Value::Num(
                    static_cast<std::int32_t>(
                        Uint32(a.Number()) << (Uint32(b.Number())&31)));
                else if(op==">>=") value=Value::Num(
                    Int32(a.Number()) >> (Uint32(b.Number())&31));
                else if(op==">>>=") value=Value::Num(
                    Uint32(a.Number()) >> (Uint32(b.Number())&31));
                Write(left,value);left={value,{}};continue;
            }
            if(op==",") left.value=b;
            else if(op=="+") left.value=(a.kind==Value::Kind::Text||b.kind==Value::Kind::Text)
                ?Value::Str(a.String()+b.String()):Value::Num(a.Number()+b.Number());
            else if(op=="-") left.value=Value::Num(a.Number()-b.Number());
            else if(op=="*") left.value=Value::Num(a.Number()*b.Number());
            else if(op=="/") left.value=Value::Num(a.Number()/b.Number());
            else if(op=="%") left.value=Value::Num(std::fmod(a.Number(),b.Number()));
            else if(op=="**") left.value=Value::Num(std::pow(a.Number(),b.Number()));
            else if(op=="&") left.value=Value::Num(Int32(a.Number()) & Int32(b.Number()));
            else if(op=="|") left.value=Value::Num(Int32(a.Number()) | Int32(b.Number()));
            else if(op=="^") left.value=Value::Num(Int32(a.Number()) ^ Int32(b.Number()));
            else if(op=="<<") left.value=Value::Num(
                static_cast<std::int32_t>(
                    Uint32(a.Number()) << (Uint32(b.Number())&31)));
            else if(op==">>") left.value=Value::Num(
                Int32(a.Number()) >> (Uint32(b.Number())&31));
            else if(op==">>>") left.value=Value::Num(
                Uint32(a.Number()) >> (Uint32(b.Number())&31));
            else if(op=="<") left.value=Value::Bool(a.Number()<b.Number());
            else if(op==">") left.value=Value::Bool(a.Number()>b.Number());
            else if(op=="<=") left.value=Value::Bool(a.Number()<=b.Number());
            else if(op==">=") left.value=Value::Bool(a.Number()>=b.Number());
            else if(op=="===" || op=="!==") {
                const bool equal = a.kind==b.kind &&
                    (a.kind==Value::Kind::Text ? a.text==b.text :
                     a.kind==Value::Kind::Number || a.kind==Value::Kind::Boolean
                        ? a.number==b.number :
                     a.kind==Value::Kind::Undefined || a.kind==Value::Kind::Null ? true :
                     a.kind==Value::Kind::Element ? a.element==b.element :
                     a.kind==Value::Kind::Array ? a.array==b.array :
                     a.kind==Value::Kind::Object ? a.object==b.object :
                     a.kind==Value::Kind::Document ||
                     a.kind==Value::Kind::Window ||
                     a.kind==Value::Kind::Location ? true :
                     a.kind==Value::Kind::Function ? a.text==b.text : false);
                left.value=Value::Bool(op=="==="?equal:!equal);
            }
            else if(op=="==" || op=="!=") {
                const bool nullishA = a.kind==Value::Kind::Undefined || a.kind==Value::Kind::Null;
                const bool nullishB = b.kind==Value::Kind::Undefined || b.kind==Value::Kind::Null;
                const bool equal = nullishA || nullishB ? nullishA && nullishB :
                    a.kind==b.kind ? a.String()==b.String() :
                    a.Number()==b.Number();
                left.value=Value::Bool(op=="=="?equal:!equal);
            }
            else if(op=="&&") left.value=a.Truth()?b:a;
            else if(op=="||") left.value=a.Truth()?a:b;
            else if(op=="??") left.value=(a.kind==Value::Kind::Undefined || a.kind==Value::Kind::Null)?b:a;
            left.path.clear();
        }
        if(min<=2 && Match("?")) {
            const bool condition=left.value.Truth();
            if(condition) {
                auto selected=Expression();
                Expect(":");
                SkipConditionalBranch(false);
                return {selected.value,{}};
            }
            SkipConditionalBranch(true);
            Expect(":");
            auto selected=Expression(2);
            return {selected.value,{}};
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
        if(Peek().kind==TokenKind::Identifier &&
           pos_+1<tokens_.size() && tokens_[pos_+1].text==":") {
            const auto label=tokens_[pos_++].text;
            Expect(":");
            const size_t start=pos_;
            SkipStatement();
            const size_t after=pos_;
            pos_=start;
            try { Statement(); }
            catch(const LabelBreak& signal) {
                if(signal.label!=label) throw;
            }
            pos_=after;
            return;
        }
        if(Match("{")) {
            while(!Match("}")) {
                if(Peek().kind==TokenKind::End) throw std::runtime_error("Unterminated JS block");
                Statement();
            }
            return;
        }
        if(Match("function")) {
            if(Peek().kind!=TokenKind::Identifier)
                throw std::runtime_error("Function name expected");
            const std::string name=tokens_[pos_++].text;
            Expect("(");
            Function function;
            if(callDepth_>0)
                function.captured=std::make_shared<std::unordered_map<std::string,Value>>(vars_);
            if(!Match(")")) {
                do {
                    if(Peek().kind!=TokenKind::Identifier)
                        throw std::runtime_error("Parameter name expected");
                    function.parameters.push_back(tokens_[pos_++].text);
                } while(Match(","));
                Expect(")");
            }
            if(Peek().text!="{") throw std::runtime_error("Function body expected");
            const size_t begin=pos_;
            int nesting=0;
            do {
                if(Peek().kind==TokenKind::End)
                    throw std::runtime_error("Unterminated function body");
                if(Peek().text=="{") ++nesting;
                else if(Peek().text=="}") --nesting;
                ++pos_;
            } while(nesting>0);
            function.body.assign(tokens_.begin()+static_cast<std::ptrdiff_t>(begin),
                                 tokens_.begin()+static_cast<std::ptrdiff_t>(pos_));
            function.body.push_back(Token{TokenKind::End,"",0,0});
            functions_.insert_or_assign(name,std::move(function));
            return;
        }
        if(Match("break")) {
            if(Peek().kind==TokenKind::Identifier) {
                const auto label=tokens_[pos_++].text;
                (void)Match(";");
                throw LabelBreak{label};
            }
            if(!loopDepth_ && !switchDepth_)
                throw std::runtime_error("break outside loop or switch");
            (void)Match(";");
            throw BreakSignal{};
        }
        if(Match("continue")) {
            if(!loopDepth_) throw std::runtime_error("continue outside loop");
            (void)Match(";");
            throw ContinueSignal{};
        }
        if(Match("throw")) {
            auto error=Expression().value;
            (void)Match(";");
            throw ThrowSignal{error};
        }
        if(Match("try")) {
            const size_t tryBody=pos_;
            SkipStatement();
            std::string binding;
            size_t catchBody=0;
            bool hasCatch=false,hasFinally=false;
            if(Match("catch")) {
                hasCatch=true;
                if(Match("(")) {
                    if(Peek().kind!=TokenKind::Identifier)
                        throw std::runtime_error("Catch identifier required");
                    binding=tokens_[pos_++].text;
                    Expect(")");
                }
                catchBody=pos_;
                SkipStatement();
            }
            size_t finallyBody=0;
            if(Match("finally")) {
                hasFinally=true;
                finallyBody=pos_;
                SkipStatement();
            }
            if(!hasCatch && !hasFinally)
                throw std::runtime_error("try must have catch or finally");
            const size_t after=pos_;
            std::exception_ptr pending;
            pos_=tryBody;
            try {
                Statement();
            } catch(const ThrowSignal& error) {
                if(!hasCatch) pending=std::current_exception();
                else {
                    const auto previous=vars_.find(binding);
                    const bool hadPrevious=!binding.empty() && previous!=vars_.end();
                    const Value oldValue=hadPrevious?previous->second:Value{};
                    if(!binding.empty()) vars_[binding]=error.value;
                    pos_=catchBody;
                    try {
                        Statement();
                    } catch(...) {
                        pending=std::current_exception();
                    }
                    if(!binding.empty()) {
                        if(hadPrevious) vars_[binding]=oldValue;
                        else vars_.erase(binding);
                    }
                }
            } catch(...) {
                pending=std::current_exception();
            }
            if(hasFinally) {
                pos_=finallyBody;
                try {
                    Statement();
                } catch(...) {
                    pending=std::current_exception();
                }
            }
            pos_=after;
            if(pending) std::rethrow_exception(pending);
            return;
        }
        if(Match("return")) {
            Value value;
            if(Peek().text!=";" && Peek().text!="}")
                value=Expression().value;
            (void)Match(";");
            throw ReturnSignal{value};
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
        if(Match("switch")) {
            Expect("(");
            const auto discriminant=Expression().value;
            Expect(")");
            Expect("{");
            struct CaseClause {
                size_t label=0;
                size_t body=0;
                bool isDefault=false;
            };
            std::vector<CaseClause> clauses;
            size_t scan=pos_;
            unsigned braces=0;
            size_t switchEnd=tokens_.size();
            while(scan<tokens_.size() && tokens_[scan].kind!=TokenKind::End) {
                const auto& current=tokens_[scan];
                if(current.kind==TokenKind::Symbol && current.text=="{") {
                    ++braces;
                } else if(current.kind==TokenKind::Symbol && current.text=="}") {
                    if(!braces) {switchEnd=scan;break;}
                    --braces;
                } else if(!braces && current.kind==TokenKind::Identifier &&
                          (current.text=="case" || current.text=="default")) {
                    CaseClause clause;
                    clause.isDefault=current.text=="default";
                    clause.label=scan+1;
                    size_t separator=clause.label;
                    unsigned nesting=0;
                    while(separator<tokens_.size()) {
                        const auto& token=tokens_[separator];
                        if(token.kind==TokenKind::Symbol) {
                            if(token.text=="(" || token.text=="[") ++nesting;
                            else if(token.text==")" || token.text=="]") {
                                if(nesting) --nesting;
                            } else if(token.text==":" && nesting==0) break;
                        }
                        ++separator;
                    }
                    if(separator==tokens_.size())
                        throw std::runtime_error("Unterminated JavaScript switch case");
                    clause.body=separator+1;
                    clauses.push_back(clause);
                    scan=separator;
                }
                ++scan;
            }
            if(switchEnd==tokens_.size())
                throw std::runtime_error("Unterminated JavaScript switch");
            size_t chosen=clauses.size();
            size_t fallback=clauses.size();
            for(size_t i=0;i<clauses.size();++i) {
                if(clauses[i].isDefault) {fallback=i;continue;}
                pos_=clauses[i].label;
                const auto candidate=Expression(2).value;
                const bool equal=candidate.kind==discriminant.kind &&
                    (candidate.kind==Value::Kind::Number ||
                     candidate.kind==Value::Kind::Boolean
                        ?candidate.number==discriminant.number
                        :candidate.kind==Value::Kind::Text
                            ?candidate.text==discriminant.text
                            :candidate.kind==Value::Kind::Null ||
                             candidate.kind==Value::Kind::Undefined
                                ?true
                                :candidate.kind==Value::Kind::Object
                                    ?candidate.object==discriminant.object
                                    :candidate.kind==Value::Kind::Array
                                        ?candidate.array==discriminant.array
                                        :candidate.element==discriminant.element);
                if(equal) {chosen=i;break;}
            }
            if(chosen==clauses.size()) chosen=fallback;
            ++switchDepth_;
            try {
                if(chosen<clauses.size()) {
                    pos_=clauses[chosen].body;
                    for(size_t i=chosen;i<clauses.size();++i) {
                        const size_t end=i+1<clauses.size()
                            ?clauses[i+1].label-1:switchEnd;
                        while(pos_<end) {
                            Tick();
                            Statement();
                        }
                        if(i+1<clauses.size()) pos_=clauses[i+1].body;
                    }
                }
            } catch(const BreakSignal&) {
                // An unlabelled break exits the innermost switch.
            } catch(...) {
                --switchDepth_;
                throw;
            }
            --switchDepth_;
            pos_=switchEnd+1;
            return;
        }
        if(Match("do")) {
            const size_t body=pos_;
            SkipStatement();
            const size_t endOfBody=pos_;
            Expect("while");
            Expect("(");
            const size_t condition=pos_;
            unsigned parens=0;
            while(Peek().kind!=TokenKind::End) {
                if(Peek().text=="(") ++parens;
                else if(Peek().text==")") {
                    if(!parens) break;
                    --parens;
                }
                ++pos_;
            }
            Expect(")");
            (void)Match(";");
            const size_t after=pos_;
            ++loopDepth_;
            try {
                for(unsigned count=0;count<20000;++count) {
                    Tick();
                    pos_=body;
                    bool stopped=false;
                    try {Statement();}
                    catch(const ContinueSignal&) {}
                    catch(const BreakSignal&) {stopped=true;}
                    if(stopped) break;
                    pos_=condition;
                    if(!Expression().value.Truth()) break;
                }
            } catch(...) {--loopDepth_;throw;}
            --loopDepth_;
            pos_=after;
            (void)endOfBody;
            return;
        }
        if(Match("while")) {
            Expect("(");
            const size_t condition=pos_;
            while(Peek().text!=")" && Peek().kind!=TokenKind::End) ++pos_;
            Expect(")");
            const size_t body=pos_;
            SkipStatement();
            const size_t after=pos_;
            ++loopDepth_;
            try {
                for(unsigned loops=0;loops<20000;++loops) {
                    Tick();
                    pos_=condition;
                    if(!Expression().value.Truth()) break;
                    pos_=body;
                    try {Statement();}
                    catch(const ContinueSignal&) {continue;}
                    catch(const BreakSignal&) {break;}
                }
            } catch(...) {--loopDepth_;throw;}
            --loopDepth_;
            pos_=after;
            return;
        }
        if(Match("for")) {
            Expect("(");
            if((Peek().text=="let" || Peek().text=="const" ||
                Peek().text=="var") && pos_+2<tokens_.size() &&
               tokens_[pos_+1].kind==TokenKind::Identifier &&
               (tokens_[pos_+2].text=="of" ||
                tokens_[pos_+2].text=="in")) {
                ++pos_;
                const std::string binding=tokens_[pos_++].text;
                const bool enumerate=Match("in");
                if(!enumerate) Expect("of");
                const auto iterable=Expression().value;
                Expect(")");
                std::vector<Value> values;
                if(enumerate) {
                    if(iterable.kind==Value::Kind::Object && iterable.object) {
                        for(const auto& [key,_]:*iterable.object)
                            values.push_back(Value::Str(key));
                    } else if(iterable.kind==Value::Kind::Array && iterable.array) {
                        for(size_t index=0;index<iterable.array->size();++index)
                            values.push_back(Value::Str(std::to_string(index)));
                    } else if(iterable.kind==Value::Kind::Text) {
                        for(size_t index=0;index<iterable.text.size();++index)
                            values.push_back(Value::Str(std::to_string(index)));
                    } else if(iterable.kind!=Value::Kind::Null &&
                              iterable.kind!=Value::Kind::Undefined)
                        throw std::runtime_error("for..in value not enumerable");
                } else if(iterable.kind==Value::Kind::Array && iterable.array)
                    values=*iterable.array;
                else if(iterable.kind==Value::Kind::Text) {
                    for(char c:iterable.text)
                        values.push_back(Value::Str(std::string(1,c)));
                } else throw std::runtime_error("for..of value not iterable");
                if(values.size()>20000)
                    throw std::runtime_error("for..of iteration limit");
                const size_t body=pos_;
                SkipStatement();
                const size_t after=pos_;
                const auto previous=vars_.find(binding);
                const bool existed=previous!=vars_.end();
                const Value oldValue=existed?previous->second:Value{};
                ++loopDepth_;
                try {
                    for(const auto& value:values) {
                        Tick();
                        vars_[binding]=value;
                        pos_=body;
                        try {Statement();}
                        catch(const ContinueSignal&) {continue;}
                        catch(const BreakSignal&) {break;}
                    }
                } catch(...) {
                    --loopDepth_;
                    if(existed) vars_[binding]=oldValue;
                    else vars_.erase(binding);
                    throw;
                }
                --loopDepth_;
                if(existed) vars_[binding]=oldValue;
                else vars_.erase(binding);
                pos_=after;
                return;
            }
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
            ++loopDepth_;
            try {
                for(unsigned n=0;n<20000;++n) {
                    Tick();
                    pos_=test;
                    if(!Match(";") && !Expression(2).value.Truth()) break;
                    pos_=body;
                    try {Statement();}
                    catch(const ContinueSignal&) {}
                    catch(const BreakSignal&) {break;}
                    pos_=update;
                    if(Peek().text!=")") (void)Expression();
                }
            } catch(...) {--loopDepth_;throw;}
            --loopDepth_;
            pos_=after;
            return;
        }
        Expression();
        (void)Match(";");
    }
public:
    Interpreter(Document& d,std::wstring_view base,Execution& stats):
        doc_(d),base_(base),stats_(stats) {}
    void FlushReadyCallbacks() {
        auto fire=[&](const std::string& name) {
            auto found=eventListeners_.find(name);
            if(found==eventListeners_.end()) return;
            const auto callbacks=std::move(found->second);
            eventListeners_.erase(found);
            for(const auto& callback:callbacks) {
                if(!stats_.navigation.empty()) return;
                try {
                    Tick();
                    CallFunction(callback,{});
                    ++stats_.operations;
                } catch(const std::exception& error) {
                    if(stats_.lastError.empty())
                        stats_.lastError=std::string("Event ")+name+": "+error.what();
                    ++stats_.unsupported;
                }
            }
        };
        fire("DOMContentLoaded");
        fire("load");
        const auto callbacks=std::move(scheduledCallbacks_);
        scheduledCallbacks_.clear();
        for(const auto& callback:callbacks) {
            if(!stats_.navigation.empty()) break;
            if(callback.kind!=Value::Kind::Function) continue;
            try {
                Tick();
                CallFunction(callback,{});
                ++stats_.operations;
            } catch(const std::exception& error) {
                if(stats_.lastError.empty())
                    stats_.lastError=std::string("Scheduled callback: ")+error.what();
                ++stats_.unsupported;
            }
        }
    }
    bool Run(const std::string& source) {
        try {
            tokens_=js::Tokenize(source);
            pos_=0;
            while(Peek().kind!=TokenKind::End && stats_.navigation.empty()) Statement();
            return true;
        } catch(const std::exception& error) {
            if(stats_.lastError.empty()) {
                std::string diagnostic=error.what();
                // Source positions remain attached to tokens inside nested
                // functions, including scripts with many minified statements.
                // Emit a narrow, printable context rather than the page body.
                if(!tokens_.empty()) {
                    const size_t index=std::min(pos_,tokens_.size()-1);
                    const auto offset=tokens_[index].offset;
                    diagnostic+=" | token "+std::to_string(index)+
                                " byte "+std::to_string(offset)+" | ";
                    const size_t first=index>4?index-4:0;
                    const size_t last=std::min(tokens_.size(),index+5);
                    for(size_t cursor=first;cursor<last;++cursor) {
                        if(cursor==index) diagnostic+=" [";
                        const auto& value=tokens_[cursor].text;
                        const size_t length=std::min(value.size(),static_cast<size_t>(24));
                        for(size_t i=0;i<length;++i) {
                            const unsigned char ch=static_cast<unsigned char>(value[i]);
                            diagnostic+=ch>=32 && ch<127?static_cast<char>(ch):'?';
                        }
                        if(value.size()>length) diagnostic+="...";
                        if(cursor==index) diagnostic+="]";
                        diagnostic+=' ';
                    }
                }
                stats_.lastError=std::move(diagnostic);
            }
            return false;
        } catch(const ThrowSignal& error) {
            if(stats_.lastError.empty())
                stats_.lastError="Uncaught JavaScript exception: "+error.value.String();
            return false;
        } catch(...) {
            if(stats_.lastError.empty())
                stats_.lastError="Unsupported JavaScript execution failure";
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
    std::vector<ScriptSource> scripts;
    CollectScripts(*document.root,baseUrl,external,scripts);
    Interpreter interpreter(document,baseUrl,report);
    for(const auto& script:scripts) {
        ++report.scripts;
        if(!interpreter.Run(script.source)) {
            ++report.unsupported;
            if(!report.lastError.empty() && !report.lastError.starts_with("Script "))
                report.lastError="Script "+script.url+": "+report.lastError;
        }
        if(!report.navigation.empty()) break;
    }
    if(report.navigation.empty()) interpreter.FlushReadyCallbacks();
    return report;
}
} // namespace ac::script
