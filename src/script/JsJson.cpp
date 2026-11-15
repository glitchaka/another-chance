#include "script/JsJson.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ac::script::js {
namespace {
void Utf8(std::string& out,unsigned cp) {
    if(cp<=0x7f) out+=static_cast<char>(cp);
    else if(cp<=0x7ff) {
        out+=static_cast<char>(0xc0|(cp>>6));
        out+=static_cast<char>(0x80|(cp&63));
    } else if(cp<=0xffff) {
        out+=static_cast<char>(0xe0|(cp>>12));
        out+=static_cast<char>(0x80|((cp>>6)&63));
        out+=static_cast<char>(0x80|(cp&63));
    } else {
        out+=static_cast<char>(0xf0|(cp>>18));
        out+=static_cast<char>(0x80|((cp>>12)&63));
        out+=static_cast<char>(0x80|((cp>>6)&63));
        out+=static_cast<char>(0x80|(cp&63));
    }
}
class Parser {
    std::string_view source_;
    size_t position_=0;
    unsigned depth_=0;
    void Space() {
        while(position_<source_.size() &&
              (source_[position_]==' ' || source_[position_]=='\r' ||
               source_[position_]=='\n' || source_[position_]=='\t'))
            ++position_;
    }
    bool Match(char ch) {
        Space();
        if(position_<source_.size() && source_[position_]==ch) {
            ++position_;return true;
        }
        return false;
    }
    void Require(char ch) {
        if(!Match(ch)) throw std::runtime_error("JSON syntax error");
    }
    unsigned Codepoint() {
        if(source_.size()-position_<4) throw std::runtime_error("Invalid JSON unicode");
        unsigned cp=0;
        for(unsigned i=0;i<4;++i) {
            const char digit=source_[position_++];
            cp<<=4;
            if(digit>='0'&&digit<='9') cp+=digit-'0';
            else if(digit>='a'&&digit<='f') cp+=digit-'a'+10;
            else if(digit>='A'&&digit<='F') cp+=digit-'A'+10;
            else throw std::runtime_error("Invalid JSON unicode");
        }
        return cp;
    }
    std::string String() {
        Require('"');
        std::string out;
        while(position_<source_.size()) {
            unsigned char c=static_cast<unsigned char>(source_[position_++]);
            if(c=='"') return out;
            if(c<0x20) throw std::runtime_error("Control character in JSON string");
            if(c!='\\') {
                out+=static_cast<char>(c);
                continue;
            }
            if(position_>=source_.size()) throw std::runtime_error("Invalid JSON escape");
            c=static_cast<unsigned char>(source_[position_++]);
            switch(c) {
                case '"':out+='"';break;
                case '\\':out+='\\';break;
                case '/':out+='/';break;
                case 'n':out+='\n';break;
                case 'r':out+='\r';break;
                case 't':out+='\t';break;
                case 'b':out+='\b';break;
                case 'f':out+='\f';break;
                case 'u': {
                    unsigned cp=Codepoint();
                    if(cp>=0xd800 && cp<=0xdbff) {
                        if(position_+2>source_.size() ||
                           source_[position_]!='\\' || source_[position_+1]!='u')
                            throw std::runtime_error("Missing JSON surrogate pair");
                        position_+=2;
                        const unsigned lower=Codepoint();
                        if(lower<0xdc00 || lower>0xdfff)
                            throw std::runtime_error("Invalid JSON surrogate pair");
                        cp=0x10000+((cp-0xd800)<<10)+(lower-0xdc00);
                    } else if(cp>=0xdc00 && cp<=0xdfff) {
                        throw std::runtime_error("Unexpected low JSON surrogate");
                    }
                    Utf8(out,cp);
                    break;
                }
                default:throw std::runtime_error("Invalid JSON escape");
            }
            if(out.size()>4*1024*1024) throw std::runtime_error("JSON string limit");
        }
        throw std::runtime_error("Unterminated JSON string");
    }
    Value ParseValue() {
        Space();
        if(position_>=source_.size()) throw std::runtime_error("Incomplete JSON");
        const char c=source_[position_];
        if(c=='"') return Value::Str(String());
        if(c=='[') {
            if(++depth_>128) throw std::runtime_error("JSON nesting limit");
            ++position_;
            std::vector<Value> elements;
            if(!Match(']')) {
                do {
                    elements.push_back(ParseValue());
                    if(elements.size()>100000) throw std::runtime_error("JSON array limit");
                    if(Match(']')) break;
                    Require(',');
                } while(true);
            }
            --depth_;
            return Value::ArrayValue(std::move(elements));
        }
        if(c=='{') {
            if(++depth_>128) throw std::runtime_error("JSON nesting limit");
            ++position_;
            std::unordered_map<std::string,Value> properties;
            if(!Match('}')) {
                do {
                    Space();
                    if(position_>=source_.size() || source_[position_]!='"')
                        throw std::runtime_error("JSON object key must be a string");
                    auto key=String();
                    Require(':');
                    properties.insert_or_assign(std::move(key),ParseValue());
                    if(properties.size()>100000) throw std::runtime_error("JSON object limit");
                    if(Match('}')) break;
                    Require(',');
                } while(true);
            }
            --depth_;
            return Value::ObjectValue(std::move(properties));
        }
        for(const auto keyword:{"true","false","null"}) {
            const std::string_view word(keyword);
            if(source_.substr(position_,word.size())==word) {
                position_+=word.size();
                if(word=="true") return Value::Bool(true);
                if(word=="false") return Value::Bool(false);
                return Value::Special(Value::Kind::Null);
            }
        }
        const size_t begin=position_;
        if(source_[position_]=='-') ++position_;
        if(position_>=source_.size()) throw std::runtime_error("Invalid JSON number");
        if(source_[position_]=='0') ++position_;
        else if(source_[position_]>='1' && source_[position_]<='9') {
            while(position_<source_.size() &&
                  std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
        } else throw std::runtime_error("Invalid JSON number");
        if(position_<source_.size() && source_[position_]=='.') {
            ++position_;
            const size_t digits=position_;
            while(position_<source_.size() &&
                  std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
            if(digits==position_) throw std::runtime_error("Invalid JSON fraction");
        }
        if(position_<source_.size() &&
           (source_[position_]=='e'||source_[position_]=='E')) {
            ++position_;
            if(position_<source_.size() &&
               (source_[position_]=='+'||source_[position_]=='-')) ++position_;
            const size_t digits=position_;
            while(position_<source_.size() &&
                  std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
            if(digits==position_) throw std::runtime_error("Invalid JSON exponent");
        }
        const std::string number(source_.substr(begin,position_-begin));
        return Value::Num(std::strtod(number.c_str(),nullptr));
    }
public:
    explicit Parser(std::string_view input):source_(input) {
        if(source_.size()>4*1024*1024) throw std::runtime_error("JSON input limit");
    }
    Value Parse() {
        auto value=ParseValue();
        Space();
        if(position_!=source_.size()) throw std::runtime_error("Trailing JSON data");
        return value;
    }
};
void Escape(const std::string& value,std::string& out) {
    constexpr char hex[]="0123456789abcdef";
    out+='"';
    for(unsigned char c:value) {
        switch(c) {
            case '"':out+="\\\"";break;
            case '\\':out+="\\\\";break;
            case '\n':out+="\\n";break;
            case '\r':out+="\\r";break;
            case '\t':out+="\\t";break;
            default:
                if(c<32) {
                    out+="\\u00";
                    out+=hex[(c>>4)&15];
                    out+=hex[c&15];
                } else out+=static_cast<char>(c);
        }
    }
    out+='"';
}
void Serialize(const Value& value,std::string& out,
               std::unordered_set<const void*>& seen,unsigned depth) {
    if(depth>128 || out.size()>4*1024*1024)
        throw std::runtime_error("JSON serialization limit");
    switch(value.kind) {
        case Value::Kind::Undefined:
        case Value::Kind::Null: out+="null";break;
        case Value::Kind::Boolean:out+=value.number?"true":"false";break;
        case Value::Kind::Number:
            out+=std::isfinite(value.number)?value.String():"null";break;
        case Value::Kind::Text:Escape(value.text,out);break;
        case Value::Kind::Array: {
            if(!value.array) {out+="[]";break;}
            if(!seen.insert(value.array.get()).second)
                throw std::runtime_error("Circular JSON array");
            out+='[';
            for(size_t i=0;i<value.array->size();++i) {
                if(i) out+=',';
                Serialize((*value.array)[i],out,seen,depth+1);
            }
            out+=']';
            seen.erase(value.array.get());
            break;
        }
        case Value::Kind::Object: {
            if(!value.object) {out+="{}";break;}
            if(!seen.insert(value.object.get()).second)
                throw std::runtime_error("Circular JSON object");
            out+='{';
            bool first=true;
            for(const auto& [key,member]:*value.object) {
                if(member.kind==Value::Kind::Undefined ||
                   member.kind==Value::Kind::Function) continue;
                if(!first) out+=',';
                first=false;
                Escape(key,out);
                out+=':';
                Serialize(member,out,seen,depth+1);
            }
            out+='}';
            seen.erase(value.object.get());
            break;
        }
        default:out+="null";break;
    }
}
} // namespace
Value ParseJson(std::string_view input) {return Parser(input).Parse();}
std::string StringifyJson(const Value& value) {
    if(value.kind==Value::Kind::Undefined ||
       value.kind==Value::Kind::Function) return {};
    std::string result;
    std::unordered_set<const void*> seen;
    Serialize(value,result,seen,0);
    return result;
}
} // namespace ac::script::js
