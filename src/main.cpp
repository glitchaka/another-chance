#include "window/BrowserWindow.h"
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    std::wstring initialUrl;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--url") {
                initialUrl = argv[i + 1];
                break;
            }
        }
        LocalFree(argv);
    }

    HWND window = ac::BrowserWindow::Create(instance, std::move(initialUrl));
    if (!window) {
        MessageBoxW(nullptr, L"No se pudo crear la ventana de Another Chance.",
                    L"Another Chance", MB_ICONERROR | MB_OK);
        if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    ACCEL shortcuts[] = {
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'L', ac::BrowserWindow::kFocus},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'R', ac::BrowserWindow::kReload},
        {static_cast<BYTE>(FVIRTKEY), VK_F5, ac::BrowserWindow::kReload},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_LEFT, ac::BrowserWindow::kBack},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_RIGHT, ac::BrowserWindow::kForward},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'T', ac::BrowserWindow::kNewTab},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'W', ac::BrowserWindow::kCloseTab}
    };
    HACCEL accelerators = CreateAcceleratorTableW(
        shortcuts, static_cast<int>(sizeof(shortcuts) / sizeof(shortcuts[0])));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!accelerators || !TranslateAcceleratorW(window, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (accelerators) DestroyAcceleratorTable(accelerators);
    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
