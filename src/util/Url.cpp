#include "util/Url.h"
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace ac::url {

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (!length) {
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

namespace {
std::wstring Trim(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}
bool Prefix(std::wstring_view text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(text[i]) != std::towlower(prefix[i])) return false;
    }
    return true;
}
bool InvalidInput(std::wstring_view value) {
    return value.empty() || value.size() > 16384 ||
           std::any_of(value.begin(), value.end(), [](wchar_t c) {
               return c < 32 || c == 127 || std::iswspace(c) || c == L'\\';
           });
}
bool Digits(std::wstring_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
        [](wchar_t c){ return c >= L'0' && c <= L'9'; });
}
bool ValidPort(std::wstring_view text) {
    if (!Digits(text) || text.size() > 5) return false;
    unsigned value = 0;
    for (wchar_t c : text) value = value * 10 + static_cast<unsigned>(c - L'0');
    return value > 0 && value <= 65535;
}
bool HostName(std::wstring_view host) {
    if (host.empty()) return false;
    if (host == L"[::1]") return true;
    if (host.front() == L'[') {
        return host.back() == L']' && host.size() > 3;
    }
    if (host.front() == L'.' || host.back() == L'.') return false;
    size_t begin = 0;
    while (begin < host.size()) {
        const size_t end = host.find(L'.', begin);
        const auto label = host.substr(begin,
            end == std::wstring_view::npos ? host.size() - begin : end - begin);
        if (label.empty() || label.size() > 63 ||
            label.front() == L'-' || label.back() == L'-') return false;
        for (wchar_t c : label) {
            const bool allowed = (c >= L'a' && c <= L'z') ||
                                 (c >= L'A' && c <= L'Z') ||
                                 (c >= L'0' && c <= L'9') || c == L'-';
            if (!allowed) return false;
        }
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return true;
}
bool LocalAddress(std::wstring_view host) {
    std::wstring lower(host);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lower == L"localhost" || lower == L"[::1]" || lower == L"127.0.0.1")
        return true;
    return lower.starts_with(L"10.") ||
           lower.starts_with(L"192.168.") ||
           lower.starts_with(L"169.254.") ||
           lower.starts_with(L"127.") ||
           lower.starts_with(L"172.16.") || lower.starts_with(L"172.17.") ||
           lower.starts_with(L"172.18.") || lower.starts_with(L"172.19.") ||
           lower.starts_with(L"172.20.") || lower.starts_with(L"172.21.") ||
           lower.starts_with(L"172.22.") || lower.starts_with(L"172.23.") ||
           lower.starts_with(L"172.24.") || lower.starts_with(L"172.25.") ||
           lower.starts_with(L"172.26.") || lower.starts_with(L"172.27.") ||
           lower.starts_with(L"172.28.") || lower.starts_with(L"172.29.") ||
           lower.starts_with(L"172.30.") || lower.starts_with(L"172.31.");
}
bool ParsedWebUrl(std::wstring_view input) {
    const std::wstring value(input);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(value.c_str(), 0, 0, &parts)) return false;
    return (parts.nScheme == INTERNET_SCHEME_HTTP ||
            parts.nScheme == INTERNET_SCHEME_HTTPS) &&
           parts.dwHostNameLength > 0;
}
std::wstring AuthorityHost(std::wstring_view input, bool& valid) {
    valid = false;
    const auto end = input.find_first_of(L"/?#");
    std::wstring_view authority = input.substr(0, end);
    if (authority.empty() || authority.find(L'@') != std::wstring_view::npos)
        return {};
    std::wstring_view host = authority;
    if (authority.front() == L'[') {
        const auto close = authority.find(L']');
        if (close == std::wstring_view::npos) return {};
        host = authority.substr(0, close + 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty() &&
           (remainder.front() != L':' || !ValidPort(remainder.substr(1))))
            return {};
    } else {
        const auto colon = authority.find(L':');
        if (colon != std::wstring_view::npos) {
            if (authority.find(L':', colon + 1) != std::wstring_view::npos ||
                !ValidPort(authority.substr(colon + 1))) return {};
            host = authority.substr(0, colon);
        }
    }
    if (!HostName(host)) return {};
    valid = true;
    return std::wstring(host);
}
std::wstring PercentEncode(std::wstring_view query) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring output;
    for (unsigned char ch : ToUtf8(query)) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
            ch == '.' || ch == '~') {
            output.push_back(static_cast<wchar_t>(ch));
        } else if (ch == ' ') {
            output += L'+';
        } else {
            output.push_back(L'%');
            output.push_back(digits[ch >> 4]);
            output.push_back(digits[ch & 15]);
        }
    }
    return output;
}
} // namespace

bool IsWeb(std::wstring_view value) {
    return Prefix(value, L"http://") || Prefix(value, L"https://");
}

std::wstring Normalize(std::wstring input) {
    input = Trim(std::move(input));
    if (InvalidInput(input)) return {};

    if (IsWeb(input)) {
        const size_t separator = input.find(L"://");
        if (separator == std::wstring::npos) return {};
        bool valid = false;
        AuthorityHost(std::wstring_view(input).substr(separator + 3), valid);
        if (!valid) return {};
        const bool secure = Prefix(input, L"https://");
        input.replace(0, separator + 3, secure ? L"https://" : L"http://");
        return ParsedWebUrl(input) ? input : std::wstring{};
    }
    if (input.starts_with(L"//")) {
        input = L"https:" + input;
        return Normalize(std::move(input));
    }
    if (input.find(L"://") != std::wstring::npos) return {};
    bool valid = false;
    const std::wstring host = AuthorityHost(input, valid);
    if (!valid) return {};
    // Bare single-label names are ambiguous. Explicit http://intranet
    // remains valid, but "intranet" is a search term.
    if (host != L"localhost" && host != L"[::1]" &&
        host.find(L'.') == std::wstring::npos) return {};

    const std::wstring output =
        std::wstring(LocalAddress(host) ? L"http://" : L"https://") + input;
    return ParsedWebUrl(output) ? output : std::wstring{};
}

std::wstring Search(std::wstring_view terms, SearchProvider provider) {
    if (provider == SearchProvider::Disabled || terms.empty() ||
        terms.size() > 4096 || std::any_of(terms.begin(), terms.end(),
            [](wchar_t c){return c < 32 || c == 127;})) return {};
    const auto query = PercentEncode(terms);
    switch (provider) {
        case SearchProvider::Google:
            return L"https://www.google.com/search?q=" + query;
        case SearchProvider::Bing:
            return L"https://www.bing.com/search?q=" + query;
        case SearchProvider::Disabled:
            return {};
    }
    return {};
}

std::wstring ResolveInput(std::wstring input, SearchProvider provider) {
    input = Trim(std::move(input));
    if (auto result = Normalize(input); !result.empty()) return result;
    if (input.empty() || input.find(L"://") != std::wstring::npos ||
        input.starts_with(L"//") ||
        input.find(L'@') != std::wstring::npos ||
        input.find(L'\\') != std::wstring::npos ||
        input.find(L':') != std::wstring::npos) return {};
    return Search(input, provider);
}

std::wstring Resolve(std::wstring_view base, std::wstring_view reference) {
    if (reference.empty()) return {};
    std::wstring ref(reference);
    if (IsWeb(ref)) return Normalize(ref);
    if (ref.front() == L'#') {
        // Same-document anchors require element-ID scrolling, not yet supported.
        return {};
    }
    if (ref.find(L':') != std::wstring::npos &&
        !ref.starts_with(L"//")) return {};
    std::wstring output(16384, L'\0');
    DWORD size = static_cast<DWORD>(output.size());
    const std::wstring parent(base);
    if (SUCCEEDED(UrlCombineW(parent.c_str(), ref.c_str(),
                              output.data(), &size, 0))) {
        output.resize(size);
        return Normalize(output);
    }
    return {};
}
} // namespace ac::url
