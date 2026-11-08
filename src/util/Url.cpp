#include "util/Url.h"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace ac::url {

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (!length) {
        // Older pages sometimes incorrectly send Windows-1252 as UTF-8.
        codepage = 1252;
        length = MultiByteToWideChar(codepage, 0, utf8.data(),
            static_cast<int>(utf8.size()), nullptr, 0);
    }
    if (length <= 0) return {};
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codepage, 0, utf8.data(),
        static_cast<int>(utf8.size()), output.data(), length);
    return output;
}

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        output.data(), length, nullptr, nullptr);
    return output;
}

static std::wstring Trim(std::wstring str) {
    auto whitespace = [](wchar_t c) { return std::iswspace(c) != 0; };
    while (!str.empty() && whitespace(str.front())) str.erase(str.begin());
    while (!str.empty() && whitespace(str.back())) str.pop_back();
    return str;
}

static std::wstring PercentEncode(std::wstring_view query) {
    std::wstring encoded;
    for (unsigned char byte : ToUtf8(query)) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_'
            || byte == '.' || byte == '~') {
            encoded.push_back(static_cast<wchar_t>(byte));
        } else if (byte == ' ') {
            encoded += L"+";
        } else {
            constexpr wchar_t digits[] = L"0123456789ABCDEF";
            encoded += L'%';
            encoded += digits[byte >> 4];
            encoded += digits[byte & 15];
        }
    }
    return encoded;
}

bool IsWeb(std::wstring_view input) {
    return input.starts_with(L"https://") || input.starts_with(L"http://");
}

std::wstring Normalize(std::wstring input) {
    input = Trim(std::move(input));
    if (input.empty()) return {};
    std::transform(input.begin(), input.end(), input.begin(),
        [](wchar_t c) { return c == L'\r' || c == L'\n' ? L' ' : c; });
    if (IsWeb(input)) return input;
    if (input.find(L"://") != std::wstring::npos ||
        input.starts_with(L"javascript:") || input.starts_with(L"file:") ||
        input.starts_with(L"data:")) {
        return {}; // Explicitly disallow unsupported or active schemes.
    }
    if (input.find_first_of(L" \t") != std::wstring::npos ||
        input.find(L'.') == std::wstring::npos) {
        return L"https://lite.duckduckgo.com/lite/?q=" + PercentEncode(input);
    }
    return L"https://" + input;
}

std::wstring Resolve(std::wstring_view base, std::wstring_view reference) {
    if (reference.empty()) return {};
    std::wstring ref(reference);
    if (IsWeb(ref)) return ref;
    if (ref.starts_with(L"javascript:") || ref.starts_with(L"data:") ||
        ref.starts_with(L"mailto:") || ref.starts_with(L"file:")) return {};
    std::wstring parent(base);
    DWORD length = 16384;
    std::wstring result(length, L'\0');
    if (SUCCEEDED(UrlCombineW(parent.c_str(), ref.c_str(),
            result.data(), &length, 0))) {
        result.resize(length);
        if (IsWeb(result)) return result;
    }
    return {};
}
} // namespace ac::url
