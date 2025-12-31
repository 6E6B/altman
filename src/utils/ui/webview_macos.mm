#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "webview.hpp"
#include "../../components/data.h"
#include <thread>
#include <filesystem>
#include <memory>

static inline const std::string kUserDataFolder = [] {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::filesystem::path p = std::filesystem::path(home) / "Library" / "Application Support" / "Altman" / "WebViewProfiles" / "Roblox";
    std::filesystem::create_directories(p);
    return p.string();
}();

@class WebViewWindowController;

// Global dictionary to track one window per account
static NSMutableDictionary<NSString*, WebViewWindowController*> *g_webByUser;

static NSMutableDictionary *GetWebByUser() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_webByUser = [NSMutableDictionary new];
    });
    return g_webByUser;
}

@interface WebViewWindowController : NSObject <NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) WKWebView *webView;
@property (strong) NSString *cookieValue;
@property (strong) NSString *userDataFolder;
@property (strong) NSString *accountKey;  // Unique key for this account

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId
                 accountKey:(NSString *)accountKey;
- (void)show;
- (void)injectCookie;
@end

@implementation WebViewWindowController

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId
                 accountKey:(NSString *)accountKey {
    self = [super init];
    if (self) {
        _cookieValue = cookie;
        _accountKey = [accountKey copy];

        // Derive per-user data folder
        std::string userDataPath;
        if (userId && [userId length] > 0) {
            std::string userIdStr = [userId UTF8String];
            std::string sanitized;
            sanitized.reserve(userIdStr.size());
            for (char ch : userIdStr) {
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') || ch == '_')
                    sanitized.push_back(ch);
                else
                    sanitized.push_back('_');
            }
            std::filesystem::path p = std::filesystem::path(kUserDataFolder) / ("u_" + sanitized);
            std::filesystem::create_directories(p);
            userDataPath = p.string();
        } else if (cookie && [cookie length] > 0) {
            std::string cookieStr = [cookie UTF8String];
            size_t h = std::hash<std::string>{}(cookieStr);
            char hashHex[17]{};
            snprintf(hashHex, 17, "%016llX", static_cast<unsigned long long>(h));
            std::filesystem::path p = std::filesystem::path(kUserDataFolder) / ("c_" + std::string(hashHex));
            std::filesystem::create_directories(p);
            userDataPath = p.string();
        } else {
            userDataPath = kUserDataFolder;
        }

        _userDataFolder = [NSString stringWithUTF8String:userDataPath.c_str()];

        // Create window
        NSRect frame = NSMakeRect(100, 100, 1280, 800);
        NSWindowStyleMask styleMask = NSWindowStyleMaskTitled |
                                      NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskResizable |
                                      NSWindowStyleMaskMiniaturizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        [_window setTitle:title];
        [_window setDelegate:self];

        // Create WKWebView configuration with custom data store
        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];

        // Use non-persistent data store for isolated sessions
        if (cookie && [cookie length] > 0) {
            config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        } else {
            config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        }

        // Create WKWebView
        _webView = [[WKWebView alloc] initWithFrame:[[_window contentView] bounds]
                                      configuration:config];
        _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        // Enable developer extras
        [_webView.configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

        [[_window contentView] addSubview:_webView];

        // Inject cookie before loading
        if (cookie && [cookie length] > 0) {
            [self injectCookieWithCompletion:^{
                [self preWarmNetwork];

                NSURL *nsURL = [NSURL URLWithString:url];
                if (nsURL) {
                    NSURLRequest *request = [NSURLRequest requestWithURL:nsURL];
                    [self->_webView loadRequest:request];
                }
            }];
        } else {
            [self preWarmNetwork];

            NSURL *nsURL = [NSURL URLWithString:url];
            if (nsURL) {
                NSURLRequest *request = [NSURLRequest requestWithURL:nsURL];
                [_webView loadRequest:request];
            }
        }
    }
    return self;
}

- (void)preWarmNetwork {
    NSString *script = @"fetch('https://www.roblox.com/favicon.ico').catch(()=>{});";
    [_webView evaluateJavaScript:script completionHandler:nil];
}

- (void)injectCookieWithCompletion:(void(^)(void))completion {
    if (!_cookieValue || [_cookieValue length] == 0) {
        if (completion) completion();
        return;
    }

    WKHTTPCookieStore *cookieStore = _webView.configuration.websiteDataStore.httpCookieStore;

    NSDate *expirationDate = [NSDate dateWithTimeIntervalSinceNow:(60 * 60 * 24 * 365 * 10)];

    NSMutableDictionary *cookieProperties = [NSMutableDictionary dictionary];
    [cookieProperties setObject:@".ROBLOSECURITY" forKey:NSHTTPCookieName];
    [cookieProperties setObject:_cookieValue forKey:NSHTTPCookieValue];
    [cookieProperties setObject:@".roblox.com" forKey:NSHTTPCookieDomain];
    [cookieProperties setObject:@"/" forKey:NSHTTPCookiePath];
    [cookieProperties setObject:expirationDate forKey:NSHTTPCookieExpires];
    [cookieProperties setObject:@"TRUE" forKey:NSHTTPCookieSecure];

    NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:cookieProperties];

    if (cookie) {
        [cookieStore setCookie:cookie completionHandler:^{
            NSLog(@"Cookie injected successfully");
            if (completion) completion();
        }];
    } else {
        NSLog(@"Failed to create cookie");
        if (completion) completion();
    }
}

- (void)injectCookie {
    [self injectCookieWithCompletion:nil];
}

- (void)show {
    [_window makeKeyAndOrderFront:nil];
    [_window center];
}

- (void)windowWillClose:(NSNotification *)notification {
    // Remove from global dictionary to allow new window for this account
    if (_accountKey) {
        [GetWebByUser() removeObjectForKey:_accountKey];
    }

    _webView = nil;
    _window = nil;
}

@end

void LaunchWebview(const std::string &url,
                   const std::string &windowName,
                   const std::string &cookie,
                   const std::string &userId) {
    // Copy std::strings (safe to capture)
    std::string urlCopy = url;
    std::string titleCopy = windowName;
    std::string cookieCopy = cookie;
    std::string userIdCopy = userId;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            // Convert to NSString on main thread inside autorelease pool
            NSString *nsUrl = [NSString stringWithUTF8String:urlCopy.c_str()];
            NSString *nsTitle = [NSString stringWithUTF8String:titleCopy.c_str()];
            NSString *nsCookie = cookieCopy.empty() ? nil :
                [NSString stringWithUTF8String:cookieCopy.c_str()];
            NSString *nsUserId = userIdCopy.empty() ? nil :
                [NSString stringWithUTF8String:userIdCopy.c_str()];

            // Generate unique key for this account
            NSString *accountKey;
            if (nsUserId && [nsUserId length] > 0) {
                accountKey = nsUserId;
            } else if (nsCookie && [nsCookie length] > 0) {
                // Hash the cookie for the key
                size_t h = std::hash<std::string>{}([nsCookie UTF8String]);
                accountKey = [NSString stringWithFormat:@"cookie_%016llX", (unsigned long long)h];
            } else {
                // Fallback: use URL as key (less ideal but prevents duplicates)
                accountKey = nsUrl;
            }

            // Check if window already exists for this account
            NSMutableDictionary *dict = GetWebByUser();
            WebViewWindowController *existing = dict[accountKey];

            if (existing) {
                // Window already exists - just bring it to front
                [existing show];
                [existing.window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                return;
            }

            // Create new controller
            WebViewWindowController *controller = [[WebViewWindowController alloc]
                initWithURL:nsUrl
                      title:nsTitle
                     cookie:nsCookie
                     userId:nsUserId
                 accountKey:accountKey];

            // Store controller in dictionary
            dict[accountKey] = controller;

            [controller show];
            [NSApp activateIgnoringOtherApps:YES];
        }
    });
}

// Convenience overload for AccountData
void LaunchWebview(const std::string &url, const AccountData &account) {
    std::string windowName = account.displayName.empty() ? account.username : account.displayName;
    LaunchWebview(url, windowName, account.cookie, account.userId);
}