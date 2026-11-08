#include "window/BrowserWindow.h"
#include "util/Url.h"
#include <windowsx.h>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <unordered_set>

namespace ac {
namespace {
constexpr wchar_t kClassName[] = L"AnotherChance.NativeBrowser";

void SetControlFont(HWND handle) {
    SendMessageW(handle, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void CollectStyleSheets(const ac::html::Node& node, const std::wstring& base,
                        std::vector<std::wstring>& urls,
                        std::unordered_set<std::wstring>& known) {
    if (urls.size() >= 4) return;
    if (node.tag == "link" && node.Attribute("rel").find("stylesheet") != std::string::npos) {
        const auto url = ac::url::Resolve(base, ac::url::ToWide(node.Attribute("href")));
        if (ac::url::IsWeb(url) && known.insert(url).second) urls.push_back(url);
    }
    for (const auto& child : node.children) {
        CollectStyleSheets(*child, base, urls, known);
    }
}

void CollectImages(const ac::html::Node& node, const std::wstring& base,
                   std::vector<std::wstring>& urls,
                   std::unordered_set<std::wstring>& known) {
    if (urls.size() >= 12) return;
    if (node.tag == "img") {
        const auto url = ac::url::Resolve(base, ac::url::ToWide(node.Attribute("src")));
        if (ac::url::IsWeb(url) && known.insert(url).second) urls.push_back(url);
    }
    for (const auto& child : node.children) {
        CollectImages(*child, base, urls, known);
    }
}

std::string EscapeHtml(const std::string& input) {
    std::string result;
    for (char ch : input) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += ch; break;
        }
    }
    return result;
}
} // namespace

BrowserWindow::~BrowserWindow() {
    StopWorker();
}

HWND BrowserWindow::Create(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BrowserWindow::StaticProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    auto* window = new BrowserWindow();
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Another Chance",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1060, 780,
        nullptr, nullptr, instance, window);
    if (!hwnd) {
        delete window;
        return nullptr;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

LRESULT CALLBACK BrowserWindow::StaticProc(HWND hwnd, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* instance = static_cast<BrowserWindow*>(create->lpCreateParams);
        instance->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        return TRUE;
    }
    auto* instance = reinterpret_cast<BrowserWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!instance) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
        delete instance;
        return result;
    }
    return instance->HandleMessage(message, wparam, lparam);
}

LRESULT CALLBACK BrowserWindow::AddressProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    auto* instance = reinterpret_cast<BrowserWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!instance || !instance->oldAddressProc_) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        PostMessageW(instance->hwnd_, WM_COMMAND, MAKEWPARAM(kGo, BN_CLICKED), 0);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        SetWindowTextW(hwnd, instance->currentUrl_.c_str());
        return 0;
    }
    return CallWindowProcW(instance->oldAddressProc_, hwnd, message, wparam, lparam);
}

void BrowserWindow::OnCreate() {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    auto button = [this, instance](WORD identifier, const wchar_t* caption) {
        HWND handle = CreateWindowExW(0, L"BUTTON", caption,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 70, 30, hwnd_,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(identifier)), instance, nullptr);
        SetControlFont(handle);
        return handle;
    };
    back_ = button(kBack, L"\u2190 Atrás");
    forward_ = button(kForward, L"Adelante \u2192");
    reload_ = button(kReload, L"\u21bb");
    address_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 30, hwnd_,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kGo + 100)),
        instance, nullptr);
    SetControlFont(address_);
    go_ = button(kGo, L"Ir");
    status_ = CreateWindowExW(0, L"STATIC", L"Preparando motor web original...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 26, hwnd_, nullptr, instance, nullptr);
    SetControlFont(status_);

    SetWindowLongPtrW(address_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    oldAddressProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        address_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BrowserWindow::AddressProc)));
    RECT client{};
    GetClientRect(hwnd_, &client);
    Resize(client.right, client.bottom);

    worker_ = std::thread(&BrowserWindow::WorkerLoop, this);
    document_ = std::make_unique<html::Document>(html::Parse(R"HTML(
        <html><head><title>Another Chance</title>
        <style>h1 {color:#24344a;} p {color:#526175;} a {color:#1762bf;}</style></head>
        <body>
          <h1>Another Chance</h1>
          <p>Navegador nativo con motor HTML y CSS propio.</p>
          <p>Introduce una direccion o una busqueda en la barra superior.</p>
          <p><a href="https://example.org/">Abrir pagina de ejemplo</a></p>
          <p>Nota: esta version experimental no ejecuta JavaScript y no soporta
          todos los sitios web modernos.</p>
        </body></html>
    )HTML"));
    sheet_.Parse(document_->embeddedCss);
    Reflow();
    SetStatus(L"Listo para navegar | Motor HTML/CSS original");
}

void BrowserWindow::OnDestroy() {
    StopWorker();
    PostQuitMessage(0);
}

void BrowserWindow::StopWorker() {
    {
        std::lock_guard guard(taskMutex_);
        stopping_ = true;
        while (!pendingJobs_.IsEmpty()) delete pendingJobs_.RemoveFirst();
    }
    taskCondition_.notify_all();
    if (worker_.joinable()) worker_.join();

    if (hwnd_) {
        MSG message{};
        while (PeekMessageW(&message, hwnd_, kPageReady, kPageReady, PM_REMOVE)) {
            delete reinterpret_cast<LoadedPage*>(message.lParam);
        }
        while (PeekMessageW(&message, hwnd_, kImageReady, kImageReady, PM_REMOVE)) {
            delete reinterpret_cast<LoadedImage*>(message.lParam);
        }
        while (PeekMessageW(&message, hwnd_, kStyleReady, kStyleReady, PM_REMOVE)) {
            delete reinterpret_cast<LoadedStyle*>(message.lParam);
        }
    }
}

void BrowserWindow::WorkerLoop() {
    net::HttpClient http;
    while (true) {
        std::unique_ptr<NavigationJob> job;
        {
            std::unique_lock lock(taskMutex_);
            taskCondition_.wait(lock, [this] {
                return stopping_ || !pendingJobs_.IsEmpty();
            });
            if (stopping_) break;
            job.reset(pendingJobs_.RemoveFirst());
        }
        if (!job) continue;
        auto result = std::make_unique<LoadedPage>();
        result->generation = job->generation;
        result->response = http.Get(job->url);
        if (result->response.Ok()) {
            const auto& response = result->response;
            if (!response.contentType.empty() &&
                response.contentType.find(L"application/pdf") != std::wstring::npos) {
                result->document = html::Parse(
                    "<h1>Formato no admitido</h1><p>Esta versión no incorpora visor PDF.</p>");
            } else if (response.contentType.find(L"text/plain") != std::wstring::npos) {
                result->document = html::Parse(
                    "<h1>Documento de texto</h1><p>" + EscapeHtml(response.body) + "</p>");
            } else {
                result->document = html::Parse(response.body);
            }
        } else {
            result->document = html::Parse(
                "<html><body><h1>No se pudo abrir la página</h1><p>" +
                EscapeHtml(url::ToUtf8(result->response.error)) +
                "</p><p>Comprueba la dirección y la conexión.</p></body></html>");
        }
        std::vector<std::wstring> imageUrls;
        std::vector<std::wstring> cssUrls;
        std::unordered_set<std::wstring> knownImages;
        std::unordered_set<std::wstring> knownCss;
        if (result->document.root) {
            CollectImages(*result->document.root, result->response.url,
                          imageUrls, knownImages);
            CollectStyleSheets(*result->document.root, result->response.url,
                               cssUrls, knownCss);
        }
        if (!PostMessageW(hwnd_, kPageReady, 0,
                          reinterpret_cast<LPARAM>(result.get()))) {
            // Window has been destroyed; unique_ptr frees it.
        } else {
            result.release();
        }

        // External CSS is fetched after the initial page; it never executes scripts.
        for (const auto& cssUrl : cssUrls) {
            if (job->generation != newestRequest_.load()) break;
            {
                std::lock_guard guard(taskMutex_);
                if (stopping_) break;
            }
            auto cssResponse = http.Get(cssUrl, 512 * 1024);
            if (!cssResponse.Ok() || cssResponse.status >= 400 ||
                cssResponse.body.empty()) continue;
            auto loadedCss = std::make_unique<LoadedStyle>();
            loadedCss->generation = job->generation;
            loadedCss->css = std::move(cssResponse.body);
            if (PostMessageW(hwnd_, kStyleReady, 0,
                             reinterpret_cast<LPARAM>(loadedCss.get()))) {
                loadedCss.release();
            }
        }

        // Images arrive progressively, after text, without blocking first paint.
        for (const auto& imageUrl : imageUrls) {
            if (job->generation != newestRequest_.load()) break;
            {
                std::lock_guard guard(taskMutex_);
                if (stopping_) break;
            }
            auto picture = http.Get(imageUrl, 4 * 1024 * 1024);
            if (!picture.Ok() || picture.status >= 400 || picture.body.empty()) continue;
            auto imageResult = std::make_unique<LoadedImage>();
            imageResult->generation = job->generation;
            imageResult->url = imageUrl;
            imageResult->bytes = std::move(picture.body);
            if (PostMessageW(hwnd_, kImageReady, 0,
                             reinterpret_cast<LPARAM>(imageResult.get()))) {
                imageResult.release();
            }
        }
    }
}

void BrowserWindow::SetStatus(const std::wstring& message) {
    if (status_) SetWindowTextW(status_, message.c_str());
}

void BrowserWindow::Navigate(std::wstring address, bool recordHistory) {
    address = url::Normalize(std::move(address));
    if (address.empty()) {
        SetStatus(L"Dirección no admitida. Escribe una URL HTTP/HTTPS o una búsqueda.");
        return;
    }
    if (recordHistory) {
        if (!history_.empty()) {
            history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(historyIndex_ + 1),
                           history_.end());
        }
        if (history_.empty() || history_.back() != address) {
            history_.push_back(address);
        }
        historyIndex_ = history_.size() - 1;
    }
    currentUrl_ = address;
    SetWindowTextW(address_, address.c_str());
    UpdateButtons();
    isLoading_ = true;
    SetStatus(L"Cargando " + address + L"...");
    const unsigned id = ++generation_;
    newestRequest_.store(id);
    {
        std::lock_guard guard(taskMutex_);
        if (stopping_) return;
        // Keep only the newest queued navigation; stale network results are ignored.
        while (!pendingJobs_.IsEmpty()) delete pendingJobs_.RemoveFirst();
        pendingJobs_.Add(new NavigationJob(id, address));
    }
    taskCondition_.notify_one();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::GoBack() {
    if (historyIndex_ == 0 || history_.empty()) return;
    --historyIndex_;
    Navigate(history_[historyIndex_], false);
}
void BrowserWindow::GoForward() {
    if (history_.empty() || historyIndex_ + 1 >= history_.size()) return;
    ++historyIndex_;
    Navigate(history_[historyIndex_], false);
}
void BrowserWindow::Reload() {
    if (!currentUrl_.empty()) Navigate(currentUrl_, false);
}
void BrowserWindow::UpdateButtons() {
    EnableWindow(back_, !history_.empty() && historyIndex_ > 0);
    EnableWindow(forward_, !history_.empty() && historyIndex_ + 1 < history_.size());
    EnableWindow(reload_, !currentUrl_.empty());
}

std::wstring BrowserWindow::AddressText() const {
    const int count = GetWindowTextLengthW(address_);
    std::wstring result(static_cast<size_t>(count) + 1, L'\0');
    GetWindowTextW(address_, result.data(), count + 1);
    result.resize(static_cast<size_t>(count));
    return result;
}

RECT BrowserWindow::ContentRect() const {
    return {0, kToolbarHeight, clientWidth_,
            std::max(kToolbarHeight, clientHeight_ - kStatusHeight)};
}
int BrowserWindow::MaxScroll() const {
    const RECT content = ContentRect();
    return std::max(0, layout_.Height() - static_cast<int>(content.bottom - content.top));
}

void BrowserWindow::Resize(int width, int height) {
    clientWidth_ = std::max(1, width);
    clientHeight_ = std::max(1, height);
    const int buttonY = 13;
    MoveWindow(back_, 10, buttonY, 80, 32, TRUE);
    MoveWindow(forward_, 96, buttonY, 98, 32, TRUE);
    MoveWindow(reload_, 200, buttonY, 42, 32, TRUE);
    const int addressWidth = std::max(140, width - 336);
    MoveWindow(address_, 250, buttonY, addressWidth, 32, TRUE);
    MoveWindow(go_, std::max(396, width - 78), buttonY, 68, 32, TRUE);
    MoveWindow(status_, 12, std::max(kToolbarHeight, height - kStatusHeight + 5),
               std::max(100, width - 24), 20, TRUE);
    if (document_) Reflow();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::Reflow() {
    if (!document_ || !hwnd_) return;
    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    layout_.Reflow(dc, *document_, sheet_, images_, currentUrl_, clientWidth_);
    ReleaseDC(hwnd_, dc);
    scrollY_ = std::clamp(scrollY_, 0, MaxScroll());
}

void BrowserWindow::OnPageReady(LoadedPage* rawResult) {
    std::unique_ptr<LoadedPage> result(rawResult);
    if (!result || result->generation != generation_) return;
    isLoading_ = false;
    if (url::IsWeb(result->response.url)) {
        currentUrl_ = result->response.url;
        SetWindowTextW(address_, currentUrl_.c_str());
        if (!history_.empty()) history_[historyIndex_] = currentUrl_;
    }
    images_.Clear();
    document_ = std::make_unique<html::Document>(std::move(result->document));
    sheet_.Parse(document_->embeddedCss);
    scrollY_ = 0;
    Reflow();
    const std::wstring title = url::ToWide(document_->title);
    SetWindowTextW(hwnd_, (title.empty() ? L"Another Chance" :
        title + L" — Another Chance").c_str());
    if (result->response.Ok()) {
        SetStatus(L"HTTP " + std::to_wstring(result->response.status) +
                  L"  |  Motor de páginas original (sin JavaScript)");
    } else {
        SetStatus(result->response.error);
    }
    UpdateButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::OnStyleReady(LoadedStyle* raw) {
    std::unique_ptr<LoadedStyle> style(raw);
    if (!style || style->generation != generation_ || !document_) return;
    if (document_->embeddedCss.size() + style->css.size() > 2 * 1024 * 1024) return;
    document_->embeddedCss.append("\n");
    document_->embeddedCss.append(style->css);
    sheet_.Parse(document_->embeddedCss);
    Reflow();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::OnImageReady(LoadedImage* raw) {
    std::unique_ptr<LoadedImage> image(raw);
    if (!image || image->generation != generation_) return;
    if (images_.Decode(image->url, image->bytes)) {
        Reflow();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void BrowserWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{0, 0, clientWidth_, clientHeight_};
    HBRUSH background = CreateSolidBrush(RGB(255,255,255));
    FillRect(dc, &client, background);
    DeleteObject(background);

    const RECT toolbar{0, 0, clientWidth_, kToolbarHeight};
    HBRUSH toolbarBrush = CreateSolidBrush(RGB(29,35,45));
    FillRect(dc, &toolbar, toolbarBrush);
    DeleteObject(toolbarBrush);

    const RECT statusRect{0, std::max(kToolbarHeight, clientHeight_ - kStatusHeight),
        clientWidth_, clientHeight_};
    HBRUSH statusBrush = CreateSolidBrush(RGB(242,244,247));
    FillRect(dc, &statusRect, statusBrush);
    DeleteObject(statusBrush);

    RECT view = ContentRect();
    if (document_) {
        layout_.Draw(dc, view.top, scrollY_, view);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(80, 88, 99));
        TextOutW(dc, 28, kToolbarHeight + 30,
                 L"Cargando la primera página...", 29);
    }
    EndPaint(hwnd_, &paint);
}

LRESULT BrowserWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_SIZE:
            Resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case kPageReady:
            OnPageReady(reinterpret_cast<LoadedPage*>(lparam));
            return 0;
        case kImageReady:
            OnImageReady(reinterpret_cast<LoadedImage*>(lparam));
            return 0;
        case kStyleReady:
            OnStyleReady(reinterpret_cast<LoadedStyle*>(lparam));
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kBack: GoBack(); return 0;
                case kForward: GoForward(); return 0;
                case kReload: Reload(); return 0;
                case kGo: Navigate(AddressText(), true); return 0;
                case kFocus:
                    SetFocus(address_);
                    SendMessageW(address_, EM_SETSEL, 0, -1);
                    return 0;
                default: break;
            }
            break;
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            scrollY_ = std::clamp(scrollY_ - notches * 96, 0, MaxScroll());
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const auto content = ContentRect();
            if (y >= content.top && y < content.bottom && document_) {
                const std::wstring link = layout_.LinkAt(x, y, content.top, scrollY_);
                if (!link.empty()) Navigate(link, true);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && document_) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd_, &point);
                const RECT content = ContentRect();
                if (point.y >= content.top && point.y < content.bottom &&
                    !layout_.LinkAt(point.x, point.y, content.top, scrollY_).empty()) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_NEXT || wparam == VK_DOWN) {
                scrollY_ = std::min(MaxScroll(), scrollY_ +
                    (wparam == VK_NEXT ? 480 : 48));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_PRIOR || wparam == VK_UP) {
                scrollY_ = std::max(0, scrollY_ -
                    (wparam == VK_PRIOR ? 480 : 48));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 680;
            info->ptMinTrackSize.y = 420;
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}
} // namespace ac
