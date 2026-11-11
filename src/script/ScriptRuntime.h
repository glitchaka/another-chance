#pragma once
#include "html/HtmlParser.h"
#include <cstddef>

namespace ac::script {
struct Execution {
    size_t scripts = 0;
    size_t operations = 0;
    size_t unsupported = 0;
};
// Restricted stage-1 DOM string operations, not an ECMAScript engine.
Execution ExecuteInline(html::Document& document);
}
