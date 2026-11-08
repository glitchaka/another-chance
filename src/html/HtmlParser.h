#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ac::html {
struct Node {
    enum class Type { Element, Text };
    Type type = Type::Element;
    std::string tag;
    std::string text;
    std::unordered_map<std::string, std::string> attributes;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;

    [[nodiscard]] std::string Attribute(std::string_view name) const;
};

struct Document {
    std::unique_ptr<Node> root;
    std::string title;
    std::string embeddedCss;
};

[[nodiscard]] Document Parse(std::string_view html);
}
