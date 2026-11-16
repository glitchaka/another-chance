#include "script/DomProperties.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace ac::script::dom {
namespace {
std::string Trim(std::string_view value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string_view::npos) return {};
    const auto last=value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first,last-first+1));
}
std::vector<std::string> ClassTokens(const html::Node& node) {
    std::vector<std::string> classes;
    std::istringstream input(node.Attribute("class"));
    std::string token;
    while(input>>token) classes.push_back(token);
    return classes;
}
void WriteClasses(html::Node& node,const std::vector<std::string>& classes) {
    std::string value;
    for(const auto& item:classes) {
        if(!value.empty()) value+=' ';
        value+=item;
    }
    node.attributes["class"]=std::move(value);
}
}
namespace {
void EscapeMarkup(std::string_view value,std::string& output,bool attribute) {
    for(char c:value) {
        if(c=='&') output+="&amp;";
        else if(c=='<') output+="&lt;";
        else if(c=='>') output+="&gt;";
        else if(attribute && c=='"') output+="&quot;";
        else output+=c;
    }
}
void SerializeNode(const html::Node& node,std::string& output,unsigned depth) {
    if(depth>128 || output.size()>4*1024*1024)
        return;
    if(node.type==html::Node::Type::Text) {
        EscapeMarkup(node.text,output,false);
        return;
    }
    if(node.tag=="document") {
        for(const auto& child:node.children) SerializeNode(*child,output,depth+1);
        return;
    }
    output+='<';
    output+=node.tag;
    for(const auto& [key,value]:node.attributes) {
        output+=' ';
        output+=key;
        output+="=\"";
        EscapeMarkup(value,output,true);
        output+='"';
    }
    output+='>';
    static constexpr std::string_view voidTags[]={
        "area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr"
    };
    for(const auto tag:voidTags) if(tag==node.tag) return;
    for(const auto& child:node.children) SerializeNode(*child,output,depth+1);
    output+="</";
    output+=node.tag;
    output+='>';
}
}
std::string SerializeChildren(const html::Node& node) {
    std::string result;
    for(const auto& child:node.children)
        SerializeNode(*child,result,0);
    return result;
}
std::string CssPropertyName(std::string_view name) {
    std::string output;
    if(name.starts_with("--")) return std::string(name);
    for(unsigned char byte:name) {
        if(std::isupper(byte)) {
            output+='-';
            output+=static_cast<char>(std::tolower(byte));
        } else output+=static_cast<char>(byte);
    }
    if(output=="css-float") return "float";
    return output;
}
std::string GetStyle(const html::Node& element,std::string_view name) {
    const auto property=CssPropertyName(name);
    const auto css=element.Attribute("style");
    std::string result;
    size_t offset=0;
    while(offset<css.size()) {
        const auto separator=css.find(';',offset);
        const auto declaration=std::string_view(css).substr(offset,
            separator==std::string::npos?std::string::npos:separator-offset);
        const auto colon=declaration.find(':');
        if(colon!=std::string_view::npos &&
           Trim(declaration.substr(0,colon))==property)
            result=Trim(declaration.substr(colon+1));
        if(separator==std::string::npos) break;
        offset=separator+1;
    }
    return result;
}
void SetStyle(html::Node& element,std::string_view name,std::string_view value) {
    const auto property=CssPropertyName(name);
    if(property.empty() || property.size()>128 || value.size()>65536) return;
    std::string css=element.Attribute("style");
    std::string updated;
    size_t offset=0;
    while(offset<css.size()) {
        const auto separator=css.find(';',offset);
        const auto declaration=std::string_view(css).substr(offset,
            separator==std::string::npos?std::string::npos:separator-offset);
        const auto colon=declaration.find(':');
        if(colon!=std::string_view::npos &&
           Trim(declaration.substr(0,colon))!=property) {
            if(!updated.empty()) updated+=';';
            updated+=Trim(declaration);
        }
        if(separator==std::string::npos) break;
        offset=separator+1;
    }
    if(!value.empty()) {
        if(!updated.empty()) updated+=';';
        updated+=property;
        updated+=':';
        updated+=std::string(value);
    }
    element.attributes["style"]=std::move(updated);
}
bool ContainsClass(const html::Node& element,std::string_view name) {
    const auto classes=ClassTokens(element);
    return std::find(classes.begin(),classes.end(),name)!=classes.end();
}
void SetClass(html::Node& element,std::string_view name,bool enabled) {
    if(name.empty() || name.size()>256) return;
    for(unsigned char c:name)
        if(std::isspace(c)) return;
    auto classes=ClassTokens(element);
    const auto found=std::find(classes.begin(),classes.end(),name);
    if(enabled && found==classes.end()) classes.emplace_back(name);
    if(!enabled && found!=classes.end()) classes.erase(found);
    WriteClasses(element,classes);
}
bool ToggleClass(html::Node& element,std::string_view name) {
    const bool enabled=!ContainsClass(element,name);
    SetClass(element,name,enabled);
    return enabled;
}
} // namespace ac::script::dom
