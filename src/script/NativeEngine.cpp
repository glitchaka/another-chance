#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include "util/Url.h"
#include <algorithm>
#include <cctype>
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
    enum class Kind { Undefined, Null, Number, Text, Boolean, Element, Document, Location, Window, Array, Function, Object, Math };
    Kind kind=Kind::Undefined;
    double number=0;
    std::string text;
    Node* element=nullptr;
    std::shared_ptr<std::vector<Value>> array;
    std::shared_ptr<std::unordered_map<std::string,Value>> object;
    static Value ObjectValue(std::unordered_map<std::string,Value> props) {
        Value v;v.kind=Kind::Object;
        v.object=std::make_shared<std::unordered_map<std::string,Value>>(std::move(props));
        return v;
    }
    static Value ArrayValue(std::vector<Value> items) {
        Value v;v.kind=Kind::Array;
        v.array=std::make_shared<std::vector<Value>>(std::move(items));
        return v;
    }
    static Value Num(double n) {Value v;v.kind=Kind::Number;v.number=n;return v;}
    static Value Str(std::string s) {Value v;v.kind=Kind::Text;v.text=std::move(s);return v;}
    static Value Bool(bool b) {Value v;v.kind=Kind::Boolean;v.number=b?1:0;return v;}
    static Value NodeValue(Node* n) {
        Value v;
        if(!n) {v.kind=Kind::Null;return v;}
        v.kind=Kind::Element;v.element=n;return v;
    }
    static Value Special(Kind k) {Value v;v.kind=k;return v;}
    std::string String() const {
        if(kind==Kind::Text) return text;
        if(kind==Kind::Boolean) return number?"true":"false";
        if(kind==Kind::Number) {
            if(std::isnan(number)) return "NaN";
            if(std::isinf(number)) return std::signbit(number)?"-Infinity":"Infinity";
            if(number==0) return "0";
            std::ostringstream s;s<<std::setprecision(15)<<number;return s.str();
        }
        if(kind==Kind::Array && array) {
            std::string result;
            for(const auto& item:*array) {
                if(!result.empty()) result += ",";
                result += item.kind==Kind::Undefined?"":item.String();
            }
            return result;
        }
        if(kind==Kind::Null) return "null";
        if(kind==Kind::Undefined) return "undefined";
        return "[object Object]";
    }
    double Number() const {
        if(kind==Kind::Number || kind==Kind::Boolean) return number;
        if(kind==Kind::Null) return 0;
        if(kind==Kind::Text) {
            char* end=nullptr;
            double x=std::strtod(text.c_str(),&end);
            if(end!=text.c_str() && *end==0) return x;
            if(text.empty()) return 0;
        }
        return std::nan("");
    }
    bool Truth() const {
        if(kind==Kind::Undefined || kind==Kind::Null) return false;
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
    struct Function { std::vector<std::string> parameters; std::vector<Token> body; };
    std::unordered_map<std::string,Function> functions_;
    struct ReturnSignal { Value value; };
    struct ThrowSignal { Value value; };
    struct BreakSignal {};
    struct ContinueSignal {};
    unsigned loopDepth_=0;
    unsigned callDepth_=0;
    std::vector<std::unique_ptr<Node>> detached_;
    std::vector<Token> tokens_;
    size_t pos_=0;
    unsigned steps_=0;
    struct Result {
        Value value;
        std::string path;
        Node* propertyNode=nullptr;
        std::shared_ptr<std::unordered_map<std::string,Value>> propertyObject;
    };
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
            if(name=="Math") return Value::Special(Value::Kind::Math);
            if(name=="location") return Value::Special(Value::Kind::Location);
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
            if(name=="window") return {Value::Special(Value::Kind::Window),"window"};
            if(name=="location") return {Value::Special(Value::Kind::Location),"location"};
            if(name=="NaN") return {Value::Num(std::nan("")),{}};
            if(name=="Infinity") return {Value::Num(INFINITY),{}};
            if(name=="Math") return {Value::Special(Value::Kind::Math),{}};
            if(name=="String" || name=="Number" || name=="Boolean" ||
               name=="parseInt" || name=="parseFloat" || name=="isNaN") {
                auto builtin=Value::Special(Value::Kind::Function);
                builtin.text="@builtin:"+name;
                return {builtin,{}};
            }
            auto it=vars_.find(name);
            if(it!=vars_.end()) return {it->second,"$"+name};
            if(functions_.contains(name)) {
                auto callable=Value::Special(Value::Kind::Function);
                callable.text=name;
                return {callable,{}};
            }
            return {Value{},"$"+name};
        }
        throw std::runtime_error("Unsupported JavaScript primary expression");
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
            } else if(Match(".")) {
                if(Peek().kind!=TokenKind::Identifier) throw std::runtime_error("Expected member name");
                const auto member=tokens_[pos_++].text;
                if(Match("(")) {
                    std::vector<Value> args;
                    if(!Match(")")) {
                        do {args.push_back(Expression(2).value);} while(Match(","));
                        Expect(")");
                    }
                    if(result.value.kind==Value::Kind::Math &&
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
            else if(op=="===" || op=="!==") {
                const bool equal = a.kind==b.kind &&
                    (a.kind==Value::Kind::Text ? a.text==b.text :
                     a.kind==Value::Kind::Number || a.kind==Value::Kind::Boolean
                        ? a.number==b.number :
                     a.kind==Value::Kind::Undefined || a.kind==Value::Kind::Null ? true :
                     a.kind==Value::Kind::Element ? a.element==b.element : false);
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
        if(Match("function")) {
            if(Peek().kind!=TokenKind::Identifier)
                throw std::runtime_error("Function name expected");
            const std::string name=tokens_[pos_++].text;
            Expect("(");
            Function function;
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
            Expect("catch");
            std::string binding;
            if(Match("(")) {
                if(Peek().kind!=TokenKind::Identifier)
                    throw std::runtime_error("Catch identifier required");
                binding=tokens_[pos_++].text;
                Expect(")");
            }
            const size_t catchBody=pos_;
            SkipStatement();
            const size_t afterCatch=pos_;
            pos_=tryBody;
            try {
                Statement();
            } catch(const ThrowSignal& error) {
                const auto saved=vars_.find(binding);
                const bool hadPrevious=!binding.empty() && saved!=vars_.end();
                const Value previous=hadPrevious?saved->second:Value{};
                if(!binding.empty()) vars_[binding]=error.value;
                pos_=catchBody;
                try {
                    Statement();
                } catch(...) {
                    if(!binding.empty()) {
                        if(hadPrevious) vars_[binding]=previous;
                        else vars_.erase(binding);
                    }
                    throw;
                }
                if(!binding.empty()) {
                    if(hadPrevious) vars_[binding]=previous;
                    else vars_.erase(binding);
                }
            }
            pos_=afterCatch;
            if(Match("finally")) Statement();
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
        } catch(const std::exception&) {
            return false;
        } catch(...) {
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
