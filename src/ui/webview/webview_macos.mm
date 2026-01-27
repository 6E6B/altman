#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <filesystem>
#include <memory>
#include <thread>

#include "components/data.h"
#include "utils/paths.h"
#include "webview.h"

std::string GetUserDataFolder() {
    std::filesystem::path p = AltMan::Paths::WebViewProfiles();
    return p.string();
}

@class WebViewWindowController;

// Global dictionary to track one window per account
static NSMutableDictionary<NSString *, WebViewWindowController *> *g_webByUser;

static NSMutableDictionary *GetWebByUser() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      g_webByUser = [NSMutableDictionary new];
    });
    return g_webByUser;
}

@interface WebViewWindowController : NSObject <NSWindowDelegate, WKNavigationDelegate>
@property(strong) NSWindow *window;
@property(strong) WKWebView *webView;
@property(strong) NSString *cookieValue;
@property(strong) NSString *userDataFolder;
@property(strong) NSString *accountKey;
@property(copy) void (^cookieCallback)(NSString *);

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId
                 accountKey:(NSString *)accountKey
             cookieCallback:(void (^)(NSString *))callback;
- (void)show;
- (void)injectCookie;
- (void)extractAndNotifyCookie;
@end

@implementation WebViewWindowController

- (instancetype)initWithURL:(NSString *)url
                      title:(NSString *)title
                     cookie:(NSString *)cookie
                     userId:(NSString *)userId
                 accountKey:(NSString *)accountKey
             cookieCallback:(void (^)(NSString *))callback {
    self = [super init];
    if (self) {
        _cookieValue = cookie;
        _accountKey = [accountKey copy];
        _cookieCallback = [callback copy];

        std::string userDataPath;
        if (userId && [userId length] > 0) {
            std::string userIdStr = [userId UTF8String];
            std::string sanitized;
            sanitized.reserve(userIdStr.size());
            for (char ch: userIdStr) {
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
                    sanitized.push_back(ch);
                } else {
                    sanitized.push_back('_');
                }
            }
            std::filesystem::path p = std::filesystem::path(GetUserDataFolder()) / ("u_" + sanitized);
            std::filesystem::create_directories(p);
            userDataPath = p.string();
        } else if (cookie && [cookie length] > 0) {
            std::string cookieStr = [cookie UTF8String];
            size_t h = std::hash<std::string> {}(cookieStr);
            char hashHex[17] {};
            snprintf(hashHex, 17, "%016llX", static_cast<unsigned long long>(h));
            std::filesystem::path p = std::filesystem::path(GetUserDataFolder()) / ("c_" + std::string(hashHex));
            std::filesystem::create_directories(p);
            userDataPath = p.string();
        } else {
            userDataPath = GetUserDataFolder();
        }

        _userDataFolder = [NSString stringWithUTF8String:userDataPath.c_str()];

        NSRect frame = NSMakeRect(100, 100, 1280, 800);
        NSWindowStyleMask styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                      | NSWindowStyleMaskMiniaturizable;

        _window =
            [[NSWindow alloc] initWithContentRect:frame styleMask:styleMask backing:NSBackingStoreBuffered defer:NO];
        [_window setTitle:title];
        [_window setDelegate:self];

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];

        WKWebpagePreferences *pagePrefs = [[WKWebpagePreferences alloc] init];
config.defaultWebpagePreferences = pagePrefs;

        config.suppressesIncrementalRendering = NO; // Don't wait for full page

        if (callback != nil || (cookie && [cookie length] > 0)) {
            config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        } else {
            config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        }

        // Fix for annoying keychain popups when there is a login
        NSString *blockWebCrypto = @"Object.defineProperty(window, 'crypto', { get: function() { return { subtle: null, getRandomValues: function(arr) { for(var i=0; i<arr.length; i++) arr[i] = Math.floor(Math.random()*256); return arr; } }; }});";
        WKUserScript *script = [[WKUserScript alloc]
            initWithSource:blockWebCrypto
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
            forMainFrameOnly:NO];
        [config.userContentController addUserScript:script];

        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

        _webView = [[WKWebView alloc] initWithFrame:[[_window contentView] bounds] configuration:config];
        _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _webView.navigationDelegate = self;

        [_webView setWantsLayer:YES];
        _webView.layer.drawsAsynchronously = YES;

        [_webView.configuration.preferences setValue:@YES forKey:@ "developerExtrasEnabled"];

        WKPreferences *prefs = _webView.configuration.preferences;
        prefs.javaScriptCanOpenWindowsAutomatically = YES;
        [prefs setValue:@YES forKey:@ "acceleratedCompositingEnabled"];
        [prefs setValue:@YES forKey:@ "webGLEnabled"];

        [[_window contentView] addSubview:_webView];

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

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    if (_cookieCallback) {
        NSString *urlString = webView.URL.absoluteString;

        if ([urlString containsString:@ "roblox.com"]) {
            [self extractAndNotifyCookie];
        }
    }
}

- (void)extractAndNotifyCookie {
    WKHTTPCookieStore *cookieStore = _webView.configuration.websiteDataStore.httpCookieStore;

    [cookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
      NSString *robloSecurityValue = nil;

      for (NSHTTPCookie *cookie in cookies) {
          if ([cookie.name isEqualToString:@ ".ROBLOSECURITY"]) {
              robloSecurityValue = cookie.value;
              break;
          }
      }

      if (robloSecurityValue && self.cookieCallback) {
          self.cookieCallback(robloSecurityValue);
      }
    }];
}

- (void)preWarmNetwork {
    NSString *script = @ "fetch('https://www.roblox.com/favicon.ico').catch(()=>{});";
    [_webView evaluateJavaScript:script completionHandler:nil];
}

- (void)injectCookieWithCompletion:(void (^)(void))completion {
    if (!_cookieValue || [_cookieValue length] == 0) {
        if (completion) {
            completion();
        }
        return;
    }

    WKHTTPCookieStore *cookieStore = _webView.configuration.websiteDataStore.httpCookieStore;

    NSMutableDictionary *cookieProperties = [NSMutableDictionary dictionary];
    [cookieProperties setObject:@ ".ROBLOSECURITY" forKey:NSHTTPCookieName];
    [cookieProperties setObject:_cookieValue forKey:NSHTTPCookieValue];
    [cookieProperties setObject:@ ".roblox.com" forKey:NSHTTPCookieDomain];
    [cookieProperties setObject:@ "/" forKey:NSHTTPCookiePath];
    [cookieProperties setObject:@ "TRUE" forKey:NSHTTPCookieSecure];

    NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:cookieProperties];

    if (cookie) {
        [cookieStore setCookie:cookie
             completionHandler:^{
               if (completion) {
                   completion();
               }
             }];
    } else {
        NSLog(@ "Failed to create cookie");
        if (completion) {
            completion();
        }
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
    if (_accountKey) {
        [GetWebByUser() removeObjectForKey:_accountKey];
    }

    _webView = nil;
    _window = nil;
}

@end

void LaunchWebviewImpl(
    const std::string &url,
    const std::string &windowName,
    const std::string &cookie,
    const std::string &userId,
    CookieCallback onCookieExtracted
) {
    std::string urlCopy = url;
    std::string titleCopy = windowName;
    std::string cookieCopy = cookie;
    std::string userIdCopy = userId;

    void (^objcCallback)(NSString *) = nullptr;
    if (onCookieExtracted) {
        objcCallback = ^(NSString *cookieValue) {
          std::string cppCookie = [cookieValue UTF8String];
          onCookieExtracted(cppCookie);
        };
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      @autoreleasepool {
          NSString *nsUrl = [NSString stringWithUTF8String:urlCopy.c_str()];
          NSString *nsTitle = [NSString stringWithUTF8String:titleCopy.c_str()];
          NSString *nsCookie = cookieCopy.empty() ? nil : [NSString stringWithUTF8String:cookieCopy.c_str()];
          NSString *nsUserId = userIdCopy.empty() ? nil : [NSString stringWithUTF8String:userIdCopy.c_str()];

          NSString *accountKey;
          if (objcCallback != nil) {
              accountKey = [NSString stringWithFormat:@ "login_%f", [[NSDate date] timeIntervalSince1970]];
          } else if (nsUserId && [nsUserId length] > 0) {
              accountKey = nsUserId;
          } else if (nsCookie && [nsCookie length] > 0) {
              size_t h = std::hash<std::string> {}([nsCookie UTF8String]);
              accountKey = [NSString stringWithFormat:@ "cookie_%016llX", (unsigned long long) h];
          } else {
              accountKey = nsUrl;
          }

          NSMutableDictionary *dict = GetWebByUser();
          WebViewWindowController *existing = dict[accountKey];

          if (existing && objcCallback == nil) {
              [existing show];
              [existing.window makeKeyAndOrderFront:nil];
              [NSApp activateIgnoringOtherApps:YES];
              return;
          }

          WebViewWindowController *controller = [[WebViewWindowController alloc] initWithURL:nsUrl
                                                                                       title:nsTitle
                                                                                      cookie:nsCookie
                                                                                      userId:nsUserId
                                                                                  accountKey:accountKey
                                                                              cookieCallback:objcCallback];

          dict[accountKey] = controller;

          [controller show];
          [NSApp activateIgnoringOtherApps:YES];
      }
    });
}

void LaunchWebview(const std::string &url, const AccountData &account) {
    std::string title = !account.displayName.empty()
                            ? account.displayName
                            : account.username + (account.userId.empty() ? "" : (" - " + account.userId));

    LaunchWebviewImpl(url, title, account.cookie, account.userId, nullptr);
}

void LaunchWebview(const std::string &url, const AccountData &account, const std::string &windowName) {
    LaunchWebviewImpl(url, windowName.empty() ? account.username : windowName, account.cookie, account.userId, nullptr);
}

void LaunchWebviewForLogin(const std::string &url, const std::string &windowName, CookieCallback onCookieExtracted) {
    LaunchWebviewImpl(url, windowName, "", "", onCookieExtracted);
}
