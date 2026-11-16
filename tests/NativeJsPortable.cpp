#include "html/HtmlParser.h"
#include "script/NativeEngine.h"
#include "script/JsLexer.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
const ac::html::Node* Find(const ac::html::Node* parent,std::string_view tag) {
    if(!parent) return nullptr;
    if(parent->tag==tag) return parent;
    for(const auto& child:parent->children)
        if(auto* match=Find(child.get(),tag)) return match;
    return nullptr;
}
void GatherText(const ac::html::Node& node,std::string& output) {
    if(node.type==ac::html::Node::Type::Text) output+=node.text;
    for(const auto& child:node.children) GatherText(*child,output);
}
void Check(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
void Test(const char* name,std::string_view source,std::string_view expected) {
    auto doc=ac::html::Parse("<html><body><p id='result'></p><script>"+
                            std::string(source)+"</script></body></html>");
    auto execution=ac::script::ExecuteNative(doc,L"https://example.org/",{});
    if(execution.unsupported!=0) {
        std::cerr<<name<<": "<<execution.lastError<<"\n";
        throw std::runtime_error("JavaScript execution failed");
    }
    const auto* p=Find(doc.root.get(),"p");
    Check(p && !p->children.empty(),"No result element");
    std::string rendered;
    GatherText(*p,rendered);
    if(rendered!=expected) {
        std::cerr<<name<<": expected ["<<expected<<"] actual ["<<rendered<<"]\n";
        throw std::runtime_error("Unexpected DOM output");
    }
}
}
int main() {
    try {
        Test("math","let x=7; function double(v){ return v*2; }"
             "document.getElementById('result').textContent=String(double(x));","14");
        Test("arrows","const values=[1,2,3,4];"
             "const filtered=values.map(x=>x*2).filter(x=>x>4);"
             "document.getElementById('result').textContent=filtered.join(',');","6,8");
        Test("collections",
             "const elements=document.querySelectorAll('p');"
             "const numbers=[4,5]; numbers.push(6);"
             "elements[0].textContent=numbers.join('-');","4-5-6");
        Test("objects","let obj={a:7}; obj.a=obj.a*6;"
             "document.getElementById('result').textContent=String(obj.a);","42");
        Test("finally","function f(){try{return 42;} finally{document.title='done';}}"
             "document.getElementById('result').textContent=String(f());","42");
        Test("json","const value=JSON.parse('{\"count\":42}');"
             "document.getElementById('result').textContent="
             "String(value.count)+'/'+JSON.stringify({ok:true});",
             "42/{\"ok\":true}");
        Test("logical",
             "let calls=0; function side(){ calls++; return 999; }"
             "const a=false && side(); const b=true || side();"
             "const c='set' ?? side(); const d=null ?? 42;"
             "document.getElementById('result').textContent="
             "String(a)+'/'+String(b)+'/'+String(c)+'/'+String(d)+'/'+String(calls);",
             "false/true/set/42/0");
        Test("eval-scope",
             "let answer=40;eval('answer += 2');"
             "eval(\"document.getElementById('result').textContent=String(answer);\");",
             "42");
        Test("google-style-bootstrap",
             "window.google=window.google||{};"
             "window.google.c=window.google.c||{cap:0};"
             "(function(){var local=7;google.c.cap=local*6;})();"
             "document.getElementById('result').textContent=String(google.c.cap);","42");
        Test("google-style-iife",
             "window.google=window.google||{};"
             "(function(){var active=false;"
             "(function(){active&&google.tick('load','start');}).call(this);"
             "})();"
             "document.getElementById('result').textContent='OK';","OK");
        Test("function-call-apply",
             "function sum(a,b){return a+b;}"
             "document.getElementById('result').textContent="
             "String(sum.call(null,19,23))+'/'+String(sum.apply(null,[19,23]));",
             "42/42");
        Test("native-constructor",
             "function Counter(start){this.value=start;}"
             "const counter=new Counter(40);"
             "counter.value+=2;"
             "document.getElementById('result').textContent=String(counter.value);",
             "42");
        Test("function-this",
             "function add(n){return this.base+n;}"
             "const receiver={base:40};"
             "document.getElementById('result').textContent="
             "String(add.call(receiver,2));",
             "42");
        Test("object-prototypes",
             "const base={baseValue:40};"
             "const child=Object.create(base);"
             "Object.defineProperty(child,'extra',{get:function(){return 2;}});"
             "const count=child.baseValue+child.extra;"
             "document.getElementById('result').textContent="
             "String(count)+'/'+String(Object.hasOwn(child,'extra'));",
             "42/true");
        Test("object-assign-freeze",
             "const item=Object.assign({answer:40},{added:2});"
             "item.answer+=item.added;Object.freeze(item);item.answer=999;"
             "document.getElementById('result').textContent="
             "String(item.answer)+'/'+String(Object.isFrozen(item));",
             "42/true");
        Test("browser-intrinsics",
             "const values=Array(3);"
             "const letters=String.fromCharCode(65,66);"
             "const valid=Array.isArray(values)&&!Array.isArray(letters);"
             "const dice=Math.random();"
             "document.getElementById('result').textContent="
             "String(valid&&dice>=0&&dice<1)+'/'+letters+'/'+String(values.length);",
             "true/AB/3");
        Test("for-in",
             "const source={answer:42};let result='';"
             "for(const key in source){result=String(source[key]);}"
             "document.getElementById('result').textContent=result;",
             "42");
        Test("do-while",
             "let count=0;let total=0;"
             "do{count++;if(count===2)continue;"
             "if(count===5)break;total+=count;}while(count<8);"
             "document.getElementById('result').textContent=String(total);",
             "8");
        Test("dom-node-detach",
             "const root=document.getElementById('result');"
             "const text=document.createTextNode('working');"
             "root.appendChild(text);"
             "root.removeChild(text);"
             "root.appendChild(text);",
             "working");
        Test("retained-innerhtml-node",
             "const root=document.getElementById('result');"
             "root.innerHTML='<span>kept</span>';"
             "const old=document.querySelector('span');"
             "root.innerHTML='temporary';"
             "root.appendChild(old);"
             "root.removeChild(old);"
             "root.textContent='reused';",
             "reused");
        Test("adjacent-html",
             "const root=document.getElementById('result');"
             "root.insertAdjacentHTML('afterbegin','<em>first</em>');"
             "root.insertAdjacentHTML('beforeend','<strong>last</strong>');"
             "const first=document.querySelector('em');"
             "first.insertAdjacentText('afterend','-middle-');",
             "first-middle-last");
        Test("adjacent-beforebegin",
             "const root=document.getElementById('result');"
             "root.innerHTML='<em>end</em>';"
             "document.querySelector('em').insertAdjacentHTML('beforebegin','<b>start</b>');"
             "root.textContent='start end';",
             "start end");
        Test("innerhtml-serialization",
             "const element=document.getElementById('result');"
             "element.innerHTML='<strong>Markup</strong>';"
             "const markup=element.innerHTML;"
             "element.textContent=markup;",
             "<strong>Markup</strong>");
        Test("dynamic-element-style",
             "const element=document.getElementById('result');"
             "element.style.display='none';"
             "element.style.display='block';"
             "element.style.setProperty('font-size','18px');"
             "element.classList.add('ready');"
             "element.classList.toggle('temporary');"
             "element.classList.remove('temporary');"
             "element.textContent=element.style.display+'/'+"
             "element.style.getPropertyValue('font-size')+'/'+"
             "String(element.classList.contains('ready'))+'/'+"
             "String(element.classList.contains('temporary'));",
             "block/18px/true/false");
        Test("dom-ready-events",
             "document.addEventListener('DOMContentLoaded',()=>{"
             "document.getElementById('result').textContent='ready';});",
             "ready");
        Test("scheduled-script",
             "setTimeout(function(){"
             "document.getElementById('result').textContent='later';},0);",
             "later");
        Test("cancelled-callback",
             "const id=setTimeout(function(){"
             "document.getElementById('result').textContent='wrong';},0);"
             "clearTimeout(id);"
             "document.getElementById('result').textContent='correct';",
             "correct");
        Test("regexp-constructor",
             "const expression=new RegExp('[0-9]+');"
             "const matched='Answer 42'.match(expression);"
             "const changed='Answer 42'.replace(expression,'ok');"
             "document.getElementById('result').textContent="
             "matched[0]+'/'+changed+'/'+String('a42'.search(expression));",
             "42/Answer ok/1");
        Test("regexp-literals",
             "const pattern=/[{}]+/g;"
             "const check=pattern.test('{{}') && !pattern.test('plain');"
             "document.getElementById('result').textContent=String(check);",
             "true");
        Test("switch-fallthrough",
             "let total=0;const key=2;"
             "switch(key){case 1: total=100;break;"
             "case 2: total=40;case 3: total+=2;break;"
             "default: total=999;}"
             "document.getElementById('result').textContent=String(total);",
             "42");
        Test("switch-default",
             "let total=40;switch('missing'){case 'other':total=0;break;"
             "default:total+=2;break;}"
             "document.getElementById('result').textContent=String(total);",
             "42");
        Test("for-of",
             "let total=0;for(const value of [7,14,21]) {total+=value;}"
             "document.getElementById('result').textContent=String(total);",
             "42");
        Test("labels-and-comma",
             "let answer=40;"
             "outer:{answer++;break outer;answer=999;}"
             "const indirect=(0,eval);indirect('answer += 1');"
             "document.getElementById('result').textContent=String(answer);",
             "42");
        Test("bitwise",
             "let value=(15 & 3) + (7 ^ 2) + (4 | 1);"
             "value+=1; value<<=1;"
             "const answer=value+(2**3)+~(-1);"
             "document.getElementById('result').textContent=String(answer);",
             "36");
        Test("optional-chaining",
             "let counter=0;function side(){counter++;return 1;}"
             "const unavailable=null;const record={value:42};"
             "const a=unavailable?.missing(side());"
             "const b=record?.value;"
             "document.getElementById('result').textContent="
             "String(a)+'/'+String(b)+'/'+String(counter);",
             "undefined/42/0");
        Test("array-methods",
             "const list=[3,6,9];"
             "const more=list.concat([12,15]);"
             "const middle=more.slice(1,4);"
             "document.getElementById('result').textContent="
             "middle.join('-')+'/'+String(more.includes(12))+'/'+"
             "String(more.indexOf(9))+'/'+String(more.at(-1));",
             "6-9-12/true/2/15");
        Test("string-methods",
             "const parts='  RED,Blue,Green  '.trim().toLowerCase().split(',');"
             "const chosen=parts[1].slice(0,2).toUpperCase();"
             "document.getElementById('result').textContent="
             "chosen+'/'+String(parts[2].endsWith('green'))+'/'+"
             "String(parts[0].startsWith('red'));",
             "BL/true/true");
        Test("lexical-closure",
             "const make=()=>{let count=40;return ()=>{count++;return count;};};"
             "const next=make();"
             "document.getElementById('result').textContent="
             "String(next())+'/'+String(next());",
             "41/42");
        Test("function-expression",
             "const twice=function(v){return v*2;};"
             "document.getElementById('result').textContent=String(twice(21));","42");
        Test("conditional","const a=false?'bad':'ok';"
             "document.getElementById('result').textContent=a;","ok");
        const auto regexTokens=ac::script::js::Tokenize("function f(){return /[{}]/.test('{');}");
        unsigned regexCount=0,openBraces=0,closeBraces=0;
        for(const auto& token:regexTokens) {
            if(token.kind==ac::script::js::TokenKind::RegExp) ++regexCount;
            if(token.kind==ac::script::js::TokenKind::Symbol && token.text=="{") ++openBraces;
            if(token.kind==ac::script::js::TokenKind::Symbol && token.text=="}") ++closeBraces;
        }
        Check(regexCount==1 && openBraces==1 && closeBraces==1,
              "regular expression braces must not tokenize as JavaScript blocks");
        auto tokens=ac::script::js::Tokenize("const text='Hola\\u00f1';");
        bool unicode=false;
        for(const auto& token:tokens)
            if(token.kind==ac::script::js::TokenKind::String &&
               token.text=="Hola\xC3\xB1") unicode=true;
        Check(unicode,"Unicode escape decode failed");
        std::cout<<"Native JavaScript portable smoke checks: OK\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Native JavaScript portable smoke failed: "<<error.what()<<"\n";
        return 1;
    }
}
