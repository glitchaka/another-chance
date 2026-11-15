#pragma once
#include "script/JsValue.h"
#include <string>
#include <string_view>
namespace ac::script::js {
Value ParseJson(std::string_view input);
std::string StringifyJson(const Value& value);
}
