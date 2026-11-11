#pragma once
#include <string>
#include <string_view>

namespace ac::url {
enum class SearchProvider { Google, Bing, Disabled };

std::wstring ToWide(std::string_view utf8);
std::string ToUtf8(std::wstring_view wide);
bool IsWeb(std::wstring_view input);

// Complete a genuine HTTP(S) address; never turn a search into a URL here.
std::wstring Normalize(std::wstring input);

// Address-bar only: resolve an address or explicitly use the configured provider.
std::wstring ResolveInput(std::wstring input, SearchProvider provider);
std::wstring Search(std::wstring_view terms, SearchProvider provider);
std::wstring Resolve(std::wstring_view base, std::wstring_view reference);
}
