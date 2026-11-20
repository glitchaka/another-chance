#pragma once
#include "script/ScriptRuntime.h"
#include <atomic>

namespace ac::script {
// The browser-owned JavaScript interpreter. This is a developing subset of
// ECMAScript, NOT a complete or standards-conformant implementation.
Execution ExecuteNative(html::Document& document, std::wstring_view baseUrl,
    const std::unordered_map<std::string,std::string>& external,
    const std::atomic<unsigned>* navigationGeneration = nullptr,
    unsigned expectedGeneration = 0);
}
