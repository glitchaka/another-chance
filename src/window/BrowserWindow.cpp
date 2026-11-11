#include "window/BrowserWindow.h"
#include "util/Url.h"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <unordered_set>
#include <cstring>

namespace ac {
namespace {
constexpr wchar_t kClassName[] = L"AnotherChance.NativeBrowser";
void TraceBoot(const char* message) {
    wchar_t path[2048]{};
    if (GetEnvironmentVariableW(L"ANOTHER_CHANCE_TRACE_PATH", path, 2048) == 0) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

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
    if (chromeFont_) DeleteObject(chromeFont_);
    if (statusFont_) DeleteObject(statusFont_);
    if (addressBrush_) DeleteObject(addressBrush_);
    if (statusBrush_) DeleteObject(statusBrush_);
}

HWND BrowserWindow::Create(HINSTANCE instance, std::wstring initialUrl) {
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
    window->initialUrl_ = std::move(initialUrl);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Another Chance",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
        WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
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
        // Default processing initializes the non-client window state and caption.
        return DefWindowProcW(hwnd, message, wparam, lparam);
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

LRESULT CALLBACK BrowserWindow::AddressSubclass(HWND hwnd, UINT message,
        WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR context) {
    (void)id;
    auto* instance = reinterpret_cast<BrowserWindow*>(context);
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        PostMessageW(instance->hwnd_, WM_COMMAND, MAKEWPARAM(kGo, BN_CLICKED), 0);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        SetWindowTextW(hwnd, instance->currentUrl_.c_str());
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void BrowserWindow::OnCreate() {
    TraceBoot("OnCreate: begin");
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    chromeFont_ = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    statusFont_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    addressBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    statusBrush_ = CreateSolidBrush(RGB(246, 248, 252));
    address_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 180, 28, hwnd_,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kGo + 100)),
        instance, nullptr);
    if (chromeFont_) SendMessageW(address_, WM_SETFONT,
        reinterpret_cast<WPARAM>(chromeFont_), TRUE);
    status_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 200, 20, hwnd_, nullptr, instance, nullptr);
    if (statusFont_) SendMessageW(status_, WM_SETFONT,
        reinterpret_cast<WPARAM>(statusFont_), TRUE);

    tabs_.emplace_back();
    if (!SetWindowSubclass(address_, &BrowserWindow::AddressSubclass,
                           1, reinterpret_cast<DWORD_PTR>(this))) {
        TraceBoot("OnCreate: address bar subclass failed");
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    Resize(client.right, client.bottom);
    TraceBoot("OnCreate: resized controls");

    worker_ = std::thread(&BrowserWindow::WorkerLoop, this);
    TraceBoot("OnCreate: worker started");
    ShowHome();
    if (!initialUrl_.empty()) {
        Navigate(initialUrl_, true);
    }
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

void BrowserWindow::ShowHome() {
    ++generation_;
    newestRequest_.store(generation_);
    {
        std::lock_guard guard(taskMutex_);
        while (!pendingJobs_.IsEmpty()) delete pendingJobs_.RemoveFirst();
    }
    currentUrl_.clear();
    isLoading_ = false;
    history_.clear();
    historyIndex_ = 0;
    scrollY_ = 0;
    images_.Clear();
    document_ = std::make_unique<html::Document>(html::Parse(R"HTML(
        <html><head><title>Otra oportunidad</title>
        <style>
            h1 { color: #263d60; font-size: 32px; }
            p { color: #586d8a; font-size: 17px; }
            a { color: #2979d4; }
        </style></head>
        <body>
          <h1>Another Chance</h1>
          <p>Una nueva forma de explorar. Navegador nativo con motor propio.</p>
          <p>Escribe una dirección o búsqueda en la barra superior.</p>
          <p><a href="https://example.org/">Visitar la página de ejemplo</a></p>
          <p>Versión experimental: algunos sitios requieren tecnologías
             que todavía no están implementadas.</p>
        </body></html>
    )HTML"));
    sheet_.Parse(document_->embeddedCss);
    tabs_[activeTab_].title = L"Nueva pestaña";
    tabs_[activeTab_].url.clear();
    tabs_[activeTab_].history.clear();
    tabs_[activeTab_].historyIndex = 0;
    tabs_[activeTab_].scroll = 0;
    SetWindowTextW(address_, L"");
    SetWindowTextW(hwnd_, L"Another Chance");
    Reflow();
    SetStatus(L"Listo · HTML/CSS propio · Sin Chromium ni Firefox");
    UpdateButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::SaveActiveTab() {
    if (activeTab_ >= tabs_.size()) return;
    auto& tab = tabs_[activeTab_];
    tab.url = currentUrl_;
    tab.history = history_;
    tab.historyIndex = historyIndex_;
    tab.scroll = scrollY_;
}

void BrowserWindow::SwitchTab(size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    SaveActiveTab();
    activeTab_ = index;
    const TabState tab = tabs_[activeTab_];
    if (tab.url.empty()) {
        ShowHome();
    } else {
        history_ = tab.history;
        historyIndex_ = tab.historyIndex;
        Navigate(tab.url, false);
    }
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::OpenTab() {
    if (tabs_.size() >= 5) {
        SetStatus(L"Esta versión admite hasta cinco pestañas.");
        return;
    }
    SaveActiveTab();
    tabs_.emplace_back();
    activeTab_ = tabs_.size() - 1;
    ShowHome();
    SetFocus(address_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::CloseTab(size_t index) {
    if (index >= tabs_.size()) return;
    if (tabs_.size() == 1) {
        ShowHome();
        return;
    }
    SaveActiveTab();
    const bool wasCurrent = index == activeTab_;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < activeTab_) --activeTab_;
    if (wasCurrent) {
        activeTab_ = std::min(index, tabs_.size() - 1);
        const TabState tab = tabs_[activeTab_];
        if (tab.url.empty()) ShowHome();
        else {
            history_ = tab.history;
            historyIndex_ = tab.historyIndex;
            Navigate(tab.url, false);
        }
    }
    Resize(clientWidth_, clientHeight_);
}

void BrowserWindow::ToggleBookmark() {
    if (currentUrl_.empty()) return;
    if (bookmarks_.contains(currentUrl_)) {
        bookmarks_.erase(currentUrl_);
        SetStatus(L"Marcador eliminado");
    } else {
        bookmarks_.insert(currentUrl_);
        SetStatus(L"Página añadida a marcadores de la sesión");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::OpenMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kNewTab, L"Nueva pestaña\tCtrl+T");
    AppendMenuW(menu, MF_STRING, kCloseTab, L"Cerrar pestaña\tCtrl+W");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kHome, L"Página de inicio");
    AppendMenuW(menu, MF_STRING, kReload, L"Recargar\tF5");
    AppendMenuW(menu, MF_STRING, kBookmark, L"Guardar marcador");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (searchProvider_ == url::SearchProvider::Google ?
                    MF_CHECKED : 0), kSearchGoogle, L"Buscador predeterminado: Google");
    AppendMenuW(menu, MF_STRING | (searchProvider_ == url::SearchProvider::Bing ?
                    MF_CHECKED : 0), kSearchBing, L"Buscador: Bing");
    AppendMenuW(menu, MF_STRING | (searchProvider_ == url::SearchProvider::Disabled ?
                    MF_CHECKED : 0), kSearchDisabled, L"Desactivar búsqueda");
    POINT point{clientWidth_ - 171, kToolbarHeight - 2};
    ClientToScreen(hwnd_, &point);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                   point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void BrowserWindow::OpenTabMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kNewTab, L"+ Nueva vista\tCtrl+T");
    AppendMenuW(menu, MF_STRING, kCloseTab, L"Cerrar vista actual\tCtrl+W");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const std::wstring caption = std::to_wstring(i + 1) + L"  " +
            (tabs_[i].title.empty() ? L"Nueva vista" : tabs_[i].title);
        AppendMenuW(menu, MF_STRING | (i == activeTab_ ? MF_CHECKED : 0),
                    static_cast<UINT>(kSelectTabBase + i), caption.c_str());
    }
    POINT pt{clientWidth_ - 269, kToolbarHeight - 1};
    ClientToScreen(hwnd_, &pt);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void BrowserWindow::MaximizeRestore() {
    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BrowserWindow::SetStatus(const std::wstring& message) {
    if (status_) SetWindowTextW(status_, message.c_str());
}

void BrowserWindow::Navigate(std::wstring address, bool recordHistory) {
    const std::wstring typed = address;
    address = url::ResolveInput(std::move(address), searchProvider_);
    if (address.empty()) {
        SetStatus(L"No se reconoce la dirección. Introduce una URL o selecciona un buscador.");
        return;
    }
    // Show where a search will be sent; the rendering engine is still our own.
    if (recordHistory && url::Normalize(typed).empty()) {
        SetStatus(L"Búsqueda mediante el proveedor seleccionado...");
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
    tabs_[activeTab_].url = address;
    tabs_[activeTab_].title = L"Cargando…";
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
    if (hwnd_) {
        RECT area{0, 0, clientWidth_, kToolbarHeight};
        InvalidateRect(hwnd_, &area, FALSE);
    }
}

std::wstring BrowserWindow::AddressText() const {
    const HWND edit = GetDlgItem(hwnd_, kGo + 100);
    if (!edit || !IsWindow(edit)) return {};
    std::wstring result(16384, L'\0');
    const int written = GetWindowTextW(edit, result.data(),
                                       static_cast<int>(result.size()));
    result.resize(static_cast<size_t>(std::max(0, written)));
    const std::string diagnostic = "AddressText direct: length=" +
        std::to_string(written) + " value=" + url::ToUtf8(result);
    TraceBoot(diagnostic.c_str());
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
    const auto geometry = chrome::Measure(clientWidth_);
    if (address_) {
        MoveWindow(address_, geometry.address.left + 11, 14,
                   std::max(70, static_cast<int>(geometry.address.right -
                                                geometry.address.left - 22)),
                   27, TRUE);
    }
    if (status_) {
        MoveWindow(status_, 15, std::max(kToolbarHeight, clientHeight_ - kStatusHeight + 5),
                   std::max(100, clientWidth_ - 30), 18, TRUE);
    }
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
    tabs_[activeTab_].title = title.empty() ? L"Página sin título" : title;
    tabs_[activeTab_].url = currentUrl_;
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

void BrowserWindow::DrawContent(HDC dc) {
    const RECT view = ContentRect();
    HBRUSH paper = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &view, paper);
    DeleteObject(paper);
    if (document_) {
        layout_.Draw(dc, view.top, scrollY_, view);
    }
}

void BrowserWindow::InvalidateContent() {
    if (!hwnd_) return;
    const RECT viewport = ContentRect();
    InvalidateRect(hwnd_, &viewport, FALSE);
}

void BrowserWindow::DrawClient(HDC dc) {
    RECT client{0, 0, clientWidth_, clientHeight_};
    HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, background);
    DeleteObject(background);
    DrawContent(dc);
    std::vector<chrome::Tab> tabLabels;
    tabLabels.reserve(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        tabLabels.push_back({tabs_[i].title, i == activeTab_ && isLoading_});
    }
    chrome::Draw(dc, clientWidth_, clientHeight_, tabLabels, activeTab_,
                 !history_.empty() && historyIndex_ > 0,
                 !history_.empty() && historyIndex_ + 1 < history_.size(),
                 bookmarks_.contains(currentUrl_), isLoading_,
                 IsZoomed(hwnd_) != FALSE);
}

void BrowserWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd_, &paint);
    if (!target) {
        EndPaint(hwnd_, &paint);
        return;
    }
    const RECT dirty = paint.rcPaint;
    const int width = dirty.right - dirty.left;
    const int height = dirty.bottom - dirty.top;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd_, &paint);
        return;
    }

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        // Render in window coordinates but allocate only the dirty rectangle.
        // Repaint the document independently of the fixed application header.
        SetViewportOrgEx(buffer, -dirty.left, -dirty.top, nullptr);
        const RECT view = ContentRect();
        const bool contentOnly = dirty.top >= view.top &&
                                 dirty.bottom <= view.bottom;
        if (contentOnly) {
            DrawContent(buffer);
        } else {
            ++chromePaintCount_;
            DrawClient(buffer);
        }
        SetViewportOrgEx(buffer, 0, 0, nullptr);
        BitBlt(target, dirty.left, dirty.top, width, height,
               buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
    } else {
        // Graceful fallback if the offscreen surface could not be allocated.
        const RECT view = ContentRect();
        if (dirty.top >= view.top && dirty.bottom <= view.bottom) {
            DrawContent(target);
        } else {
            ++chromePaintCount_;
            DrawClient(target);
        }
    }
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
    EndPaint(hwnd_, &paint);
}


LRESULT BrowserWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCCALCSIZE:
            // Client geometry always occupies the entire window rectangle,
            // including the initial (wParam==FALSE) calculation.
            return 0;
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCHITTEST: {
            const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT bounds{};
            GetWindowRect(hwnd_, &bounds);
            const int x = screen.x - bounds.left;
            const int y = screen.y - bounds.top;
            if (!IsZoomed(hwnd_)) {
                constexpr int edge = 8;
                const bool left = x < edge;
                const bool right = x >= bounds.right - bounds.left - edge;
                const bool top = y < edge;
                const bool bottom = y >= bounds.bottom - bounds.top - edge;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            if (chrome::IsDraggable(x, y, clientWidth_)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wparam == HTCAPTION) {
                MaximizeRestore();
                return 0;
            }
            break;
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_SIZE:
            Resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        // Internal-only diagnostics used by our Windows UI regression test.
        // No user input or navigation data is exposed.
        case WM_APP + 40:
            return static_cast<LRESULT>(chromePaintCount_);
        case WM_APP + 41:
            return static_cast<LRESULT>(scrollY_);
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_PRINTCLIENT:
            DrawClient(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(46, 59, 77));
            return reinterpret_cast<LRESULT>(addressBrush_);
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(246, 248, 252));
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(98, 111, 130));
            return reinterpret_cast<LRESULT>(statusBrush_);
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
                case kNewTab: OpenTab(); return 0;
                case kCloseTab: CloseTab(activeTab_); return 0;
                case kBookmark: ToggleBookmark(); return 0;
                case kHome: ShowHome(); return 0;
                case kSearchGoogle:
                    searchProvider_ = url::SearchProvider::Google;
                    SetStatus(L"Buscador: Google");
                    return 0;
                case kSearchBing:
                    searchProvider_ = url::SearchProvider::Bing;
                    SetStatus(L"Buscador: Bing");
                    return 0;
                case kSearchDisabled:
                    searchProvider_ = url::SearchProvider::Disabled;
                    SetStatus(L"Búsqueda desactivada. Solo se aceptarán URLs.");
                    return 0;
                case kFocus:
                    SetFocus(address_);
                    SendMessageW(address_, EM_SETSEL, 0, -1);
                    return 0;
                default:
                    if (LOWORD(wparam) >= kSelectTabBase &&
                        LOWORD(wparam) < kSelectTabBase + 5) {
                        SwitchTab(LOWORD(wparam) - kSelectTabBase);
                        return 0;
                    }
                    break;
            }
            break;
        case WM_MOUSEWHEEL: {
            // Preserve fractional wheel deltas from high-resolution devices.
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
            const int ticks = wheelRemainder_ / WHEEL_DELTA;
            wheelRemainder_ %= WHEEL_DELTA;
            if (ticks != 0) {
                const int next = std::clamp(scrollY_ - ticks * 96, 0, MaxScroll());
                if (next != scrollY_) {
                    scrollY_ = next;
                    InvalidateContent();
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (y < kToolbarHeight) {
                const auto hit = chrome::HitTest(x, y, clientWidth_);
                switch (hit.action) {
                    case chrome::Action::Back: GoBack(); break;
                    case chrome::Action::Forward: GoForward(); break;
                    case chrome::Action::Reload: Reload(); break;
                    case chrome::Action::Home: ShowHome(); break;
                    case chrome::Action::Bookmark: ToggleBookmark(); break;
                    case chrome::Action::Menu: OpenMenu(); break;
                    case chrome::Action::TabMenu: OpenTabMenu(); break;
                    case chrome::Action::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
                    case chrome::Action::Maximize: MaximizeRestore(); break;
                    case chrome::Action::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
                    case chrome::Action::None: break;
                }
                return 0;
            }
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
                if (point.y < kToolbarHeight && chrome::HitTest(
                        point.x, point.y, clientWidth_).action !=
                        chrome::Action::None) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
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
                InvalidateContent();
                return 0;
            }
            if (wparam == VK_PRIOR || wparam == VK_UP) {
                scrollY_ = std::max(0, scrollY_ -
                    (wparam == VK_PRIOR ? 480 : 48));
                InvalidateContent();
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (GetMonitorInfoW(monitor, &monitorInfo)) {
                const RECT m = monitorInfo.rcMonitor;
                const RECT work = monitorInfo.rcWork;
                info->ptMaxPosition.x = work.left - m.left;
                info->ptMaxPosition.y = work.top - m.top;
                info->ptMaxSize.x = work.right - work.left;
                info->ptMaxSize.y = work.bottom - work.top;
            }
            info->ptMinTrackSize.x = 860;
            info->ptMinTrackSize.y = 420;
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}
} // namespace ac
