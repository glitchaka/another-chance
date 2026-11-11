#pragma once
#include <string>
#include <string_view>

namespace ac::html {
// HTML5-compatible common encodings. WinHTTP returns bytes; the HTML parser
// consumes UTF-8. This conversion must happen before tokenization.
std::string DecodeToUtf8(std::string_view bytes,
                         std::wstring_view contentType = {});
} // namespace ac::html
