#define ICON_MIN_FA 0xf000
#define ICON_MAX_16_FA 0xf3ff
#define ICON_MIN_BRANDS_FA 0xf300
#define ICON_MAX_BRANDS_FA 0xf3ff
#define IDI_ICON_32 102

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include "ui.h"
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <objbase.h>

#include "components/data.h"
#include "utils/network/roblox_api.h"
#include "utils/gui/notifications.h"
#include "utils/logging/logging.hpp"
#include "utils/gui/confirm.h"
#include "utils/threads/main_thread.h"
#include "utils/system/update.h"
#include <cstdio>
#include <thread>
#include <chrono>
#include <algorithm>

#include <windows.h>

HWND Notifications::g_appHWnd = NULL;

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shcore.lib")

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

// DPI handling globals
static float g_currentDPIScale = 1.0f;
static ImFont *g_rubikFont = nullptr;
static ImFont *g_iconFont = nullptr;

bool CreateDeviceD3D(HWND hWnd);

void CleanupDeviceD3D();

void CreateRenderTarget();

void CleanupRenderTarget();

// DPI handling functions
float GetDPIScale(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi / 96.0f;
}

void ReloadFonts(float dpiScale) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    float baseFontSize = 16.0f * dpiScale;
    float iconFontSize = 13.0f * dpiScale;
    // Load main font
    g_rubikFont = io.Fonts->AddFontFromFileTTF("assets/Rubik-Regular.ttf", baseFontSize);
    if (!g_rubikFont) {
        LOG_ERROR("Failed to load Rubik-Regular.ttf font.");
        g_rubikFont = io.Fonts->AddFontDefault();
    }
    // Load icon font
    ImFontConfig iconCfg;
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    static constexpr ImWchar fa_solid_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    g_iconFont = io.Fonts->AddFontFromFileTTF(
        "assets/fa-solid-900.ttf",
        iconFontSize,
        &iconCfg,
        fa_solid_ranges);
    if (!g_iconFont && g_rubikFont) {
        LOG_ERROR("Failed to load fa-solid-900.ttf font for icons.");
    }
    io.FontDefault = g_rubikFont;
    // Scale ImGui style
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle(); // Reset to default
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(dpiScale);
    // Rebuild font atlas
    io.Fonts->Build();
    // Recreate device objects if they exist
    if (g_pd3dDevice) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Set DPI awareness first
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCom)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Failed to initialize COM library. Error code = 0x%lX", hrCom);
        LOG_ERROR(buf);
        MessageBoxA(NULL, buf, "COM Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    Data::LoadSettings("settings.json");
    if (g_checkUpdatesOnStartup) {
        CheckForUpdates();
    }
    Data::LoadAccounts("accounts.json");
    Data::LoadFriends("friends.json");

    auto refreshAccounts = [] {
        std::vector<int> invalidIds;
        std::string names;
        for (auto &acct: g_accounts) {
            if (acct.cookie.empty())
                continue;
            auto banStatus = RobloxApi::checkBanStatus(acct.cookie);
            if (banStatus == RobloxApi::BanCheckResult::InvalidCookie) {
                invalidIds.push_back(acct.id);
                if (!names.empty())
                    names += ", ";
                names += acct.displayName.empty() ? acct.username : acct.displayName;
            } else if (banStatus == RobloxApi::BanCheckResult::Banned) {
                acct.status = "Banned";
            }
        }
        for (auto &acct: g_accounts) {
            if (!acct.userId.empty()) {
                uint64_t uid = 0;
                try {
                    uid = std::stoull(acct.userId);
                    if (acct.status != "Banned")
                        acct.status = RobloxApi::getPresence(acct.cookie, uid);
                    auto vs = RobloxApi::getVoiceChatStatus(acct.cookie);
                    acct.voiceStatus = vs.status;
                    acct.voiceBanExpiry = vs.bannedUntil;
                } catch (const std::exception &e) {
                    char errorMsg[256];
                    snprintf(errorMsg, sizeof(errorMsg), "Error converting userId %s: %s", acct.userId.c_str(),
                             e.what());
                    LOG_ERROR(errorMsg);
                    acct.status = "Error: Invalid UserID";
                }
            }
        }
        Data::SaveAccounts();
        LOG_INFO("Loaded accounts and refreshed statuses");

        if (!invalidIds.empty()) {
            std::string namesCopy = names;
            MainThread::Post([invalidIds, namesCopy]() {
                char buf[512];
                snprintf(buf, sizeof(buf), "Invalid cookies for: %s. Remove them?", namesCopy.c_str());
                ConfirmPopup::Add(buf, [invalidIds]() {
                    erase_if(g_accounts, [&](const AccountData &a) {
                        return std::find(invalidIds.begin(), invalidIds.end(), a.id) != invalidIds.end();
                    });
                    for (int id: invalidIds) {
                        g_selectedAccountIds.erase(id);
                    }
                    Data::SaveAccounts();
                });
            });
        }
    };

    Threading::newThread([refreshAccounts] {
        refreshAccounts();
        while (true) {
            std::this_thread::sleep_for(std::chrono::minutes(g_statusRefreshInterval));
            LOG_INFO("Refreshing account statuses...");
            refreshAccounts();
            LOG_INFO("Refreshed account statuses");
        }
    });

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        hInstance,
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_32)),
        LoadCursor(nullptr, IDC_ARROW),
        nullptr, nullptr,
        L"ImGui Example",
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_32))
    };
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    int width = MulDiv(950, dpi, 96);
    int height = MulDiv(600, dpi, 96);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"AltMan", WS_OVERLAPPEDWINDOW, 100, 100, width, height, nullptr,
                                nullptr,
                                hInstance,
                                nullptr);

    Notifications::g_appHWnd = hwnd;

    // Get initial DPI scale
    g_currentDPIScale = GetDPIScale(hwnd);

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(
        hwnd, DWMWINDOWATTRIBUTE::DWMWA_USE_IMMERSIVE_DARK_MODE,
        &useDarkMode, sizeof(useDarkMode));

    if (!CreateDeviceD3D(hwnd)) {
        LOG_ERROR("Failed to create D3D device.");
        MessageBoxA(hwnd, "Failed to create D3D device. The application will now exit.", "D3D Error",
                    MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void) io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load fonts with current DPI scaling
    ReloadFonts(g_currentDPIScale);

    auto clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        MainThread::Process();

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        if (RenderUI()) {
            done = true;
        }

        ImGui::PopStyleVar(1);

        ImGui::Render();
        const float clear_color_with_alpha[4] = {
            clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr_present = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr_present == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    CoUninitialize();
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevel;
    constexpr D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                                            &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D *pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_DPICHANGED: {
            float newDPIScale = GetDPIScale(hWnd);
            if (abs(newDPIScale - g_currentDPIScale) > 0.01f) {
                g_currentDPIScale = newDPIScale;
                ReloadFonts(g_currentDPIScale);
            }

            RECT *prcNewWindow = reinterpret_cast<RECT *>(lParam);
            SetWindowPos(hWnd, nullptr,
                         prcNewWindow->left, prcNewWindow->top,
                         prcNewWindow->right - prcNewWindow->left,
                         prcNewWindow->bottom - prcNewWindow->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                return 0;
            g_ResizeWidth = static_cast<UINT>(LOWORD(lParam));
            g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
