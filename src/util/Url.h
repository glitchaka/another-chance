#pragma once
#include <string>
#include <string_view>

namespace ac::url {
std::wstring ToWide(std::string_view utf8);
std::string ToUtf8(std::wstring_view wide);
std::wstring Normalize(std::wstring input);
std::wstring Resolve(std::wstring_view base, std::wstring_view reference);
bool IsWeb(std::wstring_view input);
}
