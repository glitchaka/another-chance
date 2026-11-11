// End-to-end Windows test. It launches the actual GUI executable and serves
// predictable HTML, CSS and image bytes over loopback HTTP. No external network
// or existing browser engines are involved.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string PixelBmp() {
    // Valid 1x1 pixel, uncompressed 24-bit BMP, 58 bytes.
    std::string bytes(58, '\0');
    bytes[0] = 'B';
    bytes[1] = 'M';
    bytes[2] = 58; // file size
    bytes[10] = 54; // pixel data offset
    bytes[14] = 40; // DIB header size
    bytes[18] = 1;  // width
    bytes[22] = 1;  // height
    bytes[26] = 1;  // planes
    bytes[28] = 24; // bits per pixel
    bytes[34] = 4;  // pixel bytes incl. padding
    bytes[54] = 0;  // blue
    bytes[55] = 0;  // green
    bytes[56] = static_cast<char>(255); // red
    return bytes;
}

class LocalServer {
public:
    LocalServer() {
        WSADATA data{};
        Check(WSAStartup(MAKEWORD(2,2), &data) == 0, "WSAStartup failed");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Check(listener_ != INVALID_SOCKET, "socket() failed");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(0);
        Check(bind(listener_, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0,
              "bind() failed");
        Check(listen(listener_, 8) == 0, "listen() failed");
        int length = sizeof(endpoint);
        Check(getsockname(listener_, reinterpret_cast<sockaddr*>(&endpoint), &length) == 0,
              "getsockname() failed");
        port_ = ntohs(endpoint.sin_port);
        worker_ = std::thread([this] { Serve(); });
    }
    ~LocalServer() {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        WSACleanup();
    }

    [[nodiscard]] unsigned Port() const { return port_; }
    std::atomic<int> htmlRequests{0};
    std::atomic<int> cssRequests{0};
    std::atomic<int> imageRequests{0};
    std::atomic<int> nextRequests{0};
    std::atomic<int> formRequests{0};
    std::atomic<int> postRequests{0};

private:
    SOCKET listener_ = INVALID_SOCKET;
    unsigned port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    void Serve() {
        while (!stopping_.load()) {
            fd_set available{};
            FD_ZERO(&available);
            FD_SET(listener_, &available);
            timeval timeout{0, 100000};
            if (select(0, &available, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            char buffer[4096]{};
            int count = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (count > 0) {
                std::string request(buffer, static_cast<size_t>(count));
                // WinHTTP may send headers and POST data in separate reads.
                // Read Content-Length rather than assuming recv() captured all.
                const auto headEnd=request.find("\r\n\r\n");
                const auto sizeAt=request.find("Content-Length:");
                if (headEnd!=std::string::npos &&
                    sizeAt!=std::string::npos && sizeAt<headEnd) {
                    const size_t begin=sizeAt+15;
                    const auto stop=request.find("\r\n",begin);
                    const auto digits=request.substr(begin,stop-begin);
                    const unsigned expected=static_cast<unsigned>(
                        std::strtoul(digits.c_str(),nullptr,10));
                    const size_t bodyStart=headEnd+4;
                    while (request.size()-bodyStart<expected && request.size()<65536) {
                        char more[4096];
                        const int got=recv(client,more,sizeof(more),0);
                        if (got<=0) break;
                        request.append(more,static_cast<size_t>(got));
                    }
                }
                std::string body;
                std::string type = "text/html; charset=utf-8";
                if (request.find("GET /start ") != std::string::npos) {
                    ++htmlRequests;
                    body = R"(<!doctype html><html><head><title>Inicio de prueba</title><link rel="stylesheet" href="/style.css"></head><body><a href="/next">Siguiente</a><img src="/pixel.bmp" alt="rojo">)";
                    for (int i=0; i<45; ++i) {
                        body += "<p>Fila de prueba de desplazamiento " +
                            std::to_string(i) + "</p>";
                    }
                    body += "</body></html>";
                } else if (request.find("GET /js-only ") != std::string::npos) {
                    body = "<!doctype html><html><head><title>Aplicación JS</title>"
                           "<style>#root{display:none}</style></head><body>"
                           "<div id='root'></div><script>fetch('/api').then("
                           "r=>r.json()).then(data=>document.body.innerHTML=data.html)"
                           "</script></body></html>";
                } else if (request.find("GET /inline-dom ") != std::string::npos) {
                    body = "<!doctype html><html><head><title>Inicial</title></head>"
                           "<body><main id='app'></main><script>"
                           "document.getElementById('app').innerHTML="
                           "'<h1>Contenido generado</h1>';"
                           "document.title='DOM activo';"
                           "</script></body></html>";
                } else if (request.find("GET /external-dom ") != std::string::npos) {
                    body = "<!doctype html><html><head><title>Externo</title></head>"
                           "<body><main id='app'></main>"
                           "<script src='/external.js'></script></body></html>";
                } else if (request.find("GET /external.js ") != std::string::npos) {
                    type = "text/javascript";
                    body = "document.getElementById('app').innerHTML="
                           "'<h1>Contenido externo</h1>';"
                           "document.title='Externo ejecutado';";
                } else if (request.find("GET /form-post ") != std::string::npos) {
                    body = "<html><head><title>Formulario POST</title></head><body>"
                           "<form action='/posted' method='post'>"
                           "<input name='q' placeholder='Nombre'>"
                           "<input type='hidden' name='lang' value='es'>"
                           "<button type='submit'>Enviar</button>"
                           "</form></body></html>";
                } else if (request.find("POST /posted ") != std::string::npos) {
                    const bool matched=request.find("q=prueba&lang=es")!=std::string::npos;
                    if (matched) ++postRequests;
                    body=matched ? "<html><title>POST recibido</title><body>OK</body></html>"
                                 : "<html><title>POST incorrecto</title><body>Error</body></html>";
                } else if (request.find("GET /form ") != std::string::npos) {
                    body = "<html><head><title>Formulario real</title></head><body>"
                           "<form action='/submitted' method='get'>"
                           "<input name='q' placeholder='Buscar'>"
                           "<input type='hidden' name='lang' value='es'>"
                           "<input type='submit' value='Buscar'>"
                           "</form></body></html>";
                } else if (request.find("GET /submitted?q=prueba&lang=es ") != std::string::npos) {
                    ++formRequests;
                    body = "<html><head><title>Formulario enviado</title></head>"
                           "<body><h1>Consulta recibida</h1></body></html>";
                } else if (request.find("GET /next ") != std::string::npos) {
                    ++nextRequests;
                    body = "<html><head><title>Destino de prueba</title></head><body><h1>Correcto</h1></body></html>";
                } else if (request.find("GET /style.css ") != std::string::npos) {
                    ++cssRequests;
                    type = "text/css";
                    body = "a { color: #224488; text-decoration: underline; }";
                } else if (request.find("GET /pixel.bmp ") != std::string::npos) {
                    ++imageRequests;
                    type = "image/bmp";
                    body = PixelBmp();
                } else {
                    body = "<title>404</title>";
                }
                std::string response =
                    "HTTP/1.1 200 OK\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                size_t offset = 0;
                while (offset < response.size()) {
                    int sent = send(client, response.data() + offset,
                                    static_cast<int>(response.size() - offset), 0);
                    if (sent <= 0) break;
                    offset += static_cast<size_t>(sent);
                }
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
};

class BrowserProcess {
public:
    explicit BrowserProcess(std::wstring executable, const std::wstring& url) {
        tracePath_ = std::filesystem::temp_directory_path() /
            L"another_chance_boot_trace.txt";
        std::filesystem::remove(tracePath_);
        SetEnvironmentVariableW(L"ANOTHER_CHANCE_TRACE_PATH", tracePath_.c_str());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring command = L"\"" + executable + L"\" --url \"" + url + L"\"";
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, nullptr, &startup, &process_) != FALSE,
              "CreateProcessW failed");
        window_ = WaitForWindow();
        Check(window_ != nullptr, "Browser window did not open");
    }
    ~BrowserProcess() {
        if (window_ && IsWindow(window_)) PostMessageW(window_, WM_CLOSE, 0, 0);
        if (process_.hProcess) {
            if (WaitForSingleObject(process_.hProcess, 16000) == WAIT_TIMEOUT) {
                TerminateProcess(process_.hProcess, 2);
                WaitForSingleObject(process_.hProcess, 3000);
            }
            CloseHandle(process_.hProcess);
        }
        if (process_.hThread) CloseHandle(process_.hThread);
    }
    HWND Window() const { return window_; }
    void Navigate(const std::wstring& address) {
        HWND field = FindWindowExW(window_, nullptr, L"Edit", nullptr);
        Check(field != nullptr, "Address bar missing");
        // Cross-process SetWindowText can modify the cached window caption
        // without exercising the actual EDIT control's input path.
        // Send keystrokes to the EDIT proc, as a user would type them.
        SendMessageW(field, EM_SETSEL, 0, -1);
        SendMessageW(field, WM_CLEAR, 0, 0);
        for (wchar_t character : address) {
            SendMessageW(field, WM_CHAR, static_cast<WPARAM>(character), 1);
        }
        wchar_t typed[2048]{};
        GetWindowTextW(field, typed, 2048);
        std::wcerr << L"DIAG input set: [" << typed << L"]\n";
        Check(std::wstring_view(typed) == address, "Address edit rejected test input");
        SendMessageW(window_, WM_COMMAND, MAKEWPARAM(104, BN_CLICKED), 0);
    }
    bool HasTitle(std::wstring_view expected) const {
        wchar_t title[512]{};
        GetWindowTextW(window_, title, 512);
        return std::wstring_view(title).find(expected) != std::wstring_view::npos;
    }
    bool StatusContains(std::wstring_view text) const {
        HWND label = FindWindowExW(window_, nullptr, L"Static", nullptr);
        if (!label) return false;
        wchar_t status[1024]{};
        GetWindowTextW(label, status, 1024);
        return std::wstring_view(status).find(text) != std::wstring_view::npos;
    }
    bool HasHttp200() const {
        HWND label = FindWindowExW(window_, nullptr, L"Static", nullptr);
        if (!label) return false;
        wchar_t status[1024]{};
        GetWindowTextW(label, status, 1024);
        return std::wstring_view(status).find(L"HTTP 200") != std::wstring_view::npos;
    }
    bool IsAlive() const {
        return WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT;
    }
    void ClickLink() {
        // First inline link at document (22,18) + 59px toolbar.
        PostMessageW(window_, WM_LBUTTONUP, MK_LBUTTON, MAKELPARAM(52, 104));
    }
    void Back() {
        PostMessageW(window_, WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), 0);
    }
private:
    PROCESS_INFORMATION process_{};
    HWND window_ = nullptr;
    std::filesystem::path tracePath_;

    HWND WaitForWindow() const {
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < 15000) {
            if (!IsAlive()) return nullptr;
            HWND found = FindWindowW(L"AnotherChance.NativeBrowser", nullptr);
            if (found && IsWindowVisible(found)) {
                wchar_t title[512]{};
                GetWindowTextW(found, title, 512);
                HWND field = FindWindowExW(found, nullptr, L"Edit", nullptr);
                HWND status = FindWindowExW(found, nullptr, L"Static", nullptr);
                wchar_t statusText[512]{};
                if (status) GetWindowTextW(status, statusText, 512);
                if (field && title[0] != L'\0' && statusText[0] != L'\0')
                    return found;
            }
            Sleep(100);
        }
        HWND found = FindWindowW(L"AnotherChance.NativeBrowser", nullptr);
        if (found) {
            wchar_t title[512]{};
            GetWindowTextW(found, title, 512);
            HWND status = FindWindowExW(found, nullptr, L"Static", nullptr);
            wchar_t statusText[512]{};
            if (status) GetWindowTextW(status, statusText, 512);
            std::wcerr << L"DIAG startup: title=[" << title
                       << L"] status=[" << statusText << L"]\n";
        }
        std::ifstream trace(tracePath_);
        std::string line;
        while (std::getline(trace, line)) std::cerr << "TRACE: " << line << "\n";
        return nullptr;
    }
};


void CaptureScreenshot(HWND hwnd, const char* filename = "AnotherChance-actual.bmp") {
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width < 100 || height < 100 || width > 4096 || height > 4096) return;

    HDC source = GetDC(hwnd);
    if (!source) return;
    HDC memory = CreateCompatibleDC(source);
    HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(hwnd, source);
        return;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL painted = PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT);
    if (!painted) {
        BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY);
    }
    SelectObject(memory, previous);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD pitch = static_cast<DWORD>((width * 3 + 3) & ~3);
    const DWORD pixels = pitch * static_cast<DWORD>(height);
    std::vector<BYTE> data(pixels);
    if (GetDIBits(source, bitmap, 0, static_cast<UINT>(height),
                  data.data(), &info, DIB_RGB_COLORS) == static_cast<int>(height)) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + pixels;
        std::ofstream file(filename, std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(hwnd, source);
}

template<typename Predicate>
bool Until(Predicate condition, unsigned timeoutMs) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        if (condition()) return true;
        Sleep(100);
    }
    return condition();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: AnotherChanceSmoke.exe <browser.exe> [--live]\n";
        return 2;
    }
    try {
        if (argc == 3) {
            if (std::wstring_view(argv[2]) != L"--live") return 2;
            // Release gate: a real website must be usable, not merely produce
            // HTTP 200 or a "no se pudo representar" fallback.
            // Captures are diagnostic and never substitutes for a passing test.
            struct Site {
                const wchar_t* url;
                const wchar_t* name;
                const char* screenshot;
            };
            const Site sites[] = {
                {L"https://www.google.com/search?q=google",
                 L"Google", "AnotherChance-live-google.bmp"},
                {L"https://archive.org/",
                 L"Archive", "AnotherChance-live-archive.bmp"}
            };
            bool allPassed = true;
            for (const auto& site : sites) {
                BrowserProcess browser(argv[1], site.url);
                const bool completed = Until([&] {
                    return !browser.IsAlive() ||
                           browser.HasHttp200() ||
                           browser.StatusContains(L"Sin contenido representable") ||
                           browser.StatusContains(L"Windows ");
                }, 50000);
                Sleep(500);
                CaptureScreenshot(browser.Window(), site.screenshot);
                const bool visible = browser.IsAlive() &&
                    SendMessageW(browser.Window(), WM_APP + 42, 0, 0) == 1;
                // A browser must support working page inputs, not just
                // draw fake square-bracket text in place of <input>.
                const HWND address = FindWindowExW(
                    browser.Window(), nullptr, L"Edit", nullptr);
                const HWND pageField = FindWindowExW(
                    browser.Window(), address, L"Edit", nullptr);
                const bool interactive = pageField != nullptr &&
                    IsWindowVisible(pageField) != FALSE;
                const bool success = completed && browser.HasHttp200() &&
                    visible && interactive;
                std::wcout << L"LIVE [" << site.name << L"]"
                           << L" completed=" << completed
                           << L" HTTP200=" << browser.HasHttp200()
                           << L" visible=" << visible
                           << L" pageInput=" << interactive << L"\n";
                if (!success) allPassed = false;
            }
            Check(allPassed,
                "The browser does not yet render and interact with real websites");
            return 0;
        }
        LocalServer server;
        std::wstring page = L"http://127.0.0.1:" +
            std::to_wstring(server.Port()) + L"/start";
        {
        BrowserProcess browser(argv[1], page);
        Check(browser.IsAlive(), "Browser process exited unexpectedly after startup");
        HICON extractedLarge = nullptr;
        HICON extractedSmall = nullptr;
        const UINT icons = ExtractIconExW(argv[1], 0, &extractedLarge, &extractedSmall, 1);
        if (icons != 1 || !extractedLarge || !extractedSmall) {
            std::wcerr << L"ICON diagnostic: executable=" << argv[1]
                       << L" returned=" << icons
                       << L" big=" << static_cast<void*>(extractedLarge)
                       << L" small=" << static_cast<void*>(extractedSmall)
                       << L" error=" << GetLastError() << L"\n";
        }
        Check(icons >= 1 && icons != UINT_MAX &&
              extractedLarge != nullptr && extractedSmall != nullptr,
              "Executable does not contain a real Windows icon resource");
        if (extractedLarge) DestroyIcon(extractedLarge);
        if (extractedSmall) DestroyIcon(extractedSmall);
        HICON windowIcon = reinterpret_cast<HICON>(
            SendMessageW(browser.Window(), WM_GETICON, ICON_SMALL, 0));
        if (!windowIcon) {
            windowIcon = reinterpret_cast<HICON>(
                GetClassLongPtrW(browser.Window(), GCLP_HICONSM));
        }
        Check(windowIcon != nullptr, "Window/taskbar icon is missing");
        std::cout << "PASS: icon embedded in EXE and registered on GUI window\n";
        const LONG_PTR style = GetWindowLongPtrW(browser.Window(), GWL_STYLE);
        Check((style & WS_CAPTION) == 0, "Unexpected native Windows caption");
        Check((style & WS_POPUP) == WS_POPUP, "Expected popup-style frameless window");
        RECT clientBounds{}, windowBounds{};
        GetClientRect(browser.Window(), &clientBounds);
        GetWindowRect(browser.Window(), &windowBounds);
        Check(clientBounds.right - clientBounds.left ==
              windowBounds.right - windowBounds.left,
              "Non-client window frame still consumes horizontal space");
        std::cout << "PASS: custom frameless Windows window\n";
        std::cout << "DIAG: browser startup launched with internal --url\n";
        const bool loaded = Until([&] { return browser.IsAlive() &&
            browser.HasTitle(L"Inicio de prueba") && browser.HasHttp200(); }, 10000);
        if (!loaded) {
            wchar_t title[1024]{};
            GetWindowTextW(browser.Window(), title, 1024);
            HWND label = FindWindowExW(browser.Window(), nullptr, L"Static", nullptr);
            wchar_t status[1024]{};
            if (label) GetWindowTextW(label, status, 1024);
            HWND field = FindWindowExW(browser.Window(), nullptr, L"Edit", nullptr);
            wchar_t address[1024]{};
            if (field) GetWindowTextW(field, address, 1024);
            std::wcerr << L"DIAG: alive=" << browser.IsAlive()
                       << L" title=[" << title << L"] status=[" << status
                       << L"] address=[" << address << L"]\n";
            wchar_t tracePath[2048]{};
            if (GetEnvironmentVariableW(L"ANOTHER_CHANCE_TRACE_PATH", tracePath, 2048)) {
                std::ifstream trace{std::filesystem::path(tracePath)};
                std::string line;
                while (std::getline(trace, line)) std::cerr << "TRACE: " << line << "\n";
            }
            std::cerr << "DIAG: GET /start=" << server.htmlRequests.load()
                      << " GET /style.css=" << server.cssRequests.load()
                      << " GET /pixel.bmp=" << server.imageRequests.load()
                      << "\n";
        }
        Check(loaded, "Could not render HTML fetched from loopback HTTP");
        std::cout << "PASS: open window + fetch HTML + parsed page title\n";

        Check(Until([&] { return server.cssRequests.load() > 0 &&
            server.imageRequests.load() > 0; }, 12000),
              "Page did not request external CSS and image");
        std::cout << "PASS: load external CSS and image resources\n";
        Sleep(550);
        CaptureScreenshot(browser.Window());

        // Scrolling must change the document but never trigger a header repaint.
        RedrawWindow(browser.Window(), nullptr, nullptr, RDW_UPDATENOW);
        constexpr UINT kCountHeaderPaints = WM_APP + 40;
        constexpr UINT kGetScrollY = WM_APP + 41;
        const LRESULT headerBefore = SendMessageW(
            browser.Window(), kCountHeaderPaints, 0, 0);
        const LRESULT scrollBefore = SendMessageW(
            browser.Window(), kGetScrollY, 0, 0);
        SendMessageW(browser.Window(), WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        RedrawWindow(browser.Window(), nullptr, nullptr, RDW_UPDATENOW);
        const LRESULT headerAfter = SendMessageW(
            browser.Window(), kCountHeaderPaints, 0, 0);
        const LRESULT scrollAfter = SendMessageW(
            browser.Window(), kGetScrollY, 0, 0);
        Check(scrollAfter > scrollBefore, "Mouse wheel did not move the document");
        Check(headerAfter == headerBefore,
              "Mouse wheel unnecessarily repainted the browser header");
        SendMessageW(browser.Window(), WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), 0);
        RedrawWindow(browser.Window(), nullptr, nullptr, RDW_UPDATENOW);
        Check(SendMessageW(browser.Window(), kGetScrollY, 0, 0) == scrollBefore,
              "Wheel-up did not restore scroll position");
        std::cout << "PASS: wheel scrolls page without repainting header\n";

        browser.ClickLink();
        Check(Until([&] { return browser.IsAlive() &&
            browser.HasTitle(L"Destino de prueba") && server.nextRequests.load() > 0; }, 15000),
              "Clicking a rendered anchor did not navigate");
        std::cout << "PASS: clickable link navigation\n";

        browser.Back();
        Check(Until([&] { return browser.IsAlive() &&
            browser.HasTitle(L"Inicio de prueba") && server.htmlRequests.load() > 1; }, 15000),
              "Browser back history did not navigate");
        std::cout << "PASS: back navigation\n";

        // Tabs are real controls: create a tab, then return to the first
        // and verify that its URL/history can be restored.
        PostMessageW(browser.Window(), WM_COMMAND, MAKEWPARAM(106, 0), 0);
        Check(Until([&] { return browser.IsAlive() &&
            browser.HasTitle(L"Another Chance") && !browser.HasHttp200(); }, 8000),
              "New tab did not open its home page");
        HWND statusLabel = FindWindowExW(browser.Window(), nullptr, L"Static", nullptr);
        wchar_t homeStatus[256]{};
        if (statusLabel) GetWindowTextW(statusLabel, homeStatus, 256);
        Check(wcscmp(homeStatus, L"Listo") == 0,
              "Home tab still displays internal engine diagnostics");
        CaptureScreenshot(browser.Window(), "AnotherChance-tabbed-ui.bmp");
        // Real pointer-based tab interactions, not direct WM_COMMAND shortcuts.
        const LRESULT countBefore=SendMessageW(browser.Window(),WM_APP+43,0,0);
        Check(countBefore==2,"new tab command should create two visible tabs");
        PostMessageW(browser.Window(),WM_LBUTTONUP,0,MAKELPARAM(470,18));
        Check(Until([&] {
            return SendMessageW(browser.Window(),WM_APP+43,0,0)==3 &&
                   SendMessageW(browser.Window(),WM_APP+44,0,0)==2;
        },5000),"new-tab '+' button was not clickable");
        // Close the active tab by clicking its close button.
        // At three tabs each has 224px; the third is x=456..677.
        PostMessageW(browser.Window(),WM_LBUTTONUP,0,MAKELPARAM(655,18));
        Check(Until([&] {
            return SendMessageW(browser.Window(),WM_APP+43,0,0)==2;
        },5000),"tab close button was not clickable");
        std::cout << "PASS: add and close native tabs with pointer clicks\n";
        std::cout << "PASS: create and activate a new tab\n";

        PostMessageW(browser.Window(), WM_COMMAND, MAKEWPARAM(300, 0), 0);
        Check(Until([&] { return browser.IsAlive() &&
            browser.HasTitle(L"Inicio de prueba") &&
            server.htmlRequests.load() >= 3; }, 14000),
              "Switching back to first tab did not restore navigation");
        std::cout << "PASS: switch tab and restore previous URL\n";
        } // close the first GUI before launching the second test instance
        {
        BrowserProcess jsBrowser(argv[1],
            L"http://127.0.0.1:" + std::to_wstring(server.Port()) + L"/js-only");
        Check(Until([&] {
            return jsBrowser.IsAlive() &&
                   jsBrowser.HasTitle(L"Aplicación JS") &&
                   jsBrowser.StatusContains(L"Sin contenido representable");
        }, 12000), "JS-only site did not report unsupported blank page");
        Sleep(200);
        CaptureScreenshot(jsBrowser.Window(), "AnotherChance-JS-fallback.bmp");
        std::cout << "PASS: unsupported JS remains visibly reported\n";
        } // JS-only browser closed
        {
            BrowserProcess scripted(argv[1],
                L"http://127.0.0.1:" + std::to_wstring(server.Port()) +
                L"/inline-dom");
            Check(Until([&] {
                return scripted.IsAlive() &&
                       scripted.HasTitle(L"DOM activo") &&
                       scripted.HasHttp200() &&
                       SendMessageW(scripted.Window(), WM_APP + 42, 0, 0) == 1;
            }, 14000), "Script-generated HTML did not reach the live renderer");
            CaptureScreenshot(scripted.Window(), "AnotherChance-scripted-dom.bmp");
            std::cout << "PASS: inline script generates visible native DOM\n";
        }
        {
            BrowserProcess controls(argv[1],
                L"http://127.0.0.1:" + std::to_wstring(server.Port()) +
                L"/form");
            Check(Until([&] {
                return controls.IsAlive() &&
                    controls.HasTitle(L"Formulario real") &&
                    SendMessageW(controls.Window(),WM_APP+45,0,0)==2;
            },12000),"HTML form did not create native Windows input controls");
            HWND input = FindWindowExW(controls.Window(),nullptr,L"Edit",nullptr);
            Check(input!=nullptr,"missing address input");
            input = FindWindowExW(controls.Window(),input,L"Edit",nullptr);
            Check(input!=nullptr && IsWindowVisible(input),
                  "page input must be a live visible Win32 Edit");
            // SetWindowTextW cannot edit an EDIT belonging to another
            // process. Send keystrokes to its control procedure instead.
            SendMessageW(input,EM_SETSEL,0,-1);
            SendMessageW(input,WM_CLEAR,0,0);
            for (wchar_t character : std::wstring_view(L"prueba")) {
                SendMessageW(input,WM_CHAR,static_cast<WPARAM>(character),1);
            }
            wchar_t typed[64]{};
            // WM_GETTEXT is explicitly marshalled across processes; the
            // convenience GetWindowTextW API may return an empty string for
            // another process's EDIT child even while the field has text.
            SendMessageW(input,WM_GETTEXT,64,reinterpret_cast<LPARAM>(typed));
            if (std::wstring_view(typed)!=L"prueba") {
                const auto result=SendMessageW(input,WM_SETTEXT,0,
                    reinterpret_cast<LPARAM>(L"prueba"));
                SendMessageW(input,WM_GETTEXT,64,reinterpret_cast<LPARAM>(typed));
                wchar_t name[80]{};
                GetClassNameW(input,name,80);
                std::wcerr<<L"FORM INPUT DIAG: WM_SETTEXT="<<result
                    <<L" class=["<<name<<L"] text=["<<typed
                    <<L"] visible="<<IsWindowVisible(input)
                    <<L" alive="<<controls.IsAlive()<<L"\n";
                CaptureScreenshot(controls.Window(),"AnotherChance-real-form.bmp");
            }
            Check(std::wstring_view(typed)==L"prueba",
                  "native HTML input did not accept typed characters");
            HWND submit = FindWindowExW(controls.Window(),nullptr,L"Button",nullptr);
            Check(submit!=nullptr && IsWindowVisible(submit),
                  "page submit must be a visible Win32 Button");
            CaptureScreenshot(controls.Window(),"AnotherChance-real-form.bmp");
            SetForegroundWindow(controls.Window());
            SendMessageW(submit,BM_CLICK,0,0);
            bool passed = Until([&] {
                return controls.IsAlive() &&
                    controls.HasTitle(L"Formulario enviado") &&
                    server.formRequests.load()>0;
            },5000);
            if (!passed) {
                HWND label=FindWindowExW(controls.Window(),nullptr,L"Static",nullptr);
                HWND urlBar=FindWindowExW(controls.Window(),nullptr,L"Edit",nullptr);
                wchar_t status[512]{},address[512]{},value[256]{};
                if(label) GetWindowTextW(label,status,512);
                if(urlBar) GetWindowTextW(urlBar,address,512);
                GetWindowTextW(input,value,256);
                std::wcerr<<L"FORM DIAG: buttonID="<<GetDlgCtrlID(submit)
                          <<L" status=["<<status<<L"] URL=["<<address
                          <<L"] input=["<<value<<L"] count="
                          <<server.formRequests.load()<<L"\n";
                // Diagnose whether cross-process BM_CLICK was ignored, while
                // keeping the regression test failed if the real click failed.
                PostMessageW(controls.Window(),WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(submit),BN_CLICKED),
                             reinterpret_cast<LPARAM>(submit));
                const bool commandWorked=Until([&] {
                    return server.formRequests.load()>0;
                },5000);
                std::cerr<<"FORM DIAG direct WM_COMMAND navigated="
                         <<commandWorked<<"\n";
            }
            Check(passed,"native form GET submission did not navigate");
            std::cout<<"PASS: native HTML input, button, hidden fields and GET submit\n";
        }
        {
            BrowserProcess posted(argv[1],
                L"http://127.0.0.1:"+std::to_wstring(server.Port())+L"/form-post");
            Check(Until([&]{
                return posted.IsAlive() &&
                    posted.HasTitle(L"Formulario POST") &&
                    SendMessageW(posted.Window(),WM_APP+45,0,0)==2;
            },14000),"POST form did not instantiate its controls");
            HWND field=FindWindowExW(posted.Window(),nullptr,L"Edit",nullptr);
            field=FindWindowExW(posted.Window(),field,L"Edit",nullptr);
            Check(field && IsWindowVisible(field),"POST form text input missing");
            SendMessageW(field,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"prueba"));
            HWND submit=FindWindowExW(posted.Window(),nullptr,L"Button",nullptr);
            Check(submit!=nullptr,"POST submit button missing");
            SendMessageW(submit,BM_CLICK,0,0);
            Check(Until([&]{
                return posted.IsAlive() &&
                    posted.HasTitle(L"POST recibido") &&
                    server.postRequests.load()>0;
            },15000),"HTTP POST did not transport encoded fields");
            std::cout<<"PASS: native HTML form encodes and submits POST body\n";
        }
        {
            BrowserProcess scripted(argv[1],
                L"http://127.0.0.1:" + std::to_wstring(server.Port()) +
                L"/external-dom");
            Check(Until([&] {
                return scripted.IsAlive() &&
                       scripted.HasTitle(L"Externo ejecutado") &&
                       scripted.HasHttp200() &&
                       SendMessageW(scripted.Window(), WM_APP + 42, 0, 0) == 1;
            }, 20000), "External JavaScript resource did not produce native DOM");
            CaptureScreenshot(scripted.Window(), "AnotherChance-external-dom.bmp");
            std::cout << "PASS: external script resource generated visible DOM\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
