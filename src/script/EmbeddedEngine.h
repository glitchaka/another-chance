#pragma once
#include "script/ScriptRuntime.h"
namespace ac::script {
// ECMAScript execution with a restricted browser DOM adapter. This adapter is
// not yet an implementation of the complete HTML DOM or Web APIs.
Execution ExecuteEmbedded(html::Document& document, std::wstring_view baseUrl,
                          const std::unordered_map<std::string, std::string>& external);
}
