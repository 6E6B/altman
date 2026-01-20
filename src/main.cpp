#define IDI_ICON_32 102
#define STB_IMAGE_IMPLEMENTATION

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <objbase.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <thread>
#include <expected>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "utils/stb_image.h"

#include "ui/ui.h"
#include "components/data.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "ui/widgets/notifications.h"
#include "ui/widgets/modal_popup.h"
#include "utils/thread_task.h"
#include "system/auto_updater.h"
#include "console/console.h"
#include "image.h"

#include "assets/fonts/embedded_rubik.h"
#include "assets/fonts/embedded_fa_solid.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool g_swapChainOccluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

ImFont* g_rubikFont = nullptr;
ImFont* g_iconFont = nullptr;
float g_currentDPIScale = 1.0f;

constexpr ImWchar ICON_MIN_FA = 0xf000;
constexpr ImWchar ICON_MAX_16_FA = 0xf3ff;
constexpr float BASE_FONT_SIZE = 16.0f;

std::mutex g_selectionMutex;
std::shared_mutex g_accountsMutex;
std::atomic<bool> g_running = true;

[[nodiscard]]
float GetDPIScale(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return static_cast<float>(dpi) / 96.0f;
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );

    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevels, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void ReloadFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    const float scaledFontSize = BASE_FONT_SIZE * dpiScale;

    ImFontConfig rubikCfg{};
    rubikCfg.FontDataOwnedByAtlas = false;
    g_rubikFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(static_cast<const void*>(EmbeddedFonts::rubik_regular_ttf)),
        sizeof(EmbeddedFonts::rubik_regular_ttf),
        scaledFontSize,
        &rubikCfg
    );

    if (!g_rubikFont) {
        LOG_ERROR("Failed to load rubik-regular.ttf font.");
        g_rubikFont = io.Fonts->AddFontDefault();
    }

    ImFontConfig iconCfg{};
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.FontDataOwnedByAtlas = false;
    iconCfg.GlyphMinAdvanceX = scaledFontSize;

    static constexpr ImWchar fa_solid_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    g_iconFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(static_cast<const void*>(EmbeddedFonts::fa_solid_ttf)),
        sizeof(EmbeddedFonts::fa_solid_ttf),
        scaledFontSize,
        &iconCfg,
        fa_solid_ranges
    );

    if (!g_iconFont && g_rubikFont) {
        LOG_ERROR("Failed to load fa-solid.ttf font for icons.");
    }

    io.FontDefault = g_rubikFont;

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(dpiScale);

    io.Fonts->Build();

    if (g_pd3dDevice) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

}

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromMemory(const void* data, size_t dataSize) {
    int imageWidth = 0;
    int imageHeight = 0;
    unsigned char* imageData = stbi_load_from_memory(
        static_cast<const unsigned char*>(data),
        static_cast<int>(dataSize),
        &imageWidth,
        &imageHeight,
        nullptr,
        4
    );

    if (!imageData) {
        return std::unexpected("Failed to decode image data");
    }

    auto imageDataCleanup = std::unique_ptr<unsigned char, decltype(&stbi_image_free)>(
        imageData, stbi_image_free
    );

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(imageWidth);
    desc.Height = static_cast<UINT>(imageHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource{};
    subResource.pSysMem = imageData;
    subResource.SysMemPitch = static_cast<UINT>(imageWidth) * 4;

    ID3D11Texture2D* pTexture = nullptr;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    if (FAILED(hr) || !pTexture) {
        return std::unexpected("Failed to create D3D11 texture");
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &srv);
    pTexture->Release();

    if (FAILED(hr) || !srv) {
        return std::unexpected("Failed to create shader resource view");
    }

    TextureLoadResult result;
    result.texture.reset(srv);
    result.width = imageWidth;
    result.height = imageHeight;

    return result;
}

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromFile(const char* fileName)
{
	std::ifstream file(fileName, std::ios::binary | std::ios::ate);
	if (!file) {
		return std::unexpected(std::format("Failed to open file: {}", fileName));
	}

	const std::streamsize fileSize = file.tellg();
	if (fileSize <= 0) {
		return std::unexpected("Failed to determine file size");
	}

	file.seekg(0, std::ios::beg);

	std::vector<char> fileData(static_cast<size_t>(fileSize));
	if (!file.read(fileData.data(), fileSize)) {
		return std::unexpected(std::format(
			"Failed to read file: expected {} bytes", fileSize));
	}

	return LoadTextureFromMemory(fileData.data(),
								 static_cast<size_t>(fileSize));
}

namespace AccountProcessor {

using AccountSnapshot = AccountData;

struct ProcessResult {
    int id;
    std::string userId;
    std::string username;
    std::string displayName;
    std::string status;
    std::string lastLocation;
    uint64_t placeId = 0;
    std::string jobId;
    std::string voiceStatus;
    time_t banExpiry = 0;
    time_t voiceBanExpiry = 0;
    bool shouldDeselect = false;
    bool isInvalid = false;
};

[[nodiscard]]
std::vector<AccountSnapshot> takeAccountSnapshots() {
    std::shared_lock lock(g_accountsMutex);
    return { g_accounts.begin(), g_accounts.end() };
}

[[nodiscard]]
ProcessResult processAccount(const AccountSnapshot& account) {
    ProcessResult result{
        .id = account.id,
        .userId = account.userId,
        .username = account.username,
        .displayName = account.displayName,
        .status = "Unknown"
    };

    if (account.cookie.empty()) {
        return result;
    }

    auto userJson = Roblox::getAuthenticatedUser(account.cookie);
    if (userJson.empty()) {
        result.status = "Network Error";
        result.voiceStatus = "N/A";
        result.shouldDeselect = true;
        result.isInvalid = false;
        return result;
    }

    const uint64_t userId = userJson.value("id", 0ULL);
    result.userId = std::to_string(userId);
    result.username = userJson.value("name", std::string{});
    result.displayName = userJson.value("displayName", std::string{});

    if (userId == 0) {
        result.status = "Error";
        return result;
    }

    auto banInfo = Roblox::checkBanStatus(account.cookie);
    switch (banInfo.status) {
        case Roblox::BanCheckResult::InvalidCookie:
            result.isInvalid = true;
            break;

        case Roblox::BanCheckResult::Banned:
            result.status = "Banned";
            result.banExpiry = banInfo.endDate;
            result.voiceStatus = "N/A";
            result.shouldDeselect = true;
            return result;

        case Roblox::BanCheckResult::Warned:
        case Roblox::BanCheckResult::Terminated:
        case Roblox::BanCheckResult::Unbanned:
        case Roblox::BanCheckResult::NetworkError:
            break;
    }

    auto voiceStatus = Roblox::getVoiceChatStatus(account.cookie);
    result.voiceStatus = voiceStatus.status;
    result.voiceBanExpiry = voiceStatus.bannedUntil;

    auto presences = Roblox::getPresences({ userId }, account.cookie);
    if (auto it = presences.find(userId); it != presences.end()) {
        const auto& presence = it->second;
        result.status = presence.presence;
        result.lastLocation = presence.lastLocation;
        result.placeId = presence.placeId;
        result.jobId = presence.jobId;
    } else {
        result.status = "Offline";
    }

    return result;
}

void applyResults(const std::vector<ProcessResult>& results) {
    std::unique_lock lock(g_accountsMutex);

    for (const auto& result : results) {
        auto it = std::ranges::find_if(g_accounts, [&result](const AccountData& a) {
            return a.id == result.id;
        });

        if (it == g_accounts.end()) {
            continue;
        }

        it->userId = result.userId;
        it->username = result.username;
        it->displayName = result.displayName;
        it->status = result.status;
        it->lastLocation = result.lastLocation;
        it->placeId = result.placeId;
        it->jobId = result.jobId;
        it->voiceStatus = result.voiceStatus;
        it->banExpiry = result.banExpiry;
        it->voiceBanExpiry = result.voiceBanExpiry;

        if (result.shouldDeselect) {
            std::lock_guard selLock(g_selectionMutex);
            g_selectedAccountIds.erase(result.id);
        }
    }

    invalidateAccountIndex();
}

void showInvalidCookieModal(std::vector<int> invalidIds, std::string invalidNames) {
    if (invalidIds.empty()) {
        return;
    }

    ThreadTask::RunOnMain([ids = std::move(invalidIds),
                           names = std::move(invalidNames)]() {
        auto message = std::format("Invalid cookies for: {}. Remove them?", names);

        ModalPopup::AddYesNo(message.c_str(), [ids]() {
            std::unique_lock lock(g_accountsMutex);

            std::erase_if(g_accounts, [&ids](const AccountData& a) {
                return std::ranges::find(ids, a.id) != ids.end();
            });

            invalidateAccountIndex();

            for (int id : ids) {
                g_selectedAccountIds.erase(id);
            }

            Data::SaveAccounts();
        });
    });
}

}

void refreshAccounts() {
    auto snapshots = AccountProcessor::takeAccountSnapshots();

    std::vector<AccountProcessor::ProcessResult> results;
    results.reserve(snapshots.size());

    std::vector<int> invalidIds;
    std::string invalidNames;

    for (const auto& snapshot : snapshots) {
        auto result = AccountProcessor::processAccount(snapshot);

        if (result.isInvalid) {
            invalidIds.push_back(result.id);
            if (!invalidNames.empty()) {
                invalidNames.append(", ");
            }
            invalidNames.append(snapshot.displayName.empty() ? snapshot.username : snapshot.displayName);
        }

        results.push_back(std::move(result));
    }

    ThreadTask::RunOnMain([results = std::move(results),
                           invalidIds = std::move(invalidIds),
                           invalidNames = std::move(invalidNames)]() mutable {
        AccountProcessor::applyResults(results);
        Data::SaveAccounts();
        LOG_INFO("Loaded accounts and refreshed statuses");

        AccountProcessor::showInvalidCookieModal(std::move(invalidIds), std::move(invalidNames));
    });
}

void startAccountRefreshLoop() {
    ThreadTask::fireAndForget([] {
        refreshAccounts();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::minutes(g_statusRefreshInterval));

            if (!g_running.load()) break;

            refreshAccounts();
        }
    });
}

void initializeAutoUpdater() {
    AutoUpdater::Initialize();
    AutoUpdater::SetBandwidthLimit(5_MB);
    AutoUpdater::SetShowNotifications(true);
    AutoUpdater::SetUpdateChannel(UpdateChannel::Stable);
    AutoUpdater::SetAutoUpdate(true, true, false);
}

namespace {

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        case WM_DPICHANGED: {
            float newDPIScale = GetDPIScale(hWnd);
            if (std::abs(newDPIScale - g_currentDPIScale) > 0.01f) {
                g_currentDPIScale = newDPIScale;
                ReloadFonts(g_currentDPIScale);
            }
            auto* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hWnd, nullptr,
                         rect->left, rect->top,
                         rect->right - rect->left,
                         rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

[[nodiscard]]
HWND createMainWindow(HINSTANCE hInstance) {
    constexpr wchar_t CLASS_NAME[] = L"AltMan_WindowClass";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_32));
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int width = MulDiv(1000, dpi, 96);
    int height = MulDiv(560, dpi, 96);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"AltMan",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        nullptr, nullptr,
        hInstance,
        nullptr
    );

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    return hwnd;
}

}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCom)) {
        auto msg = std::format("Failed to initialize COM library. Error code = {:#x}", static_cast<unsigned long>(hrCom));
        LOG_ERROR(msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "COM Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    Data::LoadSettings("settings.json");

    if (g_checkUpdatesOnStartup) {
        initializeAutoUpdater();
    }

    Data::LoadAccounts("accounts.json");
    Data::LoadFriends("friends.json");

    startAccountRefreshLoop();

    HWND hwnd = createMainWindow(hInstance);
    g_currentDPIScale = GetDPIScale(hwnd);

    if (!CreateDeviceD3D(hwnd)) {
        LOG_ERROR("Failed to create D3D device.");
        MessageBoxA(hwnd, "Failed to create D3D device. The application will now exit.",
                    "D3D Error", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ReloadFonts(g_currentDPIScale);

    constexpr ImVec4 clearColor{ 0.45f, 0.55f, 0.60f, 1.00f };

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }

        if (done) break;

        ThreadTask::RunOnMainUpdate();

        if (g_swapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        bool shouldQuit = false;
        {
            std::shared_lock lock(g_accountsMutex);
            shouldQuit = RenderUI();
        }

        ImGui::PopStyleVar(1);
        ImGui::Render();

        const float clearColorWithAlpha[4] = {
            clearColor.x * clearColor.w,
            clearColor.y * clearColor.w,
            clearColor.z * clearColor.w,
            clearColor.w
        };

        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorWithAlpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hrPresent = g_pSwapChain->Present(1, 0);
        g_swapChainOccluded = (hrPresent == DXGI_STATUS_OCCLUDED);

        if (shouldQuit) {
            done = true;
        }
    }

    g_running = false;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    CoUninitialize();

    return 0;
}
