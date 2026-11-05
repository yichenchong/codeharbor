#include "../MobileWebViewPolicyAdapter.h"

#include <QGuiApplication>
#include <QTest>
#include <QUuid>
#include <QWindow>

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

// This standalone target MUST NOT link QtWebView: this fixture deliberately
// models its delegate identity/ownership/KVO contract without loading the plugin.
// Real WKWebView and the actual production adapter are used. Action fixtures
// supply only public protocol inputs, so both policy selectors can be invoked
// deterministically without opening OS applications or using remote servers.
@interface QtWKWebViewDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic) NSUInteger unsafePolicyCalls;
@property(nonatomic) NSUInteger progressNotifications;
@property(nonatomic) NSUInteger finishedNotifications;
@end
@implementation QtWKWebViewDelegate
- (void)webView:(WKWebView *)view
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler
{
    Q_UNUSED(view);
    Q_UNUSED(action);
    ++_unsafePolicyCalls;
    handler(WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation
{
    Q_UNUSED(view);
    Q_UNUSED(navigation);
    ++_finishedNotifications;
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                       change:(NSDictionary *)change context:(void *)context
{
    Q_UNUSED(keyPath);
    Q_UNUSED(object);
    Q_UNUSED(change);
    Q_UNUSED(context);
    ++_progressNotifications;
}
@end

// Observe only our guard's explicit readiness property, not WebKit internals.
@protocol CHPolicyGuardState <NSObject>
@property(nonatomic, readonly) BOOL rulesReady;
@end

@interface CHPolicyActionFixture : NSObject
@property(nonatomic, strong) NSURLRequest *request;
@property(nonatomic, strong) id targetFrame;
@end
@implementation CHPolicyActionFixture
@end

@interface CHPolicyResponseFixture : NSObject
@property(nonatomic, strong) NSURLResponse *response;
@end
@implementation CHPolicyResponseFixture
@end

@interface CHPolicyWebView : WKWebView
@property(nonatomic, copy) NSString *lastRequestedURL;
@end
@implementation CHPolicyWebView
- (WKNavigation *)loadRequest:(NSURLRequest *)request
{
    self.lastRequestedURL = request.URL.absoluteString;
    // Only the inert marker enters real WebKit; record routed remote requests
    // without doing network I/O. Native lookup, delegate installation, resource
    // rule compilation and KVO remain the production paths.
    if ([request.URL.scheme isEqualToString:@"about"])
        return [super loadRequest:request];
    return nil;
}
@end

namespace {
WKNavigationAction *actionFor(NSString *url, bool newWindow = false)
{
    CHPolicyActionFixture *action = [CHPolicyActionFixture new];
    action.request = [NSURLRequest requestWithURL:[NSURL URLWithString:url]];
    action.targetFrame = newWindow ? nil : [NSObject new];
    return (WKNavigationAction *)(id)action;
}

WKNavigationResponse *responseFor(NSString *url)
{
    CHPolicyResponseFixture *response = [CHPolicyResponseFixture new];
    response.response = [[NSURLResponse alloc] initWithURL:[NSURL URLWithString:url]
                                                MIMEType:@"text/html"
                                   expectedContentLength:0 textEncodingName:nil];
    return (WKNavigationResponse *)(id)response;
}
} // namespace

class MobileWebViewPolicyDarwinTest : public QObject {
    Q_OBJECT
private slots:
    void nativePolicies();
};

void MobileWebViewPolicyDarwinTest::nativePolicies()
{
    QWindow window;
    window.resize(320, 240);
    window.show();
    QTRY_VERIFY(window.isExposed());
#if TARGET_OS_IPHONE
    UIView *native = (__bridge UIView *)(reinterpret_cast<void *>(window.winId()));
    UIView *root = native.window;
#else
    NSView *native = (__bridge NSView *)(reinterpret_cast<void *>(window.winId()));
    NSView *root = native.window.contentView;
#endif
    QVERIFY(root);
    CHPolicyWebView *view = [[CHPolicyWebView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)];
    QtWKWebViewDelegate *original = [QtWKWebViewDelegate new];
    view.navigationDelegate = original;
    [view addObserver:original forKeyPath:@"estimatedProgress" options:0 context:nil];
    [view addObserver:original forKeyPath:@"title" options:0 context:nil];
    [root addSubview:view];
    const QUrl marker(QStringLiteral("about:blank#ch-policy-")
                      + QUuid::createUuid().toString(QUuid::WithoutBraces));
    [view loadRequest:[NSURLRequest requestWithURL:marker.toNSURL()]];
    QTRY_COMPARE(QString::fromNSString(view.URL.absoluteString), marker.toString());
    auto adapter = ch::createMobileWebViewPolicyAdapter(&window, marker);
    ch::MobileWebViewPolicyResult status;
    QTRY_VERIFY(([&] {
        status = adapter->poll();
        return status.state != ch::MobileWebViewPolicyState::Pending;
    })());
    QVERIFY2(status.state == ch::MobileWebViewPolicyState::Ready,
             qPrintable(status.error));
    // The trusted marker was loaded through the original delegate before guard
    // installation. Count only policy calls after Ready.
    original.unsafePolicyCalls = 0;
    QVERIFY(view.navigationDelegate == original);
    QCOMPARE(class_getInstanceSize(object_getClass(original)),
             class_getInstanceSize([QtWKWebViewDelegate class]));
    QVERIFY(view.UIDelegate);

    NSArray<NSString *> *blocked = @[
        @"file:///private/secret", @"content://provider/secret",
        @"data:text/html,secret", @"javascript:alert(1)", @"mailto:a@example.test",
        @"tel:12345", @"codeharbor-test:external", @"about:blank#not-owned"
    ];
    id<WKNavigationDelegate> delegate = view.navigationDelegate;
    for (NSString *url in blocked) {
        __block NSUInteger calls = 0;
        [delegate webView:view decidePolicyForNavigationAction:actionFor(url)
              decisionHandler:^(WKNavigationActionPolicy policy) {
            ++calls;
            QCOMPARE(policy, WKNavigationActionPolicyCancel);
        }];
        QCOMPARE(calls, NSUInteger(1));
        WKWebpagePreferences *preferences = [WKWebpagePreferences new];
        [delegate webView:view decidePolicyForNavigationAction:actionFor(url)
              preferences:preferences
              decisionHandler:^(WKNavigationActionPolicy policy, WKWebpagePreferences *result) {
            ++calls;
            QCOMPARE(policy, WKNavigationActionPolicyCancel);
            QVERIFY(result == preferences);
        }];
        QCOMPARE(calls, NSUInteger(2));
        [delegate webView:view decidePolicyForNavigationResponse:responseFor(url)
              decisionHandler:^(WKNavigationResponsePolicy policy) {
            ++calls;
            QCOMPARE(policy, WKNavigationResponsePolicyCancel);
        }];
        QCOMPARE(calls, NSUInteger(3));
    }
    QCOMPARE(original.unsafePolicyCalls, NSUInteger(0));

    __block WKNavigationActionPolicy result = WKNavigationActionPolicyCancel;
    [delegate webView:view decidePolicyForNavigationAction:actionFor(@"about:blank")
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyCancel);
    [delegate webView:view decidePolicyForNavigationResponse:responseFor(@"about:blank")
        decisionHandler:^(WKNavigationResponsePolicy policy) {
            QCOMPARE(policy, WKNavigationResponsePolicyAllow);
        }];
    [delegate webView:view
        decidePolicyForNavigationAction:actionFor(@"https://127.0.0.1:1/allowed")
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyAllow);
    // The internal marker ceases to be allowed after the first remote action.
    [delegate webView:view decidePolicyForNavigationAction:actionFor(marker.toString().toNSString())
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyCancel);
    [delegate webView:view decidePolicyForNavigationResponse:responseFor(@"about:blank")
        decisionHandler:^(WKNavigationResponsePolicy policy) {
            QCOMPARE(policy, WKNavigationResponsePolicyCancel);
        }];

    // Nil-target HTTP requests are cancelled as new windows and reissued in the
    // same WKWebView. The recording subclass makes network I/O impossible.
    [delegate webView:view
        decidePolicyForNavigationAction:actionFor(@"https://127.0.0.1:1/new-window", true)
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyCancel);
    QCOMPARE(QString::fromNSString(view.lastRequestedURL),
             QStringLiteral("https://127.0.0.1:1/new-window"));
    WKWebView *newView = [view.UIDelegate webView:view
        createWebViewWithConfiguration:[WKWebViewConfiguration new]
        forNavigationAction:actionFor(@"https://127.0.0.1:1/ui-window", true)
        windowFeatures:[WKWindowFeatures new]];
    QVERIFY(!newView);
    QCOMPARE(QString::fromNSString(view.lastRequestedURL),
             QStringLiteral("https://127.0.0.1:1/ui-window"));
    const auto finished = original.finishedNotifications;
    [delegate webView:view didFinishNavigation:nil];
    QCOMPARE(original.finishedNotifications, finished + 1);
    [view stopLoading];

    adapter.reset();
    QVERIFY(view.navigationDelegate == original);
    [delegate webView:view
        decidePolicyForNavigationAction:actionFor(@"https://127.0.0.1:1/after-disposal")
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyCancel);
    QCOMPARE(original.unsafePolicyCalls, NSUInteger(0));
    // Exactly Qt's identity-sensitive KVO teardown must continue to work.
    [view removeObserver:view.navigationDelegate forKeyPath:@"estimatedProgress"];
    [view removeObserver:view.navigationDelegate forKeyPath:@"title"];
    view.navigationDelegate = nil;
    [view removeFromSuperview];

    // Cancellation before the asynchronous native resource-rule compilation
    // completes must not retain a dead C++ adapter or reopen the surviving view.
    CHPolicyWebView *pendingView =
        [[CHPolicyWebView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)];
    QtWKWebViewDelegate *pendingOriginal = [QtWKWebViewDelegate new];
    pendingView.navigationDelegate = pendingOriginal;
    [pendingView addObserver:pendingOriginal forKeyPath:@"estimatedProgress" options:0 context:nil];
    [pendingView addObserver:pendingOriginal forKeyPath:@"title" options:0 context:nil];
    [root addSubview:pendingView];
    const QUrl pendingMarker(QStringLiteral("about:blank#ch-policy-")
                             + QUuid::createUuid().toString(QUuid::WithoutBraces));
    [pendingView loadRequest:[NSURLRequest requestWithURL:pendingMarker.toNSURL()]];
    QTRY_COMPARE(QString::fromNSString(pendingView.URL.absoluteString),
                 pendingMarker.toString());
    auto pendingAdapter = ch::createMobileWebViewPolicyAdapter(&window, pendingMarker);
    QVERIFY(pendingAdapter->poll().state == ch::MobileWebViewPolicyState::Pending);
    QVERIFY(pendingView.UIDelegate);
    pendingOriginal.unsafePolicyCalls = 0;
    id<WKNavigationDelegate> pendingDelegate = pendingView.navigationDelegate;
    pendingAdapter.reset();
    QTRY_VERIFY([(id<CHPolicyGuardState>)pendingView.UIDelegate rulesReady]);
    [pendingDelegate webView:pendingView
        decidePolicyForNavigationAction:actionFor(@"https://127.0.0.1:1/after-pending-disposal")
        decisionHandler:^(WKNavigationActionPolicy policy) { result = policy; }];
    QCOMPARE(result, WKNavigationActionPolicyCancel);
    QCOMPARE(pendingOriginal.unsafePolicyCalls, NSUInteger(0));
    WKWebView *pendingPopup = [pendingView.UIDelegate webView:pendingView
        createWebViewWithConfiguration:[WKWebViewConfiguration new]
        forNavigationAction:actionFor(@"https://127.0.0.1:1/after-pending-disposal", true)
        windowFeatures:[WKWindowFeatures new]];
    QVERIFY(!pendingPopup);
    QCOMPARE(QString::fromNSString(pendingView.lastRequestedURL), pendingMarker.toString());
    [pendingView removeObserver:pendingView.navigationDelegate forKeyPath:@"estimatedProgress"];
    [pendingView removeObserver:pendingView.navigationDelegate forKeyPath:@"title"];
    pendingView.navigationDelegate = nil;
    [pendingView removeFromSuperview];
}

QTEST_MAIN(MobileWebViewPolicyDarwinTest)
#include "tst_mobilewebviewpolicydarwin.moc"
