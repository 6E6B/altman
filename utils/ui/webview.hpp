#define IDI_ICON_32 102
#include <windows.h>
#include <shlobj_core.h>
#include <shellscalingapi.h>  // Add this for DPI awareness
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static inline std::atomic<int> s_windowCount_;
static inline constexpr wchar_t kClassName_[] = L"WebViewModule_Class";

class WebViewWindow {
    HWND hwnd_ = nullptr;
    ComPtr<ICoreWebView2> webview_;
    ComPtr<ICoreWebView2Controller> controller_;

    std::wstring initialUrl_;
    std::wstring windowTitle_;
    std::wstring cookieValue_;

public:
    WebViewWindow(std::wstring url,
                  std::wstring windowTitle,
                  std::wstring cookie = L"")
        : initialUrl_(std::move(url)),
          windowTitle_(std::move(windowTitle)),
          cookieValue_(std::move(cookie)) {
    }

    ~WebViewWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool create() {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);

        // Enable DPI awareness
        static std::once_flag dpiFlag;
        std::call_once(dpiFlag, [] {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        });

        static std::once_flag flag;
        std::call_once(flag, [hInstance] {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.hInstance = hInstance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName_;
            wc.lpfnWndProc = wndProc;
            wc.cbWndExtra = sizeof(void *);

            wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));
            wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));

            RegisterClassExW(&wc);
        });

        // Get DPI for proper initial sizing
        HDC hdc = GetDC(nullptr);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);

        // Scale initial window size based on DPI
        int scaledWidth = MulDiv(1280, dpi, 96);
        int scaledHeight = MulDiv(800, dpi, 96);

        hwnd_ = CreateWindowExW(
            0, kClassName_, windowTitle_.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, scaledWidth, scaledHeight,
            nullptr, nullptr, hInstance, this);

        if (!hwnd_) return false;

        ++s_windowCount_;
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        createWebView();
        return true;
    }

    void messageLoop() {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

private:
    void createWebView() {
        wchar_t appDataPath[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
            appDataPath))) {
            return;
        }

        std::filesystem::path profile =
                std::filesystem::path(appDataPath) / L"WebViewProfiles";
        profile /= std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(s_windowCount_.load());

        std::filesystem::create_directories(profile);

        ComPtr<ICoreWebView2EnvironmentOptions> envOpts;
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), envOpts.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT hrEnv, ICoreWebView2Environment *env) -> HRESULT {
                    if (FAILED(hrEnv)) return hrEnv;

                    env->CreateCoreWebView2Controller(
                        hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT hrCtrl,
                                   ICoreWebView2Controller *controller) -> HRESULT {
                                if (FAILED(hrCtrl)) return hrCtrl;

                                controller_ = controller;
                                controller_->get_CoreWebView2(&webview_);

                                // Fit WebView to client rect
                                RECT rc{};
                                GetClientRect(hwnd_, &rc);
                                controller_->put_Bounds(rc);

                                injectCookie(); // no-op if none
                                webview_->Navigate(initialUrl_.c_str());
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
    }

    void injectCookie() {
        if (cookieValue_.empty()) return; // optional!

        ComPtr<ICoreWebView2_2> webview2_2;
        if (FAILED(webview_.As(&webview2_2))) return;

        ComPtr<ICoreWebView2CookieManager> mgr;
        if (FAILED(webview2_2->get_CookieManager(&mgr))) return;

        ComPtr<ICoreWebView2Cookie> cookie;
        if (FAILED(mgr->CreateCookie(L".ROBLOSECURITY",
            cookieValue_.c_str(),
            L".roblox.com", L"/", &cookie)))
            return;

        cookie->put_IsSecure(TRUE);
        cookie->put_IsHttpOnly(TRUE);
        cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);

        using namespace std::chrono;
        double expires = duration_cast<seconds>(
            system_clock::now().time_since_epoch() + hours(24 * 365 * 10)).count();
        cookie->put_Expires(expires);

        mgr->AddOrUpdateCookie(cookie.Get());
    }

    void resize() {
        if (!controller_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        controller_->put_Bounds(rc);

        // Optional: Ensure proper DPI scaling for WebView2
        ComPtr<ICoreWebView2Controller3> controller3;
        if (SUCCEEDED(controller_.As(&controller3))) {
            UINT dpi = GetDpiForWindow(hwnd_);
            double scale = static_cast<double>(dpi) / 96.0;
            controller3->put_RasterizationScale(scale);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        LONG_PTR raw =
                (m == WM_NCCREATE)
                    ? reinterpret_cast<LONG_PTR>(
                        reinterpret_cast<LPCREATESTRUCT>(l)->lpCreateParams)
                    : GetWindowLongPtrW(h, GWLP_USERDATA);

        auto *self = reinterpret_cast<WebViewWindow *>(raw);

        if (m == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              (LONG_PTR) ((LPCREATESTRUCT) l)->lpCreateParams);

        if (!self) return DefWindowProcW(h, m, w, l);

        switch (m) {
            case WM_SIZE:
                self->resize();
                return 0;

            case WM_DPICHANGED: {
                // Handle DPI changes when moving between monitors
                RECT *newRect = reinterpret_cast<RECT *>(l);
                SetWindowPos(h, nullptr,
                             newRect->left, newRect->top,
                             newRect->right - newRect->left,
                             newRect->bottom - newRect->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }

            case WM_DESTROY:
                --s_windowCount_;
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(h, m, w, l);
        }
    }
};

namespace {
    std::wstring widen(const std::string &utf8) {
        if (utf8.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                            static_cast<int>(utf8.size()), w.data(), len);
        return w;
    }
}

inline void LaunchWebview(const std::wstring &url,
                          const std::wstring &windowName = L"Altman Webview",
                          const std::wstring &cookie = L"") {
    std::thread([url, windowName, cookie] {
        auto win = std::make_unique<WebViewWindow>(url, windowName, cookie);
        if (win->create()) win->messageLoop();
    }).detach();
}

// UTF‑8 convenience overloads
inline void LaunchWebview(const std::string &url,
                          const std::string &windowName = "Altman Webview",
                          const std::string &cookie = "") {
    LaunchWebview(widen(url), widen(windowName), widen(cookie));
}
