#pragma once
#include <cstddef>
#include <string>

namespace ac::net {
struct Response {
    std::wstring url;
    std::wstring contentType;
    std::wstring error;
    unsigned status = 0;
    std::string body;
    [[nodiscard]] bool Ok() const { return error.empty(); }
};

class HttpClient {
public:
    [[nodiscard]] Response Get(const std::wstring& url,
                               size_t maxBytes = 8 * 1024 * 1024) const;
    [[nodiscard]] Response Post(const std::wstring& url,
                                const std::string& urlEncodedBody,
                                size_t maxBytes = 8 * 1024 * 1024) const;
private:
    [[nodiscard]] Response Request(const std::wstring& url,
                                   const wchar_t* method,
                                   const std::string& urlEncodedBody,
                                   size_t maxBytes) const;
};
}
