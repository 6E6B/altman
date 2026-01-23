#define STB_IMAGE_IMPLEMENTATION

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <TargetConditionals.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include "utils/stb_image.h"

#include "assets/fonts/embedded_fa_solid.h"
#include "assets/fonts/embedded_rubik.h"
#include "components/data.h"
#include "console/console.h"
#include "image.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/auto_updater.h"
#include "system/client_update_checker_macos.h"
#include "ui/ui.h"
#include "ui/widgets/modal_popup.h"
#include "ui/widgets/notifications.h"
#include "utils/crypto.h"
#include "utils/thread_task.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <thread>

namespace {
    id<MTLDevice> g_metalDevice = nil;
    id<MTLCommandQueue> g_commandQueue = nil;
    ImFont *g_rubikFont = nullptr;
    ImFont *g_iconFont = nullptr;

    constexpr ImWchar ICON_MIN_FA = 0xf000;
    constexpr ImWchar ICON_MAX_16_FA = 0xf3ff;
    constexpr float BASE_FONT_SIZE = 16.0f;

    std::atomic<bool> g_fontReloadPending = false;
    std::atomic<bool> g_running = true;
} // namespace

void ReloadFonts(float dpiScale) {
    IM_ASSERT([NSThread isMainThread]);

    ImGuiIO &io = ImGui::GetIO();

    io.Fonts->Clear();

    const float scaledFontSize = BASE_FONT_SIZE * dpiScale;

    ImFontConfig rubikCfg {};
    rubikCfg.FontDataOwnedByAtlas = false;
    g_rubikFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void *>(static_cast<const void *>(EmbeddedFonts::rubik_regular_ttf)),
        sizeof(EmbeddedFonts::rubik_regular_ttf),
        scaledFontSize,
        &rubikCfg
    );

    if (!g_rubikFont) {
        LOG_ERROR("Failed to load rubik-regular.ttf font.");
        g_rubikFont = io.Fonts->AddFontDefault();
    }

    ImFontConfig iconCfg {};
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.FontDataOwnedByAtlas = false;
    iconCfg.GlyphMinAdvanceX = scaledFontSize;

    static constexpr ImWchar fa_solid_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    g_iconFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void *>(static_cast<const void *>(EmbeddedFonts::fa_solid_ttf)),
        sizeof(EmbeddedFonts::fa_solid_ttf),
        scaledFontSize,
        &iconCfg,
        fa_solid_ranges
    );

    if (!g_iconFont && g_rubikFont) {
        LOG_ERROR("Failed to load fa-solid.ttf font for icons.");
    }

    io.FontDefault = g_rubikFont;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(dpiScale);

    g_fontReloadPending = true;
}

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromMemory(const void *data, size_t data_size) {
    int image_width = 0;
    int image_height = 0;
    unsigned char *image_data = stbi_load_from_memory(
        static_cast<const unsigned char *>(data),
        static_cast<int>(data_size),
        &image_width,
        &image_height,
        nullptr,
        4
    );

    if (!image_data) {
        return std::unexpected("Failed to decode image data");
    }

    auto imageDataCleanup = std::unique_ptr<unsigned char, decltype(&stbi_image_free)>(image_data, stbi_image_free);

    MTLTextureDescriptor *textureDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:static_cast<NSUInteger>(image_width)
                                                          height:static_cast<NSUInteger>(image_height)
                                                       mipmapped:NO];
    textureDescriptor.usage = MTLTextureUsageShaderRead;
    textureDescriptor.storageMode = MTLStorageModeShared;

    id<MTLTexture> texture = [g_metalDevice newTextureWithDescriptor:textureDescriptor];
    if (!texture) {
        return std::unexpected("Failed to create Metal texture");
    }

    [texture
        replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(image_width), static_cast<NSUInteger>(image_height))
          mipmapLevel:0
            withBytes:image_data
          bytesPerRow:4 * static_cast<NSUInteger>(image_width)];

    TextureLoadResult result;
    result.texture.reset(texture);
    result.width = image_width;
    result.height = image_height;

    return result;
}

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromFile(const char *fileName) {
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
        return std::unexpected(std::format("Failed to read file: expected {} bytes", fileSize));
    }

    return LoadTextureFromMemory(fileData.data(), static_cast<size_t>(fileSize));
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
        return {g_accounts.begin(), g_accounts.end()};
    }

    [[nodiscard]]
    ProcessResult processAccount(const AccountSnapshot &account) {
        ProcessResult result {
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
        result.username = userJson.value("name", std::string {});
        result.displayName = userJson.value("displayName", std::string {});

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
                break;

            case Roblox::BanCheckResult::Terminated:
                break;

            case Roblox::BanCheckResult::Unbanned:
                break;

            case Roblox::BanCheckResult::NetworkError:
                break;
        }

        auto voiceStatus = Roblox::getVoiceChatStatus(account.cookie);
        result.voiceStatus = voiceStatus.status;
        result.voiceBanExpiry = voiceStatus.bannedUntil;

        auto presences = Roblox::getPresences({userId}, account.cookie);
        if (auto it = presences.find(userId); it != presences.end()) {
            const auto &presence = it->second;
            result.status = presence.presence;
            result.lastLocation = presence.lastLocation;
            result.placeId = presence.placeId;
            result.jobId = presence.jobId;
        } else {
            result.status = "Offline";
        }

        return result;
    }

    void applyResults(const std::vector<ProcessResult> &results) {
        std::unique_lock lock(g_accountsMutex);

        for (const auto &result: results) {
            auto it = std::ranges::find_if(g_accounts, [&result](const AccountData &a) {
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
                {
                    std::lock_guard lock(g_selectionMutex);
                    g_selectedAccountIds.erase(result.id);
                }
            }
        }

        invalidateAccountIndex();
    }

    void showInvalidCookieModal(std::vector<int> invalidIds, std::string invalidNames) {
        if (invalidIds.empty()) {
            return;
        }

        ThreadTask::RunOnMain([ids = std::move(invalidIds), names = std::move(invalidNames)]() {
            auto message = std::format("Invalid cookies for: {}. Remove them?", names);

            ModalPopup::AddYesNo(message.c_str(), [ids]() {
                std::unique_lock lock(g_accountsMutex);

                std::erase_if(g_accounts, [&ids](const AccountData &a) {
                    return std::ranges::find(ids, a.id) != ids.end();
                });

                invalidateAccountIndex();

                for (int id: ids) {
                    g_selectedAccountIds.erase(id);
                }

                Data::SaveAccounts();
            });
        });
    }
} // namespace AccountProcessor

void refreshAccounts() {
    auto snapshots = AccountProcessor::takeAccountSnapshots();

    std::vector<AccountProcessor::ProcessResult> results;
    results.reserve(snapshots.size());

    std::vector<int> invalidIds;
    std::string invalidNames;

    for (const auto &snapshot: snapshots) {
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

            if (!g_running.load()) {
                break;
            }

            refreshAccounts();
        }
    });
}

void initializeAutoUpdater() {
    /*
        ## GitHub Release Setup

        For delta updates to work, structure releases like this:

        Release v1.1.0
        ├── macOS/
        │   ├── AltMan-macOS-arm64.app.zip
        │   ├── AltMan-macOS-x86_64.app.zip
        │   └── deltas/
        │       ├── AltMan-Delta-1.0.0-to-1.1.0-macOS-arm64.bsdiff
        │       └── AltMan-Delta-1.0.0-to-1.1.0-macOS-x86_64.bsdiff
        │
        ├── Windows/
        │   ├── AltMan-Windows-x86_64.exe
        │   ├── AltMan-Windows-arm64.exe
        │   └── deltas/
        │       ├── AltMan-Delta-1.0.0-to-1.1.0-Windows-x86_64.xdelta
        │       └── AltMan-Delta-1.0.0-to-1.1.0-Windows-arm64.xdelta
        │
        └── beta/
            └── AltMan-Windows-x86_64-beta.exe


        Creating Delta Patches
        Windows (xdelta3):
        ```xdelta3 -e -s AltMan-1.0.0-Windows-x86_64.exe AltMan-1.1.0-Windows-x86_64.exe
       AltMan-Delta-1.0.0-to-1.1.0-Windows-x86_64.xdelta```
        ```xdelta3 -e -s AltMan-1.0.0-Windows-arm64.exe AltMan-1.1.0-Windows-arm64.exe
       AltMan-Delta-1.0.0-to-1.1.0-Windows-arm64.xdelta``` macOS (bsdiff): Extract slices
        ```lipo AltMan-1.0.0.app/Contents/MacOS/AltMan -thin arm64 -output old.arm64```
        ```lipo AltMan-1.1.0.app/Contents/MacOS/AltMan -thin arm64 -output new.arm64```

        ```lipo AltMan-1.0.0.app/Contents/MacOS/AltMan -thin x86_64 -output old.x86_64```
        ```lipo AltMan-1.1.0.app/Contents/MacOS/AltMan -thin x86_64 -output new.x86_64```

        Create deltas per architecture
        ```xdelta3 -e -s old.arm64 new.arm64 AltMan-Delta-1.0.0-to-1.1.0-macOS-arm64.xdelta```
        ```xdelta3 -e -s old.x86_64 new.x86_64 AltMan-Delta-1.0.0-to-1.1.0-macOS-x86_64.xdelta```
        OR (preferred for macOS binaries):
        ```bsdiff old.arm64 new.arm64 AltMan-Delta-1.0.0-to-1.1.0-macOS-arm64.bsdiff```
        ```bsdiff old.x86_64 new.x86_64 AltMan-Delta-1.0.0-to-1.1.0-macOS-x86_64.bsdiff```

        Re-assemble after patch
        ```lipo patched.arm64 patched.x86_64 -create -output AltMan```
     */
    AutoUpdater::Initialize();
    AutoUpdater::SetBandwidthLimit(5_MB);
    AutoUpdater::SetUpdateChannel(UpdateChannel::Stable);
    AutoUpdater::SetAutoUpdate(true, true, false);
    ClientUpdateChecker::UpdateChecker::Initialize();
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}
@end

@interface AppViewController : NSViewController <MTKViewDelegate>
@end

@implementation AppViewController {
    MTKView *_view;
    id<MTLCommandQueue> _commandQueue;
    ImGuiContext *_imguiContext;
    float _lastDPIScale;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        g_metalDevice = MTLCreateSystemDefaultDevice();
        _commandQueue = [g_metalDevice newCommandQueue];
        g_commandQueue = _commandQueue;
        _lastDPIScale = 1.0f;

        IMGUI_CHECKVERSION();
        _imguiContext = ImGui::CreateContext();

        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        ImGui::StyleColorsDark();
    }
    return self;
}

- (void)dealloc {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();

#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    g_running = false;
}

- (void)loadView {
    constexpr CGFloat windowWidth = 1000.0;
    constexpr CGFloat windowHeight = 560.0;

    NSRect frame = NSMakeRect(0, 0, windowWidth, windowHeight);
    _view = [[MTKView alloc] initWithFrame:frame device:g_metalDevice];
    _view.delegate = self;
    _view.clearColor = MTLClearColorMake(0.45, 0.55, 0.60, 1.00);
    self.view = _view;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ImGui_ImplOSX_Init(self.view);
    ImGui_ImplMetal_Init(g_metalDevice);

    CGFloat scale = self.view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
    _lastDPIScale = static_cast<float>(scale);
    ReloadFonts(_lastDPIScale);
}

- (void)drawInMTKView:(MTKView *)view {
    IM_ASSERT([NSThread isMainThread]);

    ThreadTask::RunOnMainUpdate();

    const CGFloat framebufferScale = view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;

    const float currentScale = static_cast<float>(framebufferScale);
    if (std::abs(currentScale - _lastDPIScale) > 0.01f) {
        _lastDPIScale = currentScale;
        ReloadFonts(_lastDPIScale);
    }

    ImGuiIO &io = ImGui::GetIO();

    io.DisplaySize = ImVec2(
        static_cast<float>(view.drawableSize.width) / currentScale,
        static_cast<float>(view.drawableSize.height) / currentScale
    );
    io.DisplayFramebufferScale = ImVec2(currentScale, currentScale);

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    MTLRenderPassDescriptor *renderPassDescriptor = view.currentRenderPassDescriptor;

    if (!renderPassDescriptor) {
        return;
    }

    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    bool shouldQuit = false;
    {
        std::shared_lock lock(g_accountsMutex);
        shouldQuit = RenderUI();
    }

    if (g_fontReloadPending.exchange(false)) {
        ImGui_ImplMetal_DestroyDeviceObjects();
        ImGui_ImplMetal_CreateDeviceObjects(g_metalDevice);
    }

    ImGui::PopStyleVar(1);
    ImGui::Render();

    ImDrawData *drawData = ImGui::GetDrawData();
    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

    [renderEncoder pushDebugGroup:@ "Dear ImGui rendering"];
    ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, renderEncoder);
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];

    if (shouldQuit) {
        [[NSApplication sharedApplication] terminate:nil];
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
}

@end

NSWindow *createMainWindow() {
    constexpr CGFloat windowWidth = 1000.0;
    constexpr CGFloat windowHeight = 560.0;

    NSRect frame = NSMakeRect(0, 0, windowWidth, windowHeight);

    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];

    [window setTitle:@ "AltMan"];
    [window center];

    return window;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (auto result = Crypto::initialize(); !result) {
            std::println("Failed to initialize crypto library {}", Crypto::errorToString(result.error()));
            return 1;
        }

        Data::LoadSettings("settings.json");

        if (g_checkUpdatesOnStartup) {
            initializeAutoUpdater();
        }

        Data::LoadAccounts("accounts.json");
        Data::LoadFriends("friends.json");

        startAccountRefreshLoop();

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *appDelegate = [[AppDelegate alloc] init];
        [app setDelegate:appDelegate];

        NSWindow *window = createMainWindow();
        AppViewController *viewController = [[AppViewController alloc] init];
        [window setContentViewController:viewController];

        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
