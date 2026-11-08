#include "window/BrowserWindow.h"
#include <windows.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    HWND window = ac::BrowserWindow::Create(instance);
    if (!window) {
        MessageBoxW(nullptr, L"No se pudo crear la ventana de Another Chance.",
                    L"Another Chance", MB_ICONERROR | MB_OK);
        return 1;
    }

    ACCEL shortcuts[] = {
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'L', ac::BrowserWindow::kFocus},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'R', ac::BrowserWindow::kReload},
        {static_cast<BYTE>(FVIRTKEY), VK_F5, ac::BrowserWindow::kReload},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_LEFT, ac::BrowserWindow::kBack},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_RIGHT, ac::BrowserWindow::kForward}
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
    return static_cast<int>(message.wParam);
}
