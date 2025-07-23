#define ICON_MIN_FA 0xf000
#define ICON_MAX_16_FA 0xf3ff
#define ICON_MIN_BRANDS_FA 0xf300
#define ICON_MAX_BRANDS_FA 0xf3ff
#define IDI_ICON_32 102

#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_IMPLEMENTATION

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "stb_image.h"
#include <d3d11.h>
#include <tchar.h>
#include "../ui.h"
#include <dwmapi.h>
#include <objbase.h>

#include "components/data.h"
#include "network/roblox.h"
#include "ui/notifications.h"
#include "core/logging.hpp"
#include "ui/confirm.h"
#include "system/main_thread.h"
#include "system/update.h"
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
    g_rubikFont = io.Fonts->AddFontFromFileTTF("assets/fonts/rubik-regular.ttf", baseFontSize);
    if (!g_rubikFont) {
        LOG_ERROR("Failed to load rubik-regular.ttf font.");
        g_rubikFont = io.Fonts->AddFontDefault();
    }

    // Load icon font
    ImFontConfig iconCfg;
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    static constexpr ImWchar fa_solid_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    g_iconFont = io.Fonts->AddFontFromFileTTF(
        "assets/fonts/fa-solid.ttf",
        iconFontSize,
        &iconCfg,
        fa_solid_ranges);
    if (!g_iconFont && g_rubikFont) {
        LOG_ERROR("Failed to load fa-solid.ttf font for icons.");
    }
    io.FontDefault = g_rubikFont;

    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();

    style.ScaleAllSizes(dpiScale);

    io.Fonts->Build();

    if (g_pd3dDevice) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}


bool LoadTextureFromMemory(const void *data, size_t data_size, ID3D11ShaderResourceView **out_srv, int *out_width,
                           int *out_height) {
    // Load from disk into a raw RGBA buffer
    int image_width = 0;
    int image_height = 0;
    unsigned char *image_data = stbi_load_from_memory((const unsigned char *) data, (int) data_size, &image_width,
                                                      &image_height, NULL, 4);
    if (image_data == NULL)
        return false;

    // Create texture
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D *pTexture = NULL;
    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;
    subResource.SysMemSlicePitch = 0;
    g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);

    // Create texture view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;
    stbi_image_free(image_data);

    return true;
}

// Open and read a file, then forward to LoadTextureFromMemory()
bool LoadTextureFromFile(const char *file_name, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height) {
    FILE *f = fopen(file_name, "rb");
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    size_t file_size = (size_t) ftell(f);
    if (file_size == -1)
        return false;
    fseek(f, 0, SEEK_SET);
    void *file_data = IM_ALLOC(file_size);
    fread(file_data, 1, file_size, f);
    fclose(f);
    bool ret = LoadTextureFromMemory(file_data, file_size, out_srv, out_width, out_height);
    IM_FREE(file_data);
    return ret;
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
            auto banInfo = Roblox::checkBanStatus(acct.cookie);
            if (banInfo.status == Roblox::BanCheckResult::InvalidCookie) {
                invalidIds.push_back(acct.id);
                if (!names.empty())
                    names += ", ";
                names += acct.displayName.empty() ? acct.username : acct.displayName;
            } else if (banInfo.status == Roblox::BanCheckResult::Banned) {
                acct.status = "Banned";
                acct.banExpiry = banInfo.endDate;
                g_selectedAccountIds.erase(acct.id);
            } else if (banInfo.status == Roblox::BanCheckResult::Terminated) {
                acct.status = "Terminated";
                acct.banExpiry = 0; // Terminated accounts don't have an end date
                g_selectedAccountIds.erase(acct.id);
            }
        }
        for (auto &acct: g_accounts) {
            if (acct.cookie.empty() && acct.userId.empty())
                continue;

            bool needsUserInfoUpdate = true;
            uint64_t uid = 0;

            // Try cookie first - it's the most authoritative source
            if (!acct.cookie.empty()) {
                auto banInfo = Roblox::checkBanStatus(acct.cookie);
                if (banInfo.status == Roblox::BanCheckResult::Banned) {
                    acct.status = "Banned";
                    acct.banExpiry = banInfo.endDate;
                    acct.voiceStatus = "N/A";
                    acct.voiceBanExpiry = 0;
                    continue;
                } else if (banInfo.status == Roblox::BanCheckResult::Terminated) {
                    acct.status = "Terminated";
                    acct.banExpiry = 0;
                    acct.voiceStatus = "N/A";
                    acct.voiceBanExpiry = 0;
                    continue;
                } else if (banInfo.status == Roblox::BanCheckResult::Unbanned) {
                    // Get fresh data from authenticated endpoint
                    auto userJson = Roblox::getAuthenticatedUser(acct.cookie);
                    if (!userJson.empty()) {
                        // Update everything from authenticated data
                        acct.userId = std::to_string(userJson.value("id", 0ULL));
                        acct.username = userJson.value("name", "");
                        acct.displayName = userJson.value("displayName", "");
                        needsUserInfoUpdate = false;

                        try {
                            uid = std::stoull(acct.userId);
                            auto presences = Roblox::getPresences({uid}, acct.cookie);
                            if (!presences.empty()) {
                                auto it = presences.find(uid);
                                if (it != presences.end()) {
                                    acct.status = it->second.presence;
                                    acct.lastLocation = it->second.lastLocation;
                                } else {
                                    acct.status = "Offline";
                                    acct.lastLocation = "";
                                }
                            } else {
                                acct.status = Roblox::getPresence(acct.cookie, uid);
                                acct.lastLocation = "";
                            }
                            auto vs = Roblox::getVoiceChatStatus(acct.cookie);
                            acct.voiceStatus = vs.status;
                            acct.voiceBanExpiry = vs.bannedUntil;
                            acct.banExpiry = 0;
                        } catch (const std::exception &e) {
                            LOG_ERROR("Error getting presence: " + std::string(e.what()));
                            acct.status = "Error";
                        }
                    }
                }
            }

            // Fall back to userId if cookie failed or is empty
            if (needsUserInfoUpdate && !acct.userId.empty()) {
                try {
                    uid = std::stoull(acct.userId);
                    auto userInfo = Roblox::getUserInfo(acct.userId);
                    if (userInfo.id != 0) {
                        acct.username = userInfo.username;
                        acct.displayName = userInfo.displayName;
                        acct.status = "Cookie Invalid";
                        acct.voiceStatus = "N/A";
                        acct.voiceBanExpiry = 0;
                    } else {
                        acct.status = "Error: Invalid UserID";
                    }
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
