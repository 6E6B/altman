#define IDI_ICON_32 102

#include "webview.h"
#include <WebView2.h>
#include <shellscalingapi.h>
#include <shlobj_core.h>
#include <windows.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cwchar>
#include <memory>
#include <mutex>
#include <thread>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static inline const std::wstring kUserDataFolder = [] {
	wchar_t appDataPath[MAX_PATH]{};
	SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appDataPath);
	std::filesystem::path p = std::filesystem::path(appDataPath) / L"WebViewProfiles" / L"Roblox";
	std::filesystem::create_directories(p);
	return p.wstring();
}();

static inline std::atomic<int> s_windowCount_{0};
static inline constexpr wchar_t kClassName_[] = L"WebViewModule_Class";

class WebViewWindow {
	HWND hwnd_ = nullptr;
	ComPtr<ICoreWebView2> webview_;
	ComPtr<ICoreWebView2Controller> controller_;

	std::wstring initialUrl_;
	std::wstring windowTitle_;
	std::wstring cookieValue_;
	std::wstring userDataFolder_;

public:
	WebViewWindow(std::wstring url,
				  std::wstring windowTitle,
				  std::wstring cookie = L"",
				  std::wstring userId = L"")
		: initialUrl_(std::move(url)),
		  windowTitle_(std::move(windowTitle)),
		  cookieValue_(std::move(cookie)) {
		// Derive per-user data folder using userId when provided; otherwise fall back to app-wide folder.
		if (!userId.empty()) {
			std::wstring sanitized;
			sanitized.reserve(userId.size());
			for (wchar_t ch : userId) {
				if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_')
					sanitized.push_back(ch);
				else
					sanitized.push_back(L'_');
			}
			std::filesystem::path p = std::filesystem::path(kUserDataFolder) / (L"u_" + sanitized);
			std::filesystem::create_directories(p);
			userDataFolder_ = p.wstring();
		} else {
			if (!cookieValue_.empty()) {
				// Stable per-cookie folder using a hash
				size_t h = std::hash<std::wstring>{}(cookieValue_);
				wchar_t hashHex[17]{};
				swprintf(hashHex, 17, L"%016llX", static_cast<unsigned long long>(h));
				std::filesystem::path p = std::filesystem::path(kUserDataFolder) / (L"c_" + std::wstring(hashHex));
				std::filesystem::create_directories(p);
				userDataFolder_ = p.wstring();
			} else {
				userDataFolder_ = kUserDataFolder;
			}
		}
	}

	~WebViewWindow() {
		if (hwnd_) DestroyWindow(hwnd_);
	}

	bool create() {
		HINSTANCE hInstance = GetModuleHandleW(nullptr);

		// Register window class once.
		static std::once_flag clsFlag;
		std::call_once(clsFlag, [hInstance] {
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

		// DPI scaled initial size.
		HDC hdc = GetDC(nullptr);
		int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
		ReleaseDC(nullptr, hdc);
		int w = MulDiv(1280, dpi, 96);
		int h = MulDiv(800, dpi, 96);

		hwnd_ = CreateWindowExW(0, kClassName_, windowTitle_.c_str(), WS_OVERLAPPEDWINDOW,
		                        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
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
		ComPtr<ICoreWebView2EnvironmentOptions> envOpts;

		CreateCoreWebView2EnvironmentWithOptions(
			nullptr /* Edge runtime */, userDataFolder_.c_str(), envOpts.Get(),
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[this](HRESULT hrEnv, ICoreWebView2Environment *env) -> HRESULT {
					if (FAILED(hrEnv)) return hrEnv;

					env->CreateCoreWebView2Controller(
						hwnd_,
						Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							[this](HRESULT hrCtrl, ICoreWebView2Controller *ctrl) -> HRESULT {
								if (FAILED(hrCtrl)) return hrCtrl;
								controller_ = ctrl;
								controller_->get_CoreWebView2(&webview_);

								// Fit to client area.
								RECT rc{};
								GetClientRect(hwnd_, &rc);
								controller_->put_Bounds(rc);

								injectCookie();
								preWarmNetwork();
								webview_->Navigate(initialUrl_.c_str());
								return S_OK;
							})
						.Get());
					return S_OK;
				})
			.Get());
	}

	void preWarmNetwork() {
		if (webview_) {
			webview_->ExecuteScript(
				L"fetch('https://www.roblox.com/favicon.ico').catch(()=>{});",
				nullptr /* completion handler */);
		}
	}

	void injectCookie() {
		if (cookieValue_.empty() || !webview_) return;

		ComPtr<ICoreWebView2_2> webview2_2;
		if (FAILED(webview_.As(&webview2_2))) return;

		ComPtr<ICoreWebView2CookieManager> mgr;
		if (FAILED(webview2_2->get_CookieManager(&mgr))) return;

		ComPtr<ICoreWebView2Cookie> cookie;
		if (FAILED(mgr->CreateCookie(L".ROBLOSECURITY", cookieValue_.c_str(),
			L".roblox.com", L"/", &cookie)))
			return;

		cookie->put_IsSecure(TRUE);
		cookie->put_IsHttpOnly(TRUE);
		cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);

		using namespace std::chrono;
		double expires = duration_cast<seconds>(
					system_clock::now().time_since_epoch() + hours(24 * 365 * 10))
				.count();
		cookie->put_Expires(expires);

		mgr->AddOrUpdateCookie(cookie.Get());
	}

	void resize() {
		if (!controller_) return;
		RECT rc{};
		GetClientRect(hwnd_, &rc);
		controller_->put_Bounds(rc);

		// Per-monitor DPI scaling.
		ComPtr<ICoreWebView2Controller3> ctl3;
		if (SUCCEEDED(controller_.As(&ctl3))) {
			UINT dpi = GetDpiForWindow(hwnd_);
			ctl3->put_RasterizationScale(static_cast<double>(dpi) / 96.0);
		}
	}

	static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
		LONG_PTR raw = (m == WM_NCCREATE)
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
				RECT *nr = reinterpret_cast<RECT *>(l);
				SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left,
				             nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
					// Update WebView bounds and rasterization scale for new DPI
					self->resize();
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

void LaunchWebview(const std::string &url,
                   const std::string &windowName,
                   const std::string &cookie,
                   const std::string &userId) {
	std::wstring wUrl = widen(url);
	std::wstring wWindowName = widen(windowName);
	std::wstring wCookie = widen(cookie);
	std::wstring wUserId = widen(userId);
	
	std::thread([wUrl, wWindowName, wCookie, wUserId] {
		auto win = std::make_unique<WebViewWindow>(wUrl, wWindowName, wCookie, wUserId);
		if (win->create()) win->messageLoop();
	}).detach();
}