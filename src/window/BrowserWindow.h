#pragma once
#include "tech/IdQueue.h"
#include "html/HtmlParser.h"
#include "net/HttpClient.h"
#include "style/Style.h"
#include "layout/Layout.h"
#include "image/ImageStore.h"
#include "window/Chrome.h"
#include <unordered_set>
#include <windows.h>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ac {

class BrowserWindow final {
public:
    static constexpr WORD kBack = 101;
    static constexpr WORD kForward = 102;
    static constexpr WORD kReload = 103;
    static constexpr WORD kGo = 104;
    static constexpr WORD kFocus = 105;
    static constexpr WORD kNewTab = 106;
    static constexpr WORD kCloseTab = 107;
    static constexpr WORD kBookmark = 108;
    static constexpr WORD kHome = 109;
    static constexpr WORD kSelectTabBase = chrome::kTabCommandBase;
    static HWND Create(HINSTANCE instance, std::wstring initialUrl = {});

private:
    static constexpr UINT kPageReady = WM_APP + 7;
    static constexpr UINT kImageReady = WM_APP + 8;
    static constexpr UINT kStyleReady = WM_APP + 9;
    static constexpr int kToolbarHeight = chrome::kToolbarHeight;
    static constexpr int kStatusHeight = chrome::kStatusHeight;

    struct NavigationJob {
        idQueueNode<NavigationJob> queueNode;
        unsigned generation;
        std::wstring url;
        NavigationJob(unsigned id, std::wstring address)
            : generation(id), url(std::move(address)) {}
    };
    struct LoadedPage {
        unsigned generation = 0;
        net::Response response;
        html::Document document;
    };

    struct LoadedImage {
        unsigned generation = 0;
        std::wstring url;
        std::string bytes;
    };
    struct LoadedStyle {
        unsigned generation = 0;
        std::string css;
    };

    HWND hwnd_ = nullptr;
    HWND back_ = nullptr;
    HWND forward_ = nullptr;
    HWND reload_ = nullptr;
    HWND address_ = nullptr;
    HWND go_ = nullptr;
    HWND status_ = nullptr;
    HFONT chromeFont_ = nullptr;
    HFONT statusFont_ = nullptr;
    HBRUSH addressBrush_ = nullptr;
    HBRUSH statusBrush_ = nullptr;

    struct TabState {
        std::wstring url;
        std::wstring title = L"Nueva pestaña";
        std::vector<std::wstring> history;
        size_t historyIndex = 0;
        int scroll = 0;
    };
    std::vector<TabState> tabs_;
    size_t activeTab_ = 0;
    std::unordered_set<std::wstring> bookmarks_;

    std::wstring currentUrl_;
    std::wstring initialUrl_;
    std::unique_ptr<html::Document> document_;
    style::Sheet sheet_;
    layout::PageLayout layout_;
    image::Store images_;
    int scrollY_ = 0;
    int wheelRemainder_ = 0;
    int clientWidth_ = 980;
    int clientHeight_ = 690;
    unsigned generation_ = 0;
    std::atomic<unsigned> newestRequest_{0};
    bool isLoading_ = false;

    std::vector<std::wstring> history_;
    size_t historyIndex_ = 0;

    std::thread worker_;
    std::mutex taskMutex_;
    std::condition_variable taskCondition_;
    idQueue<NavigationJob, &NavigationJob::queueNode> pendingJobs_;
    bool stopping_ = false;

    BrowserWindow() = default;
    ~BrowserWindow();

    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT message,
                                       WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK AddressSubclass(HWND hwnd, UINT message,
                WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR context);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void OnCreate();
    void OnDestroy();
    void Resize(int width, int height);
    void Reflow();
    void Paint();
    void DrawClient(HDC dc);
    void DrawContent(HDC dc);
    void InvalidateContent();
    void ShowHome();
    void SaveActiveTab();
    void SwitchTab(size_t index);
    void OpenTab();
    void CloseTab(size_t index);
    void ToggleBookmark();
    void OpenMenu();
    void OpenTabMenu();
    void MaximizeRestore();
    void Navigate(std::wstring address, bool recordHistory);
    void GoBack();
    void GoForward();
    void Reload();
    void UpdateButtons();
    void SetStatus(const std::wstring& message);
    void WorkerLoop();
    void StopWorker();
    void OnPageReady(LoadedPage* result);
    void OnImageReady(LoadedImage* image);
    void OnStyleReady(LoadedStyle* style);
    [[nodiscard]] std::wstring AddressText() const;
    [[nodiscard]] int MaxScroll() const;
    [[nodiscard]] RECT ContentRect() const;
};
}
