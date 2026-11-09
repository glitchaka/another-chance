// End-to-end Windows test. It launches the actual GUI executable and serves
// predictable HTML, CSS and image bytes over loopback HTTP. No external network
// or existing browser engines are involved.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>
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
                std::string body;
                std::string type = "text/html; charset=utf-8";
                if (request.find("GET /start ") != std::string::npos) {
                    ++htmlRequests;
                    body = R"(<!doctype html><html><head><title>Inicio de prueba</title><link rel="stylesheet" href="/style.css"></head><body><a href="/next">Siguiente</a><img src="/pixel.bmp" alt="rojo"></body></html>)";
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
    explicit BrowserProcess(std::wstring executable) {
        tracePath_ = std::filesystem::temp_directory_path() /
            L"another_chance_boot_trace.txt";
        std::filesystem::remove(tracePath_);
        SetEnvironmentVariableW(L"ANOTHER_CHANCE_TRACE_PATH", tracePath_.c_str());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring command = L"\"" + executable + L"\"";
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
        Check(SetWindowTextW(field, address.c_str()) != FALSE, "Could not type address");
        Check(PostMessageW(window_, WM_COMMAND, MAKEWPARAM(104, BN_CLICKED), 0) != FALSE,
              "Go command failed");
    }
    bool HasTitle(std::wstring_view expected) const {
        wchar_t title[512]{};
        GetWindowTextW(window_, title, 512);
        return std::wstring_view(title).find(expected) != std::wstring_view::npos;
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
        PostMessageW(window_, WM_LBUTTONUP, MK_LBUTTON, MAKELPARAM(52, 83));
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
                if (field && title[0] != L'\0' &&
                    std::wstring_view(statusText).find(L"Listo para navegar") !=
                    std::wstring_view::npos) return found;
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
    if (argc != 2) {
        std::cerr << "Usage: AnotherChanceSmoke.exe <path-to-browser.exe>\n";
        return 2;
    }
    try {
        LocalServer server;
        BrowserProcess browser(argv[1]);
        Check(browser.IsAlive(), "Browser process exited unexpectedly after startup");
        std::wstring page = L"http://127.0.0.1:" +
            std::to_wstring(server.Port()) + L"/start";
        std::cout << "DIAG: browser startup reached ready state\n";
        browser.Navigate(page);
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
                std::ifstream trace(std::filesystem::path(tracePath));
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
