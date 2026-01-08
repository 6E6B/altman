#define STB_IMAGE_IMPLEMENTATION
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include "stb_image.h"

#include "../ui.h"
#include "components/data.h"
#include "network/roblox.h"
#include "ui/notifications.h"
#include "core/logging.hpp"
#include "ui/confirm.h"
#include "system/main_thread.h"
#include "system/update.h"

#include "assets/fonts/embedded_rubik.h"
#include "assets/fonts/embedded_fa_solid.h"

#include <cstdio>
#include <thread>
#include <chrono>
#include <algorithm>
#include <string>

static id<MTLDevice> g_metalDevice = nil;
static id<MTLCommandQueue> g_commandQueue = nil;

// DPI scaling (macOS uses points, but we track scale for consistency)
static float g_currentDPIScale = 1.0f;
static ImFont *g_rubikFont = nullptr;
static ImFont *g_iconFont = nullptr;

#define ICON_MIN_FA 0xf000
#define ICON_MAX_16_FA 0xf3ff

void ReloadFonts(float dpiScale) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    
    float baseFontSize = 16.0f * dpiScale;
    float iconFontSize = 13.0f * dpiScale;

    // Load main font
    //g_rubikFont = io.Fonts->AddFontFromFileTTF("assets/fonts/rubik-regular.ttf", baseFontSize);
    ImFontConfig rubikCfg;
    rubikCfg.FontDataOwnedByAtlas = false;
    g_rubikFont = io.Fonts->AddFontFromMemoryTTF(
        (void*)EmbeddedFonts::rubik_regular_ttf,
        EmbeddedFonts::rubik_regular_ttf_len,
        baseFontSize,
        &rubikCfg
    );

    if (!g_rubikFont) {
        LOG_ERROR("Failed to load rubik-regular.ttf font.");
        g_rubikFont = io.Fonts->AddFontDefault();
    }

    // Load icon font
    ImFontConfig iconCfg;
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.FontDataOwnedByAtlas = false;
    static constexpr ImWchar fa_solid_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    //g_iconFont = io.Fonts->AddFontFromFileTTF("assets/fonts/fa-solid.ttf", iconFontSize, &iconCfg, fa_solid_ranges);*/
    g_iconFont = io.Fonts->AddFontFromMemoryTTF(
        (void*)EmbeddedFonts::fa_solid_ttf,
        EmbeddedFonts::fa_solid_ttf_len,
        iconFontSize,
        &iconCfg,
        fa_solid_ranges
    );
    
    if (!g_iconFont && g_rubikFont) {
        LOG_ERROR("Failed to load fa-solid.ttf font for icons.");
    }
    
    io.FontDefault = g_rubikFont;

    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(dpiScale);
}

bool LoadTextureFromMemory(const void *data, size_t data_size, void **out_texture, int *out_width, int *out_height) {
    int image_width = 0;
    int image_height = 0;
    unsigned char *image_data = stbi_load_from_memory(
        (const unsigned char *)data, 
        (int)data_size, 
        &image_width,
        &image_height, 
        NULL, 
        4
    );
    
    if (image_data == NULL)
        return false;

    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:image_width
        height:image_height
        mipmapped:NO];
    textureDescriptor.usage = MTLTextureUsageShaderRead;
    textureDescriptor.storageMode = MTLStorageModeManaged;

    id<MTLTexture> texture = [g_metalDevice newTextureWithDescriptor:textureDescriptor];
    if (!texture) {
        stbi_image_free(image_data);
        return false;
    }

    [texture replaceRegion:MTLRegionMake2D(0, 0, image_width, image_height)
                mipmapLevel:0
                  withBytes:image_data
                bytesPerRow:4 * image_width];

    *out_texture = (void *)CFBridgingRetain(texture);
    *out_width = image_width;
    *out_height = image_height;
    
    stbi_image_free(image_data);
    return true;
}

bool LoadTextureFromFile(const char *file_name, void **out_texture, int *out_width, int *out_height) {
    FILE *f = fopen(file_name, "rb");
    if (f == NULL)
        return false;
    
    fseek(f, 0, SEEK_END);
    size_t file_size = (size_t)ftell(f);
    if (file_size == -1) {
        fclose(f);
        return false;
    }
    
    fseek(f, 0, SEEK_SET);
    void *file_data = malloc(file_size);
    fread(file_data, 1, file_size, f);
    fclose(f);
    
    bool ret = LoadTextureFromMemory(file_data, file_size, out_texture, out_width, out_height);
    free(file_data);
    return ret;
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
}

- (instancetype)init {
    self = [super init];
    if (self) {
        g_metalDevice = MTLCreateSystemDefaultDevice();
        _commandQueue = [g_metalDevice newCommandQueue];
        g_commandQueue = _commandQueue;

        IMGUI_CHECKVERSION();
        _imguiContext = ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        ImGui::StyleColorsDark();

        ReloadFonts(1.0f);
    }
    return self;
}

- (void)dealloc {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
}

- (void)loadView {
    NSRect frame = NSMakeRect(0, 0, 1000, 560);
    _view = [[MTKView alloc] initWithFrame:frame device:g_metalDevice];
    _view.delegate = self;
    _view.clearColor = MTLClearColorMake(0.45, 0.55, 0.60, 1.00);
    self.view = _view;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    ImGui_ImplOSX_Init(self.view);
    ImGui_ImplMetal_Init(g_metalDevice);
}

- (void)drawInMTKView:(MTKView *)view {
    MainThread::Process();

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize.x = view.bounds.size.width;
    io.DisplaySize.y = view.bounds.size.height;

    CGFloat framebufferScale = view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
    io.DisplayFramebufferScale = ImVec2(framebufferScale, framebufferScale);

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    MTLRenderPassDescriptor *renderPassDescriptor = view.currentRenderPassDescriptor;
    if (renderPassDescriptor == nil) {
        [commandBuffer commit];
        return;
    }

    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    bool shouldQuit = RenderUI();

    ImGui::PopStyleVar(1);

    ImGui::Render();
    ImDrawData *drawData = ImGui::GetDrawData();

    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    [renderEncoder pushDebugGroup:@"Dear ImGui rendering"];
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
    // Handle resize
}

@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        Data::LoadSettings("settings.json");
        if (g_checkUpdatesOnStartup) {
            /*
                ## GitHub Release Setup

                For delta updates to work, structure your releases like this:
                ```
                Release v2.0.0
                ├── AltMan-Windows.exe          (full installer)
                ├── AltMan-macOS.dmg            (full installer)
                ├── AltMan-Linux.AppImage       (full installer)
                ├── AltMan-Delta-1.9.0-to-2.0.0.patch    (delta from 1.9.0)
                ├── AltMan-Delta-1.8.0-to-2.0.0.patch    (delta from 1.8.0)
                └── AltMan-Windows-beta.exe     (beta channel)


                Creating Delta Patches
                Windows (xdelta3):
                ``bash xdelta3 -e -s AltMan-v1.9.0.exe AltMan-v2.0.0.exe AltMan-Delta-1.9.0-to-2.0.0.patch```
                Unix (bsdiff):
                ```bash bsdiff AltMan-v1.9.0.AppImage AltMan-v2.0.0.AppImage AltMan-Delta-1.9.0-to-2.0.0.patch```
                Dependencies Needed
             */
            AutoUpdater::Initialize();
            AutoUpdater::SetBandwidthLimit(5_MB);
            AutoUpdater::SetShowNotifications(true);
            AutoUpdater::SetUpdateChannel(UpdateChannel::Stable);
            AutoUpdater::SetAutoUpdate(true, true, false);
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
                } else if (banInfo.status == Roblox::BanCheckResult::Warned) {
                    acct.status = "Warned";
                    acct.banExpiry = 0;
                    g_selectedAccountIds.erase(acct.id);
                } else if (banInfo.status == Roblox::BanCheckResult::Terminated) {
                    acct.status = "Terminated";
                    acct.banExpiry = 0;
                    g_selectedAccountIds.erase(acct.id);
                }
            }

            for (auto &acct: g_accounts) {
                if (acct.cookie.empty() && acct.userId.empty())
                    continue;

                bool needsUserInfoUpdate = true;
                uint64_t uid = 0;

                if (!acct.cookie.empty()) {
                    auto banInfo = Roblox::checkBanStatus(acct.cookie);
                    if (banInfo.status == Roblox::BanCheckResult::Banned) {
                        acct.status = "Banned";
                        acct.banExpiry = banInfo.endDate;
                        acct.voiceStatus = "N/A";
                        acct.voiceBanExpiry = 0;
                        continue;
                    } else if (banInfo.status == Roblox::BanCheckResult::Warned) {
                        acct.status = "Warned";
                        acct.banExpiry = 0;
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
                        auto userJson = Roblox::getAuthenticatedUser(acct.cookie);
                        if (!userJson.empty()) {
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
                                        acct.placeId = it->second.placeId;
                                        acct.jobId = it->second.jobId;
                                    } else {
                                        acct.status = "Offline";
                                    }
                                } else {
                                    acct.status = Roblox::getPresence(acct.cookie, uid);
                                }
                                auto vs = Roblox::getVoiceChatStatus(acct.cookie);
                                acct.voiceStatus = vs.status;
                                acct.voiceBanExpiry = vs.bannedUntil;
                                acct.banExpiry = 0;
                            } catch (...) {
                                acct.status = "Error";
                            }
                        }
                    }
                }

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
                    } catch (...) {
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
                    ConfirmPopup::AddYesNo(buf, [invalidIds]() {
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

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *appDelegate = [[AppDelegate alloc] init];
        [app setDelegate:appDelegate];

        NSRect frame = NSMakeRect(0, 0, 1000, 560);
        NSWindow *window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered
            defer:NO];
        
        [window setTitle:@"AltMan"];
        [window center];

        AppViewController *viewController = [[AppViewController alloc] init];
        [window setContentViewController:viewController];

        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];

        [app run];
    }
    return 0;
}