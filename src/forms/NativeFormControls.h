#pragma once
#include "layout/Layout.h"
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace ac::forms {

// Live Win32 form widgets bound to the original HTML node, independent from
// layout and networking. No embedded browser engine or WebView is used.
class NativeFormControls final {
public:
    static constexpr UINT kSubmitByEnter = WM_APP + 52;
    NativeFormControls() = default;
    ~NativeFormControls() { Clear(); }
    NativeFormControls(const NativeFormControls&) = delete;
    NativeFormControls& operator=(const NativeFormControls&) = delete;

    void Clear();
    void Sync(HWND parent, HINSTANCE instance,
              const std::vector<layout::PageLayout::Control>& layout,
              const RECT& viewport, int scroll, HFONT font);
    void Reposition(const RECT& viewport, int scroll);
    [[nodiscard]] bool Owns(HWND hwnd) const;
    [[nodiscard]] bool IsSubmit(HWND hwnd) const;
    [[nodiscard]] bool IsReset(HWND hwnd) const;
    [[nodiscard]] std::wstring SubmitGet(HWND initiator,
                                          std::wstring_view pageUrl) const;
    void ResetForm(HWND initiator);
    [[nodiscard]] size_t Count() const { return fields_.size(); }

private:
    struct Field {
        HWND hwnd = nullptr;
        const html::Node* node = nullptr;
        layout::PageLayout::Control::Kind kind{};
        RECT bounds{};
    };
    std::vector<Field> fields_;
    HWND parent_ = nullptr;

    [[nodiscard]] const Field* Find(HWND hwnd) const;
    [[nodiscard]] static const html::Node* ParentForm(const html::Node* node);
    [[nodiscard]] std::wstring Value(const Field& field) const;
    static LRESULT CALLBACK TextProc(HWND hwnd, UINT message, WPARAM w,
                                     LPARAM l, UINT_PTR id, DWORD_PTR context);
};
} // namespace ac::forms
