#include "script/JsLexer.h"
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace ac::script::js {
namespace {
bool Start(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 128; }
bool Continue(unsigned char c) { return Start(c) || std::isdigit(c); }
void Utf8(std::string& output, unsigned cp) {
    if (cp < 128) output += static_cast<char>(cp);
    else if (cp < 2048) {
        output += static_cast<char>(0xc0 | (cp >> 6));
        output += static_cast<char>(0x80 | (cp & 63));
    } else {
        output += static_cast<char>(0xe0 | (cp >> 12));
        output += static_cast<char>(0x80 | ((cp >> 6) & 63));
        output += static_cast<char>(0x80 | (cp & 63));
    }
}
}
std::vector<Token> Tokenize(std::string_view input) {
    if (input.size() > 4 * 1024 * 1024) throw std::runtime_error("JavaScript source too large");
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < input.size()) {
        if (tokens.size() >= 500000) throw std::runtime_error("JavaScript token limit");
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (input.substr(i,2) == "//") {
            i += 2;
            while (i < input.size() && input[i] != '\n') ++i;
            continue;
        }
        if (input.substr(i,2) == "/*") {
            const auto end = input.find("*/", i + 2);
            if (end == std::string_view::npos) throw std::runtime_error("Unterminated comment");
            i = end + 2;
            continue;
        }
        // A slash starts a regular-expression literal only where an expression
        // may begin. Do not confuse division with regex delimiters, and do not
        // tokenize braces contained in regex character classes as JS blocks.
        if (c == '/') {
            bool regexpAllowed=tokens.empty();
            if(!tokens.empty()) {
                const auto& previous=tokens.back();
                if(previous.kind==TokenKind::Symbol) {
                    regexpAllowed=previous.text!=")" && previous.text!="]" &&
                                  previous.text!="}" && previous.text!="++" &&
                                  previous.text!="--";
                } else if(previous.kind==TokenKind::Identifier) {
                    regexpAllowed=previous.text=="return" || previous.text=="throw" ||
                                  previous.text=="case" || previous.text=="delete" ||
                                  previous.text=="void" || previous.text=="typeof" ||
                                  previous.text=="instanceof" || previous.text=="in" ||
                                  previous.text=="of" || previous.text=="yield" ||
                                  previous.text=="await" || previous.text=="new" ||
                                  previous.text=="else" || previous.text=="do";
                }
            }
            if(regexpAllowed) {
                const size_t begin=i++;
                bool charClass=false,escaped=false,terminated=false;
                while(i<input.size() && input[i]!='\n' && input[i]!='\r') {
                    const char ch=input[i++];
                    if(escaped) {escaped=false;continue;}
                    if(ch=='\\') {escaped=true;continue;}
                    if(ch=='[') {charClass=true;continue;}
                    if(ch==']') {charClass=false;continue;}
                    if(ch=='/' && !charClass) {terminated=true;break;}
                }
                if(terminated) {
                    while(i<input.size() &&
                          std::isalpha(static_cast<unsigned char>(input[i]))) ++i;
                    tokens.push_back({TokenKind::RegExp,
                        std::string(input.substr(begin,i-begin)),0,begin});
                    continue;
                }
                i=begin;
            }
        }
        const size_t start = i;
        if (Start(c)) {
            ++i;
            while (i < input.size() && Continue(static_cast<unsigned char>(input[i]))) ++i;
            tokens.push_back({TokenKind::Identifier,std::string(input.substr(start,i-start)),0,start});
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < input.size() &&
                                  std::isdigit(static_cast<unsigned char>(input[i+1])))) {
            ++i;
            while (i < input.size() && (std::isdigit(static_cast<unsigned char>(input[i])) ||
                   input[i] == '.')) ++i;
            if (i < input.size() && (input[i] == 'e' || input[i] == 'E')) {
                size_t e = i + 1;
                if (e < input.size() && (input[e] == '+' || input[e] == '-')) ++e;
                size_t d = e;
                while (e < input.size() && std::isdigit(static_cast<unsigned char>(input[e]))) ++e;
                if (e > d) i = e;
            }
            const std::string number(input.substr(start,i-start));
            tokens.push_back({TokenKind::Number,number,std::strtod(number.c_str(),nullptr),start});
            continue;
        }
        if (c == '"' || c == '\'' || c == 96) {
            const char quote = input[i++];
            std::string result;
            bool closed = false;
            while (i < input.size()) {
                char ch = input[i++];
                if (ch == quote) { closed=true; break; }
                if (ch == '\\' && i < input.size()) {
                    ch = input[i++];
                    switch(ch) {
                        case 'n': result += '\n'; break;
                        case 'r': result += '\r'; break;
                        case 't': result += '\t'; break;
                        case 'b': result += '\b'; break;
                        case 'f': result += '\f'; break;
                        case 'v': result += '\v'; break;
                        case '0': result += '\0'; break;
                        case '\n': break;
                        case 'u': {
                            if (input.size() - i < 4) throw std::runtime_error("Invalid unicode escape");
                            unsigned cp = 0;
                            for (int k=0; k<4; ++k) {
                                const char digit = input[i++];
                                cp <<= 4;
                                if (digit >= '0' && digit <= '9') cp += digit - '0';
                                else if (digit >= 'a' && digit <= 'f') cp += digit - 'a' + 10;
                                else if (digit >= 'A' && digit <= 'F') cp += digit - 'A' + 10;
                                else throw std::runtime_error("Invalid unicode escape");
                            }
                            Utf8(result,cp);
                            break;
                        }
                        default: result += ch; break;
                    }
                } else result += ch;
                if (result.size() > 1024 * 1024) throw std::runtime_error("String limit");
            }
            if (!closed) throw std::runtime_error("Unterminated JavaScript string");
            tokens.push_back({TokenKind::String,std::move(result),0,start});
            continue;
        }
        bool matched=false;
        for (auto op : {">>>=","===","!==",">>>","<<=",">>=","**=","=>","==","!=","<=",">=","&&","||","??",
                        "++","--","+=","-=","*=","/=","%=","&=","|=","^=","**","?.","<<",">>","..."} ) {
            const std::string_view value(op);
            if (input.substr(i,value.size())==value) {
                tokens.push_back({TokenKind::Symbol,std::string(value),0,i});
                i += value.size(); matched=true; break;
            }
        }
        if (matched) continue;
        tokens.push_back({TokenKind::Symbol,std::string(1,input[i]),0,i});
        ++i;
    }
    tokens.push_back({TokenKind::End,"",0,input.size()});
    return tokens;
}
}