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
    Check(p->children.front()->text==expected,"Unexpected DOM output");
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
        Test("conditional","const a=false?'bad':'ok';"
             "document.getElementById('result').textContent=a;","ok");
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
