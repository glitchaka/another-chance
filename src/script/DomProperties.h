#pragma once
#include "html/HtmlParser.h"
#include <string>
#include <string_view>

namespace ac::script::dom {
std::string SerializeChildren(const html::Node& node);
std::string CssPropertyName(std::string_view javascriptName);
std::string GetStyle(const html::Node& element,std::string_view property);
void SetStyle(html::Node& element,std::string_view property,std::string_view value);
bool ContainsClass(const html::Node& element,std::string_view className);
void SetClass(html::Node& element,std::string_view className,bool enabled);
bool ToggleClass(html::Node& element,std::string_view className);
}
