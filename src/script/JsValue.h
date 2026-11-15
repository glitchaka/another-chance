#pragma once
#include "html/HtmlParser.h"
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ac::script::js {
struct Value {
    enum class Kind { Undefined, Null, Number, Text, Boolean, Element, Document, Location, Window, Array, Function, Object, Math, Json };
    Kind kind=Kind::Undefined;
    double number=0;
    std::string text;
    html::Node* element=nullptr;
    std::shared_ptr<std::vector<Value>> array;
    std::shared_ptr<std::unordered_map<std::string,Value>> object;
    static Value ObjectValue(std::unordered_map<std::string,Value> props) {
        Value v;v.kind=Kind::Object;
        v.object=std::make_shared<std::unordered_map<std::string,Value>>(std::move(props));
        return v;
    }
    static Value ArrayValue(std::vector<Value> items) {
        Value v;v.kind=Kind::Array;
        v.array=std::make_shared<std::vector<Value>>(std::move(items));
        return v;
    }
    static Value Num(double n) {Value v;v.kind=Kind::Number;v.number=n;return v;}
    static Value Str(std::string s) {Value v;v.kind=Kind::Text;v.text=std::move(s);return v;}
    static Value Bool(bool b) {Value v;v.kind=Kind::Boolean;v.number=b?1:0;return v;}
    static Value NodeValue(html::Node* n) {
        Value v;
        if(!n) {v.kind=Kind::Null;return v;}
        v.kind=Kind::Element;v.element=n;return v;
    }
    static Value Special(Kind k) {Value v;v.kind=k;return v;}
    std::string String() const {
        if(kind==Kind::Text) return text;
        if(kind==Kind::Boolean) return number?"true":"false";
        if(kind==Kind::Number) {
            if(std::isnan(number)) return "NaN";
            if(std::isinf(number)) return std::signbit(number)?"-Infinity":"Infinity";
            if(number==0) return "0";
            std::ostringstream s;s<<std::setprecision(15)<<number;return s.str();
        }
        if(kind==Kind::Array && array) {
            std::string result;
            for(size_t index=0;index<array->size();++index) {
                if(index) result += ",";
                const auto& item=(*array)[index];
                if(item.kind!=Kind::Undefined && item.kind!=Kind::Null)
                    result += item.String();
            }
            return result;
        }
        if(kind==Kind::Null) return "null";
        if(kind==Kind::Undefined) return "undefined";
        return "[object Object]";
    }
    double Number() const {
        if(kind==Kind::Number || kind==Kind::Boolean) return number;
        if(kind==Kind::Null) return 0;
        if(kind==Kind::Text) {
            char* end=nullptr;
            double x=std::strtod(text.c_str(),&end);
            if(end!=text.c_str() && *end==0) return x;
            if(text.empty()) return 0;
        }
        return std::nan("");
    }
    bool Truth() const {
        if(kind==Kind::Undefined || kind==Kind::Null) return false;
        if(kind==Kind::Text) return !text.empty();
        if(kind==Kind::Boolean || kind==Kind::Number) return number!=0 && !std::isnan(number);
        return true;
    }
};
}
