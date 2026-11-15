// Test-only implementations of URL functions: never linked into the Windows browser.
// Keeps the independent lexer/parser/interpreter testable on non-Windows CI.
#include "util/Url.h"
#include <algorithm>

namespace ac::url {
std::wstring ToWide(std::string_view input) {
    return std::wstring(input.begin(),input.end()); // ASCII fixtures only
}
std::string ToUtf8(std::wstring_view input) {
    return std::string(input.begin(),input.end()); // ASCII fixtures only
}
bool IsWeb(std::wstring_view url) {
    return url.starts_with(L"https://") || url.starts_with(L"http://");
}
std::wstring Resolve(std::wstring_view base,std::wstring_view reference) {
    if(IsWeb(reference)) return std::wstring(reference);
    if(!IsWeb(base)) return {};
    const size_t scheme=base.find(L"://");
    const size_t hostEnd=base.find(L'/',scheme+3);
    const std::wstring origin(base.substr(0,hostEnd));
    if(reference.starts_with(L"//"))
        return std::wstring(base.substr(0,scheme+1))+std::wstring(reference);
    if(reference.starts_with(L"/"))
        return origin+std::wstring(reference);
    const size_t pathEnd=base.find_first_of(L"?#");
    const std::wstring_view path=base.substr(0,pathEnd);
    const size_t lastSlash=path.find_last_of(L'/');
    if(lastSlash!=std::wstring_view::npos && lastSlash>scheme+2)
        return std::wstring(path.substr(0,lastSlash+1))+std::wstring(reference);
    return origin+L"/"+std::wstring(reference);
}
}
