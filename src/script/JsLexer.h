#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ac::script::js {
enum class TokenKind { Identifier, Number, String, RegExp, Symbol, End };
struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    size_t offset = 0;
};
// Independent tokenizer: whitespace, UTF-8 string literals, escapes and
// JavaScript comments. It does not depend on any external runtime.
std::vector<Token> Tokenize(std::string_view source);
}
