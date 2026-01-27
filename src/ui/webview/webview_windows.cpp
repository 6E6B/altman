#define IDI_ICON_32 102

#include "paths.h"

#include <windows.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include <shlobj_core.h>
#include <wrl.h>
#include <wrl/client.h>

#include <WebView2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "components/data.h"
#include "webview.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

    constexpr wchar_t kClassName[] = L"AltmanWebView_Class";

    std::wstring GetUserDataFolder() {
        std::filesystem::path p = AltMan::Paths::WebViewProfiles();
        return p.wstring();
    }

    std::wstring Widen(const std::string &utf8) {
        if (utf8.empty()) {
            return {};
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), len);
        return result;
    }

    std::string Narrow(const std::wstring &wide) {
        if (wide.empty()) {
            return {};
        }
        int len
            = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string result(len, '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            result.data(),
            len,
            nullptr,
            nullptr
        );
        return result;
    }

    std::wstring SanitizeForPath(const std::wstring &input) {
        std::wstring sanitized;
        sanitized.reserve(input.size());
        for (wchar_t ch: input) {
            if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_') {
                sanitized.push_back(ch);
            } else {
                sanitized.push_back(L'_');
            }
        }
        return sanitized;
    }

    std::wstring ComputeUserDataPath(const std::wstring &userId, const std::wstring &cookie, bool isLoginFlow) {
        const std::wstring baseFolder = GetUserDataFolder();

        if (isLoginFlow) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            std::filesystem::path p = std::filesystem::path(baseFolder) / std::format(L"temp_login_{}", ms);
            std::filesystem::create_directories(p);
            return p.wstring();
        }

        if (!userId.empty()) {
            std::filesystem::path p = std::filesystem::path(baseFolder) / std::format(L"u_{}", SanitizeForPath(userId));
            std::filesystem::create_directories(p);
            return p.wstring();
        }

        if (!cookie.empty()) {
            size_t h = std::hash<std::wstring> {}(cookie);
            std::filesystem::path p = std::filesystem::path(baseFolder) / std::format(L"c_{:016X}", h);
            std::filesystem::create_directories(p);
            return p.wstring();
        }

        return baseFolder;
    }

    class WebViewWindow;

    std::mutex g_windowsMutex;
    std::unordered_map<std::wstring, WebViewWindow *> g_windowsByKey;

    class WebViewWindow {
        public:
            WebViewWindow(
                std::wstring url,
                std::wstring windowTitle,
                std::wstring cookie,
                std::wstring userId,
                std::wstring accountKey,
                std::function<void(const std::wstring &)> cookieCallback
            ) :
                initialUrl_(std::move(url)),
                windowTitle_(std::move(windowTitle)),
                cookieValue_(std::move(cookie)),
                accountKey_(std::move(accountKey)),
                cookieCallback_(std::move(cookieCallback)),
                isLoginFlow_(cookieCallback_ != nullptr),
                userDataFolder_(ComputeUserDataPath(userId, cookieValue_, isLoginFlow_)) {
            }

            ~WebViewWindow() {
                if (hwnd_) {
                    DestroyWindow(hwnd_);
                }
            }

            WebViewWindow(const WebViewWindow &) = delete;
            WebViewWindow &operator=(const WebViewWindow &) = delete;

            bool Create() {
                HINSTANCE hInstance = GetModuleHandleW(nullptr);
                RegisterWindowClass(hInstance);

                HDC hdc = GetDC(nullptr);
                int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
                ReleaseDC(nullptr, hdc);

                int width = MulDiv(1280, dpi, 96);
                int height = MulDiv(800, dpi, 96);

                hwnd_ = CreateWindowExW(
                    0,
                    kClassName,
                    windowTitle_.c_str(),
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    width,
                    height,
                    nullptr,
                    nullptr,
                    hInstance,
                    this
                );

                if (!hwnd_) {
                    return false;
                }

                ShowWindow(hwnd_, SW_SHOW);
                UpdateWindow(hwnd_);
                CreateWebView();

                return true;
            }

            void Show() {
                if (hwnd_) {
                    ShowWindow(hwnd_, SW_SHOW);
                    SetForegroundWindow(hwnd_);
                    SetFocus(hwnd_);
                }
            }

            void RunMessageLoop() {
                MSG msg;
                while (GetMessage(&msg, nullptr, 0, 0)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }

            [[nodiscard]] const std::wstring &AccountKey() const {
                return accountKey_;
            }

        private:
            static void RegisterWindowClass(HINSTANCE hInstance) {
                static std::once_flag flag;
                std::call_once(flag, [hInstance] {
                    WNDCLASSEXW wc {sizeof(wc)};
                    wc.style = CS_HREDRAW | CS_VREDRAW;
                    wc.hInstance = hInstance;
                    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    wc.lpszClassName = kClassName;
                    wc.lpfnWndProc = WndProc;
                    wc.cbWndExtra = sizeof(void *);
                    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));
                    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));
                    RegisterClassExW(&wc);
                });
            }

            void CreateWebView() {
                ComPtr<ICoreWebView2EnvironmentOptions> envOpts;

                CreateCoreWebView2EnvironmentWithOptions(
                    nullptr,
                    userDataFolder_.c_str(),
                    envOpts.Get(),
                    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                        [this](HRESULT hrEnv, ICoreWebView2Environment *env) -> HRESULT {
                            if (FAILED(hrEnv) || !env) {
                                return hrEnv;
                            }

                            env->CreateCoreWebView2Controller(
                                hwnd_,
                                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                    [this](HRESULT hrCtrl, ICoreWebView2Controller *ctrl) -> HRESULT {
                                        if (FAILED(hrCtrl) || !ctrl) {
                                            return hrCtrl;
                                        }

                                        controller_ = ctrl;
                                        controller_->get_CoreWebView2(&webview_);

                                        RECT rc {};
                                        GetClientRect(hwnd_, &rc);
                                        controller_->put_Bounds(rc);

                                        SetupNavigationHandler();
                                        InjectCookieAndNavigate();

                                        return S_OK;
                                    }
                                ).Get()
                            );
                            return S_OK;
                        }
                    ).Get()
                );
            }

            void SetupNavigationHandler() {
                if (!webview_ || !cookieCallback_) {
                    return;
                }

                webview_->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [this](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                            LPWSTR uri = nullptr;
                            if (SUCCEEDED(sender->get_Source(&uri)) && uri) {
                                std::wstring urlStr(uri);
                                CoTaskMemFree(uri);

                                if (urlStr.find(L"roblox.com") != std::wstring::npos) {
                                    ExtractAndNotifyCookie();
                                }
                            }
                            return S_OK;
                        }
                    ).Get(),
                    nullptr
                );
            }

            void ExtractAndNotifyCookie() {
                if (!webview_ || !cookieCallback_) {
                    return;
                }

                ComPtr<ICoreWebView2_2> webview2;
                if (FAILED(webview_.As(&webview2))) {
                    return;
                }

                ComPtr<ICoreWebView2CookieManager> mgr;
                if (FAILED(webview2->get_CookieManager(&mgr))) {
                    return;
                }

                mgr->GetCookies(
                    L"https://www.roblox.com",
                    Callback<ICoreWebView2GetCookiesCompletedHandler>(
                        [this](HRESULT hr, ICoreWebView2CookieList *cookieList) -> HRESULT {
                            if (FAILED(hr) || !cookieList) {
                                return hr;
                            }

                            UINT count = 0;
                            cookieList->get_Count(&count);

                            for (UINT i = 0; i < count; ++i) {
                                ComPtr<ICoreWebView2Cookie> cookie;
                                if (FAILED(cookieList->GetValueAtIndex(i, &cookie))) {
                                    continue;
                                }

                                LPWSTR name = nullptr;
                                if (SUCCEEDED(cookie->get_Name(&name)) && name) {
                                    std::wstring cookieName(name);
                                    CoTaskMemFree(name);

                                    if (cookieName == L".ROBLOSECURITY") {
                                        LPWSTR value = nullptr;
                                        if (SUCCEEDED(cookie->get_Value(&value)) && value) {
                                            std::wstring cookieValue(value);
                                            CoTaskMemFree(value);

                                            if (cookieCallback_ && !cookieValue.empty()) {
                                                cookieCallback_(cookieValue);
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                            return S_OK;
                        }
                    ).Get()
                );
            }

            void InjectCookieAndNavigate() {
                if (cookieValue_.empty()) {
                    PreWarmNetwork();
                    webview_->Navigate(initialUrl_.c_str());
                    return;
                }

                ComPtr<ICoreWebView2_2> webview2;
                if (FAILED(webview_.As(&webview2))) {
                    webview_->Navigate(initialUrl_.c_str());
                    return;
                }

                ComPtr<ICoreWebView2CookieManager> mgr;
                if (FAILED(webview2->get_CookieManager(&mgr))) {
                    webview_->Navigate(initialUrl_.c_str());
                    return;
                }

                ComPtr<ICoreWebView2Cookie> cookie;
                if (FAILED(mgr->CreateCookie(L".ROBLOSECURITY", cookieValue_.c_str(), L".roblox.com", L"/", &cookie))) {
                    webview_->Navigate(initialUrl_.c_str());
                    return;
                }

                cookie->put_IsSecure(TRUE);
                cookie->put_IsHttpOnly(TRUE);
                cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);

                auto expires
                    = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch() + std::chrono::hours(24 * 365 * 10)
                    )
                          .count();
                cookie->put_Expires(static_cast<double>(expires));

                mgr->AddOrUpdateCookie(cookie.Get());

                PreWarmNetwork();
                webview_->Navigate(initialUrl_.c_str());
            }

            void PreWarmNetwork() {
                if (webview_) {
                    webview_->ExecuteScript(L"fetch('https://www.roblox.com/favicon.ico').catch(()=>{});", nullptr);
                }
            }

            void Resize() {
                if (!controller_) {
                    return;
                }

                RECT rc {};
                GetClientRect(hwnd_, &rc);
                controller_->put_Bounds(rc);

                ComPtr<ICoreWebView2Controller3> ctl3;
                if (SUCCEEDED(controller_.As(&ctl3))) {
                    UINT dpi = GetDpiForWindow(hwnd_);
                    ctl3->put_RasterizationScale(static_cast<double>(dpi) / 96.0);
                }
            }

            void OnClose() {
                std::lock_guard lock(g_windowsMutex);
                g_windowsByKey.erase(accountKey_);

                if (isLoginFlow_ && !userDataFolder_.empty()) {
                    std::error_code ec;
                    std::filesystem::remove_all(userDataFolder_, ec);
                }
            }

            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                WebViewWindow *self = nullptr;

                if (msg == WM_NCCREATE) {
                    auto *cs = reinterpret_cast<LPCREATESTRUCT>(lParam);
                    self = static_cast<WebViewWindow *>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                } else {
                    self = reinterpret_cast<WebViewWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                }

                if (!self) {
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }

                switch (msg) {
                    case WM_SIZE:
                        self->Resize();
                        return 0;

                    case WM_DPICHANGED: {
                        auto *rect = reinterpret_cast<RECT *>(lParam);
                        SetWindowPos(
                            hwnd,
                            nullptr,
                            rect->left,
                            rect->top,
                            rect->right - rect->left,
                            rect->bottom - rect->top,
                            SWP_NOZORDER | SWP_NOACTIVATE
                        );
                        self->Resize();
                        return 0;
                    }

                    case WM_CLOSE:
                        self->OnClose();
                        DestroyWindow(hwnd);
                        return 0;

                    case WM_DESTROY:
                        PostQuitMessage(0);
                        return 0;

                    default:
                        return DefWindowProcW(hwnd, msg, wParam, lParam);
                }
            }

            HWND hwnd_ = nullptr;
            ComPtr<ICoreWebView2> webview_;
            ComPtr<ICoreWebView2Controller> controller_;
            std::wstring initialUrl_;
            std::wstring windowTitle_;
            std::wstring cookieValue_;
            std::wstring userDataFolder_;
            std::wstring accountKey_;
            std::function<void(const std::wstring &)> cookieCallback_;
            bool isLoginFlow_ = false;
    };

} // anonymous namespace

void LaunchWebviewImpl(
    const std::string &url,
    const std::string &windowName,
    const std::string &cookie,
    const std::string &userId,
    CookieCallback onCookieExtracted
) {
    std::wstring wUrl = Widen(url);
    std::wstring wTitle = Widen(windowName);
    std::wstring wCookie = Widen(cookie);
    std::wstring wUserId = Widen(userId);

    std::wstring accountKey;
    if (onCookieExtracted) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        accountKey = std::format(L"login_{}", ms);
    } else if (!wUserId.empty()) {
        accountKey = wUserId;
    } else if (!wCookie.empty()) {
        size_t h = std::hash<std::wstring> {}(wCookie);
        accountKey = std::format(L"cookie_{:016X}", h);
    } else {
        accountKey = wUrl;
    }

    std::function<void(const std::wstring &)> wideCallback;
    if (onCookieExtracted) {
        wideCallback = [onCookieExtracted](const std::wstring &wideCookie) {
            onCookieExtracted(Narrow(wideCookie));
        };
    }

    std::thread([wUrl, wTitle, wCookie, wUserId, accountKey, wideCallback]() mutable {
        {
            std::lock_guard lock(g_windowsMutex);
            auto it = g_windowsByKey.find(accountKey);
            if (it != g_windowsByKey.end() && !wideCallback) {
                it->second->Show();
                return;
            }
        }

        auto window = std::make_unique<WebViewWindow>(wUrl, wTitle, wCookie, wUserId, accountKey, wideCallback);

        if (!window->Create()) {
            return;
        }

        {
            std::lock_guard lock(g_windowsMutex);
            g_windowsByKey[accountKey] = window.get();
        }

        window->RunMessageLoop();

        {
            std::lock_guard lock(g_windowsMutex);
            g_windowsByKey.erase(accountKey);
        }
    }).detach();
}

void LaunchWebview(const std::string &url, const AccountData &account) {
    std::string title = !account.displayName.empty() ? account.displayName
                        : account.userId.empty()     ? account.username
                                                     : std::format("{} - {}", account.username, account.userId);

    LaunchWebviewImpl(url, title, account.cookie, account.userId, nullptr);
}

void LaunchWebview(const std::string &url, const AccountData &account, const std::string &windowName) {
    LaunchWebviewImpl(url, windowName.empty() ? account.username : windowName, account.cookie, account.userId, nullptr);
}

void LaunchWebviewForLogin(const std::string &url, const std::string &windowName, CookieCallback onCookieExtracted) {
    LaunchWebviewImpl(url, windowName, "", "", onCookieExtracted);
}
