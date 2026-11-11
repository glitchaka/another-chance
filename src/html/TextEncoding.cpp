#include "html/TextEncoding.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <string_view>
#include <vector>

namespace ac::html {
namespace {
std::string Lower(std::string_view v) {
    std::string text(v);
    for(char& c:text) c=static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string LowerWide(std::wstring_view v) {
    std::string text;
    text.reserve(v.size());
    for (wchar_t c : v)
        text.push_back(c<=127 ? static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))) : '?');
    return text;
}
std::string Charset(std::string_view input) {
    const auto lowercase=Lower(input);
    auto where=lowercase.find("charset");
    if(where==std::string::npos) return {};
    where+=7;
    while(where<lowercase.size() &&
          std::isspace(static_cast<unsigned char>(lowercase[where]))) ++where;
    if(where>=lowercase.size() || lowercase[where]!='=') return {};
    ++where;
    while(where<lowercase.size() &&
          std::isspace(static_cast<unsigned char>(lowercase[where]))) ++where;
    if(where>=lowercase.size()) return {};
    char quote=0;
    if(lowercase[where]=='"' || lowercase[where]=='\'') quote=lowercase[where++];
    const auto begin=where;
    while(where<lowercase.size()) {
        const char c=lowercase[where];
        if(quote ? c==quote : c==';'||c=='>'||
           std::isspace(static_cast<unsigned char>(c))) break;
        ++where;
    }
    return lowercase.substr(begin,where-begin);
}
UINT CodePage(std::string_view label) {
    const std::string id=Lower(label);
    if(id=="utf-8" || id=="utf8") return CP_UTF8;
    if(id=="windows-1252" || id=="cp1252" || id=="iso-8859-1" ||
       id=="latin1" || id=="latin-1" || id=="us-ascii") return 1252;
    if(id=="windows-1251" || id=="cp1251") return 1251;
    if(id=="iso-8859-2") return 28592;
    if(id=="shift_jis" || id=="shift-jis" || id=="sjis" ||
       id=="windows-31j") return 932;
    if(id=="gbk" || id=="gb2312") return 936;
    if(id=="big5") return 950;
    if(id=="euc-kr") return 949;
    if(id=="koi8-r") return 20866;
    return 0;
}
std::string EncodeUtf8(const std::wstring& wide) {
    if(wide.empty() || wide.size()>INT_MAX) return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,wide.data(),
        static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
    if(size<=0) return {};
    std::string output(static_cast<size_t>(size),'\0');
    if(!WideCharToMultiByte(CP_UTF8,0,wide.data(),
        static_cast<int>(wide.size()),output.data(),size,nullptr,nullptr)) return {};
    return output;
}
std::string Convert(std::string_view body, UINT codePage) {
    if(body.empty()) return {};
    if(body.size()>INT_MAX || !IsValidCodePage(codePage)) return {};
    const DWORD flags=codePage==CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int count=MultiByteToWideChar(codePage,flags,body.data(),
        static_cast<int>(body.size()),nullptr,0);
    if(count<=0) return {};
    std::wstring wide(static_cast<size_t>(count),L'\0');
    if(!MultiByteToWideChar(codePage,flags,body.data(),
        static_cast<int>(body.size()),wide.data(),count)) return {};
    return EncodeUtf8(wide);
}
std::string Utf16(std::string_view body,bool bigEndian) {
    if(body.size()%2!=0 || body.size()>INT_MAX) return {};
    std::wstring wide;
    wide.reserve(body.size()/2);
    for(size_t i=0;i+1<body.size();i+=2) {
        const unsigned char a=static_cast<unsigned char>(body[i]);
        const unsigned char b=static_cast<unsigned char>(body[i+1]);
        const wchar_t code=static_cast<wchar_t>(bigEndian ?
            (static_cast<unsigned>(a)<<8)|b :
            (static_cast<unsigned>(b)<<8)|a);
        wide.push_back(code);
    }
    return EncodeUtf8(wide);
}
} // namespace

std::string DecodeToUtf8(std::string_view bytes,
                         std::wstring_view contentType) {
    if(bytes.size()>=3 && bytes.substr(0,3)=="\xEF\xBB\xBF")
        return std::string(bytes.substr(3));
    if(bytes.size()>=2) {
        if(static_cast<unsigned char>(bytes[0])==0xFF &&
           static_cast<unsigned char>(bytes[1])==0xFE)
            return Utf16(bytes.substr(2),false);
        if(static_cast<unsigned char>(bytes[0])==0xFE &&
           static_cast<unsigned char>(bytes[1])==0xFF)
            return Utf16(bytes.substr(2),true);
    }
    std::string label=Charset(LowerWide(contentType));
    if(label.empty()) {
        // Meta charset is typically within the first 1024 document bytes.
        // Ignore script payloads or bodies beyond the encoding sniff window.
        const std::string prefix=Lower(bytes.substr(0,std::min<size_t>(1024,bytes.size())));
        size_t pos=0;
        while((pos=prefix.find("<meta",pos))!=std::string::npos) {
            const auto end=prefix.find('>',pos);
            if(end==std::string::npos) break;
            label=Charset(std::string_view(prefix).substr(pos,end-pos));
            if(!label.empty()) break;
            pos=end+1;
        }
    }
    if(label=="utf-16" || label=="utf-16le") return Utf16(bytes,false);
    if(label=="utf-16be") return Utf16(bytes,true);
    const UINT page=CodePage(label);
    if(page==CP_UTF8) {
        if(!Convert(bytes,CP_UTF8).empty()) return std::string(bytes);
        return Convert(bytes,1252);
    }
    if(page) {
        const auto converted=Convert(bytes,page);
        if(!converted.empty()) return converted;
    }
    // Without an explicit encoding, preserve well-formed UTF-8. If byte
    // sequences are invalid, fall back to Windows-1252 per legacy HTML.
    if(!Convert(bytes,CP_UTF8).empty()) return std::string(bytes);
    const auto fallback=Convert(bytes,1252);
    return fallback.empty()?std::string(bytes):fallback;
}
} // namespace ac::html
