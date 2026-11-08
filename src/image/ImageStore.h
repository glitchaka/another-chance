#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>

namespace ac::image {
struct Bitmap {
    HBITMAP handle = nullptr;
    int width = 0;
    int height = 0;
};

class Store final {
public:
    Store() = default;
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Decode bytes through Windows Imaging Component; no external browser engine.
    bool Decode(const std::wstring& url, const std::string& bytes);
    [[nodiscard]] const Bitmap* Find(const std::wstring& url) const;
    void Clear();

private:
    std::unordered_map<std::wstring, Bitmap> images_;
};
}
