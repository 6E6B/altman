#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "webview.hpp"
#include "../../components/data.h"  // For AccountData definition
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

@interface WebViewWindowController : NSObject <NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) WKWebView *webView;
@property (strong) NSString *cookieValue;
@property (strong) NSString *userDataFolder;

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId;
- (void)show;
- (void)injectCookie;
@end

@implementation WebViewWindowController

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId {
    self = [super init];
    if (self) {
        _cookieValue = cookie;
        
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
        
        // Enable developer extras (useful for debugging)
        [_webView.configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
        
        [[_window contentView] addSubview:_webView];
        
        // Inject cookie before loading
        if (cookie && [cookie length] > 0) {
            [self injectCookieWithCompletion:^{
                // Pre-warm network connection
                [self preWarmNetwork];
                
                // Load URL after cookie is set
                NSURL *nsURL = [NSURL URLWithString:url];
                if (nsURL) {
                    NSURLRequest *request = [NSURLRequest requestWithURL:nsURL];
                    [self->_webView loadRequest:request];
                }
            }];
        } else {
            // Pre-warm network connection
            [self preWarmNetwork];
            
            // Load URL directly
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
    // Pre-warm network connection by making a lightweight request
    NSString *script = @"fetch('https://www.roblox.com/favicon.ico').catch(()=>{});";
    [_webView evaluateJavaScript:script completionHandler:nil];
}

- (void)injectCookieWithCompletion:(void(^)(void))completion {
    if (!_cookieValue || [_cookieValue length] == 0) {
        if (completion) completion();
        return;
    }
    
    WKHTTPCookieStore *cookieStore = _webView.configuration.websiteDataStore.httpCookieStore;
    
    // Calculate expiration date (10 years from now)
    NSDate *expirationDate = [NSDate dateWithTimeIntervalSinceNow:(60 * 60 * 24 * 365 * 10)];
    
    // Create cookie properties
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
    // Cleanup when window closes
    _webView = nil;
    _window = nil;
}

@end

void LaunchWebview(const std::string &url,
                   const std::string &windowName,
                   const std::string &cookie,
                   const std::string &userId) {
    std::thread([=] {
        @autoreleasepool {
            // Initialize NSApplication if not already done
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            
            // Convert std::string to NSString
            NSString *nsUrl = [NSString stringWithUTF8String:url.c_str()];
            NSString *nsTitle = [NSString stringWithUTF8String:windowName.c_str()];
            NSString *nsCookie = cookie.empty() ? nil : [NSString stringWithUTF8String:cookie.c_str()];
            NSString *nsUserId = userId.empty() ? nil : [NSString stringWithUTF8String:userId.c_str()];
            
            // Create and show window
            WebViewWindowController *controller = [[WebViewWindowController alloc] 
                initWithURL:nsUrl
                      title:nsTitle
                     cookie:nsCookie
                     userId:nsUserId];
            [controller show];
            
            // Activate app
            [NSApp activateIgnoringOtherApps:YES];
            
            // Run the event loop
            [NSApp run];
        }
    }).detach();
}

// Convenience overload for AccountData
void LaunchWebview(const std::string &url, const AccountData &account) {
    std::string windowName = account.displayName.empty() ? account.username : account.displayName;
    LaunchWebview(url, windowName, account.cookie, account.userId);
}