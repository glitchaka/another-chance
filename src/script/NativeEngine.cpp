#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include "script/JsValue.h"
#include "script/JsJson.h"
#include "util/Url.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <exception>
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
    struct Function {
        std::vector<std::string> parameters;
        std::vector<Token> body;
        std::shared_ptr<std::unordered_map<std::string,Value>> captured;
    };
    std::unordered_map<std::string,Function> functions_;
    struct ReturnSignal { Value value; };
    struct ThrowSignal { Value value; };
    struct BreakSignal {};
    struct LabelBreak { std::string label; };
    struct ContinueSignal {};
    unsigned loopDepth_=0;
    unsigned lambdaCounter_=0;
    unsigned callDepth_=0;
    std::shared_ptr<std::unordered_map<std::string,Value>> globalProperties_ =
        std::make_shared<std::unordered_map<std::string,Value>>();
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
            auto property=base.object->find(name);
            return property==base.object->end()?Value{}:property->second;
        }
        if(base.kind==Value::Kind::Text && name=="length") return Value::Num(base.text.size());
        if(base.kind==Value::Kind::Array && base.array && name=="length")
            return Value::Num(base.array->size());
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
        if(target.propertyObject) {
            const auto delimiter=target.path.rfind('|');
            if(delimiter==std::string::npos) throw std::runtime_error("Invalid object property");
            (*target.propertyObject)[target.path.substr(delimiter+1)]=value;
            return;
        }
        if(target.propertyNode) {
            const auto delimiter=target.path.rfind('|');
            if(delimiter==std::string::npos) throw std::runtime_error("Invalid element property");
            const auto member=target.path.substr(delimiter+1);
            if(member=="innerHTML" || member=="textContent" || member=="innerText") {
                SetNode(doc_,*target.propertyNode,value.String(),member=="innerHTML");
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
            SetNode(doc_,*node,value.String(),member=="innerHTML");++stats_.operations;
        } else if(member=="value" || member=="id" || member=="className") {
            node->attributes[member=="className"?"class":member]=value.String();
            ++stats_.operations;
        }
    }
    Result ArrowFunction(std::vector<std::string> parameters) {
        Expect("=>");
        Function fn;
        fn.parameters=std::move(parameters);
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
        if(Match("!")) return {Value::Bool(!Primary().value.Truth()),{}};
        if(Match("typeof")) {
            const auto value=Primary().value;
            switch(value.kind) {
                case Value::Kind::Undefined: return {Value::Str("undefined"),{}};
                case Value::Kind::Number: return {Value::Str("number"),{}};
                case Value::Kind::Text: return {Value::Str("string"),{}};
                case Value::Kind::Boolean: return {Value::Str("boolean"),{}};
                case Value::Kind::Function: return {Value::Str("function"),{}};
                default: return {Value::Str("object"),{}};
            }
        }
        if(Match("~")) return {Value::Num(static_cast<double>(~Int32(Primary().value.Number()))),{}};
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
            if(name=="null") return {Value::Special(Value::Kind::Null),{}};
            if(name=="undefined") return {};
            if(name=="document") return {Value::Special(Value::Kind::Document),"document"};
            if(name=="window" || name=="self" || name=="globalThis" || name=="this")
                return {Value::Special(Value::Kind::Window),"window"};
            if(name=="location") return {Value::Special(Value::Kind::Location),"location"};
            if(name=="NaN") return {Value::Num(std::nan("")),{}};
            if(name=="Infinity") return {Value::Num(INFINITY),{}};
            if(name=="Math") return {Value::Special(Value::Kind::Math),{}};
            if(name=="JSON") return {Value::Special(Value::Kind::Json),{}};
            if(name=="String" || name=="Number" || name=="Boolean" ||
               name=="parseInt" || name=="parseFloat" || name=="isNaN" ||
               name=="eval") {
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
    Value CallFunction(const Value& value,const std::vector<Value>& args) {
        const auto first = args.empty()?Value{}:args.front();
        if(value.text=="@builtin:String") return Value::Str(first.String());
        if(value.text=="@builtin:Number") return Value::Num(args.empty()?0:first.Number());
        if(value.text=="@builtin:Boolean") return Value::Bool(first.Truth());
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
        loopDepth_=0;
        const Function function=found->second;
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
                if(result.value.kind!=Value::Kind::Function)
                    throw std::runtime_error("Attempt to call non-function");
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
                    if(result.value.kind==Value::Kind::Text &&
                       (member=="includes" || member=="startsWith" ||
                        member=="endsWith" || member=="indexOf" ||
                        member=="slice" || member=="substring" ||
                        member=="trim" || member=="toLowerCase" ||
                        member=="toUpperCase" || member=="split")) {
                        const std::string input=result.value.text;
                        const std::string needle=args.empty()?"undefined":args[0].String();
                        if(member=="includes") {
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
                        result={CallFunction(result.value,forwarded),{}};
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
                        member=="pow" || member=="sqrt" || member=="trunc")) {
                        double value=args.empty()?std::nan(""):args.front().Number();
                        if(member=="abs") value=std::fabs(value);
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
                        for(auto& arg:args) SetNode(doc_,*body,arg.String(),true,true);
                        ++stats_.operations;result={};
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
                    } else throw std::runtime_error("Unsupported native JavaScript method");
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
            if(!loopDepth_) throw std::runtime_error("break outside loop");
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
    bool Run(const std::string& source) {
        try {
            tokens_=js::Tokenize(source);
            pos_=0;
            while(Peek().kind!=TokenKind::End && stats_.navigation.empty()) Statement();
            return true;
        } catch(const std::exception& error) {
            stats_.lastError=error.what();
            return false;
        } catch(const ThrowSignal& error) {
            stats_.lastError="Uncaught JavaScript exception: "+error.value.String();
            return false;
        } catch(...) {
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
