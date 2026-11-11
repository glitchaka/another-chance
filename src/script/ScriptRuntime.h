#pragma once
#include "html/HtmlParser.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ac::script {
struct Execution {
    size_t scripts = 0;
    size_t operations = 0;
    size_t unsupported = 0;
};
// Restricted stage-1 DOM string operations, not an ECMAScript engine.
Execution ExecuteInline(html::Document& document);
Execution Execute(html::Document& document, std::wstring_view baseUrl,
                  const std::unordered_map<std::string,std::string>& scripts);
}
