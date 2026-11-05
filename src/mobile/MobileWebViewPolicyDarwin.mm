#include "MobileWebViewPolicyAdapter.h"

#include <QPointer>
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

// Qt's Darwin backend owns its navigation delegate with a manual +1 reference
// and removes its title/progress KVO registrations using that delegate's identity
// in its destructor. Replacing navigationDelegate with an ordinary proxy breaks
// that contract. A zero-ivar, per-instance runtime subclass preserves identity,
// ownership, KVO and every inherited Qt lifecycle callback. Only policy selectors
// are replaced; the original external-opening policy is NEVER called.
// This is deliberately restricted to the known Qt backend, not arbitrary views.

@class CHWebViewPolicyGuard;

namespace {
char policyAssociation;
char uiAssociation;

bool isRemoteURL(NSURL *url)
{
    if (!url)
        return false;
    const QUrl parsed = QUrl::fromEncoded(
        QString::fromNSString(url.absoluteString).toUtf8(), QUrl::StrictMode);
    return parsed.isValid() && !parsed.isRelative()
        && (parsed.scheme() == QStringLiteral("http")
            || parsed.scheme() == QStringLiteral("https"))
        && !parsed.host().isEmpty();
}

void guardedAction(id self, SEL selector, WKWebView *view,
                   WKNavigationAction *action,
                   void (^decision)(WKNavigationActionPolicy));
void guardedActionWithPreferences(id self, SEL selector, WKWebView *view,
                                 WKNavigationAction *action,
                                 WKWebpagePreferences *preferences,
                                 void (^decision)(WKNavigationActionPolicy,
                                                  WKWebpagePreferences *));
void guardedResponse(id self, SEL selector, WKWebView *view,
                     WKNavigationResponse *response,
                     void (^decision)(WKNavigationResponsePolicy));
Class guardedDelegateClass(Class original);
} // namespace

@interface CHWebViewPolicyGuard : NSObject <WKUIDelegate>
@property(nonatomic, weak) WKWebView *view;
@property(nonatomic, weak) id navigationDelegate;
@property(nonatomic, copy) NSString *marker;
@property(nonatomic, copy) NSString *failure;
@property(nonatomic) BOOL closed;
@property(nonatomic) BOOL rulesReady;
@property(nonatomic) BOOL markerAllowed;
- (BOOL)owns:(WKWebView *)view;
- (BOOL)allows:(NSURL *)url;
- (WKNavigationActionPolicy)policyFor:(WKNavigationAction *)action
                               view:(WKWebView *)view;
- (void)close;
@end

@implementation CHWebViewPolicyGuard
- (BOOL)owns:(WKWebView *)view
{
    return !self.closed && view && view == self.view
        && self.navigationDelegate
        && view.navigationDelegate == self.navigationDelegate
        && view.UIDelegate == self;
}

- (BOOL)allows:(NSURL *)url
{
    if (self.closed)
        return NO;
    if (isRemoteURL(url)) {
        if (!self.rulesReady)
            return NO;
        self.markerAllowed = NO;
        return YES;
    }
    return self.markerAllowed && url
        && [url.absoluteString isEqualToString:self.marker];
}

- (WKNavigationActionPolicy)policyFor:(WKNavigationAction *)action
                               view:(WKWebView *)view
{
    if (![self owns:view] || ![self allows:action.request.URL])
        return WKNavigationActionPolicyCancel;
    // A missing target frame is window.open()/target=_blank. Do not delegate to
    // Qt, which opens these in the OS. Its request remains in this exact view.
    if (!action.targetFrame)
        return WKNavigationActionPolicyCancel;
    return WKNavigationActionPolicyAllow;
}

- (void)close
{
    self.closed = YES;
    [self.view stopLoading];
}

- (WKWebView *)webView:(WKWebView *)view
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
              forNavigationAction:(WKNavigationAction *)action
                   windowFeatures:(WKWindowFeatures *)features
{
    Q_UNUSED(configuration);
    Q_UNUSED(features);
    if (self.rulesReady && [self owns:view] && !action.targetFrame
        && isRemoteURL(action.request.URL)) {
        self.markerAllowed = NO;
        [view loadRequest:action.request];
    }
    return nil;
}

- (void)webViewDidClose:(WKWebView *)view
{
    if (view == self.view)
        [view stopLoading];
}
@end

namespace {
CHWebViewPolicyGuard *guardFor(id delegate)
{
    return objc_getAssociatedObject(delegate, &policyAssociation);
}

void routeNewWindow(CHWebViewPolicyGuard *guard, WKWebView *view,
                    WKNavigationAction *action)
{
    if (guard.rulesReady && !action.targetFrame && [guard owns:view]
        && isRemoteURL(action.request.URL))
        [view loadRequest:action.request];
}

void guardedAction(id self, SEL selector, WKWebView *view,
                   WKNavigationAction *action,
                   void (^decision)(WKNavigationActionPolicy))
{
    Q_UNUSED(selector);
    CHWebViewPolicyGuard *guard = guardFor(self);
    const auto policy = [guard policyFor:action view:view];
    // Messaging nil returns zero (Cancel), so a missing guard fails closed.
    decision(policy);
    routeNewWindow(guard, view, action);
}

void guardedActionWithPreferences(id self, SEL selector, WKWebView *view,
                                 WKNavigationAction *action,
                                 WKWebpagePreferences *preferences,
                                 void (^decision)(WKNavigationActionPolicy,
                                                  WKWebpagePreferences *))
{
    Q_UNUSED(selector);
    CHWebViewPolicyGuard *guard = guardFor(self);
    decision([guard policyFor:action view:view], preferences);
    routeNewWindow(guard, view, action);
}

void guardedResponse(id self, SEL selector, WKWebView *view,
                     WKNavigationResponse *response,
                     void (^decision)(WKNavigationResponsePolicy))
{
    Q_UNUSED(selector);
    CHWebViewPolicyGuard *guard = guardFor(self);
    // WebKit may omit the fragment from a bootstrap response. This exception
    // applies only to a response in the already identified view, before its
    // first remote action; bare about:blank navigation actions remain denied.
    const BOOL bootstrapResponse = guard.markerAllowed
        && [response.response.URL.absoluteString isEqualToString:@"about:blank"];
    decision([guard owns:view]
                 && (bootstrapResponse || [guard allows:response.response.URL])
                 ? WKNavigationResponsePolicyAllow
                 : WKNavigationResponsePolicyCancel);
}

Class guardedDelegateClass(Class original)
{
    // One bounded runtime class, no global object registry, and no ivar changes.
    // Reject other backends rather than guessing at ownership or KVO contracts.
    static Class guarded = Nil;
    static Class base = Nil;
    if (guarded)
        return original == base ? guarded : Nil;
    if (original != objc_getClass("QtWKWebViewDelegate"))
        return Nil;
    Class candidate = objc_allocateClassPair(original, "CHRestrictedQtWKWebViewDelegate", 0);
    if (!candidate)
        return Nil;
    const SEL action = @selector(webView:decidePolicyForNavigationAction:decisionHandler:);
    const SEL preferences = @selector(webView:decidePolicyForNavigationAction:preferences:decisionHandler:);
    const SEL response = @selector(webView:decidePolicyForNavigationResponse:decisionHandler:);
    // Obtain the SDK's actual ABI encodings, rather than hard-coding pointer or
    // enum widths. The preferences selector is optional on the original Qt class.
    const auto actionDescription = protocol_getMethodDescription(
        @protocol(WKNavigationDelegate), action, NO, YES);
    const auto preferencesDescription = protocol_getMethodDescription(
        @protocol(WKNavigationDelegate), preferences, NO, YES);
    const auto responseDescription = protocol_getMethodDescription(
        @protocol(WKNavigationDelegate), response, NO, YES);
    if (!actionDescription.types || !preferencesDescription.types || !responseDescription.types
        || !class_addMethod(candidate, action, reinterpret_cast<IMP>(guardedAction),
                            actionDescription.types)
        || !class_addMethod(candidate, preferences,
                            reinterpret_cast<IMP>(guardedActionWithPreferences),
                            preferencesDescription.types)
        || !class_addMethod(candidate, response, reinterpret_cast<IMP>(guardedResponse),
                            responseDescription.types)
        || class_getInstanceSize(candidate) != class_getInstanceSize(original)) {
        objc_disposeClassPair(candidate);
        return Nil;
    }
    objc_registerClassPair(candidate);
    base = original;
    guarded = candidate;
    return guarded;
}

#if TARGET_OS_IPHONE
using NativeView = UIView;
#else
using NativeView = NSView;
#endif

void findMarker(NativeView *root, NSString *marker, WKWebView *__strong *match,
                unsigned *count)
{
    if (!root || *count > 1)
        return;
    if ([root isKindOfClass:[WKWebView class]]) {
        WKWebView *candidate = static_cast<WKWebView *>(root);
        if ([candidate.URL.absoluteString isEqualToString:marker]) {
            ++*count;
            *match = candidate;
        }
    }
    for (NativeView *child in root.subviews)
        findMarker(child, marker, match, count);
}

class DarwinWebViewPolicyAdapter final : public ch::MobileWebViewPolicyAdapter {
public:
    DarwinWebViewPolicyAdapter(QWindow *window, const QUrl &marker)
        : window_(window), marker_(marker.toString(QUrl::FullyEncoded).toNSString())
    {
    }

    ~DarwinWebViewPolicyAdapter() override
    {
        @autoreleasepool {
            [guard_ close];
        }
    }

    ch::MobileWebViewPolicyResult poll() override
    {
        using State = ch::MobileWebViewPolicyState;
        @autoreleasepool {
            if (!failure_.isEmpty())
                return {State::Failed, failure_};
            if (![NSThread isMainThread])
                return fail(QStringLiteral("Native web policy requires the Apple UI thread."));
            if (guard_) {
                if (guard_.failure.length)
                    return fail(QString::fromNSString(guard_.failure));
                if (guard_.closed || !guard_.view || !guard_.navigationDelegate
                    || guard_.view.navigationDelegate != guard_.navigationDelegate
                    || guard_.view.UIDelegate != guard_
                    || objc_getAssociatedObject(guard_.navigationDelegate, &policyAssociation) != guard_
                    || objc_getAssociatedObject(guard_.view, &uiAssociation) != guard_
                    || object_getClass(guard_.navigationDelegate)
                        != objc_getClass("CHRestrictedQtWKWebViewDelegate"))
                    return fail(QStringLiteral("The native web navigation guard was detached."));
                if (!guard_.rulesReady)
                    return {};
                return {State::Ready, {}};
            }
            if (!window_)
                return fail(QStringLiteral("The native web view window was destroyed."));
            NativeView *native = (__bridge NativeView *)(reinterpret_cast<void *>(window_->winId()));
            if (!native)
                return {};
#if TARGET_OS_IPHONE
            NativeView *root = native.window;
#else
            NativeView *root = native.window.contentView;
#endif
            if (!root)
                return {};
            WKWebView *match = nil;
            unsigned matches = 0;
            findMarker(root, marker_, &match, &matches);
            if (matches > 1)
                return fail(QStringLiteral("The native web view setup marker was not unique."));
            if (!match)
                return {};
            if (match.UIDelegate || objc_getAssociatedObject(match, &uiAssociation))
                return fail(QStringLiteral("The native web view already has an unsupported UI delegate."));
            id delegate = match.navigationDelegate;
            Class subclass = delegate ? guardedDelegateClass(object_getClass(delegate)) : Nil;
            if (!subclass)
                return fail(QStringLiteral("The native web backend cannot enforce navigation policy."));

            CHWebViewPolicyGuard *guard = [CHWebViewPolicyGuard new];
            guard.view = match;
            guard.navigationDelegate = delegate;
            guard.marker = marker_;
            guard.markerAllowed = YES;
            guard.closed = NO;
            guard_ = guard;
            // Retain by both native objects: Qt's existing delegate release and
            // view destruction remain valid in either QObject teardown order.
            objc_setAssociatedObject(delegate, &policyAssociation, guard,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            objc_setAssociatedObject(match, &uiAssociation, guard,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            if (object_setClass(delegate, subclass) != class_getSuperclass(subclass))
                return fail(QStringLiteral("The native navigation delegate changed during setup."));
            // WebKit caches optional-selector availability in this setter. The
            // same object must be assigned again after changing its class.
            match.navigationDelegate = delegate;
            match.UIDelegate = guard;
            if (match.navigationDelegate != delegate || match.UIDelegate != guard)
                return fail(QStringLiteral("The native navigation guard could not be installed."));

            // Public WebKit resource filtering complements the navigation gate.
            // No private allowFileAccessFromFileURLs/allowUniversalAccess... KVC.
            // Qt's public allowFileAccess/localContentCanAccessFileUrls settings
            // are also disabled by the common page before assigning any URL.
            NSString *identifier = [@"ch-policy-" stringByAppendingString:[NSUUID UUID].UUIDString];
            // Use the common content-blocker regex subset: no alternation.
            NSString *rules = @"[{\"trigger\":{\"url-filter\":\"^file:\","
                               "\"url-filter-is-case-sensitive\":false},"
                               "\"action\":{\"type\":\"block\"}},"
                               "{\"trigger\":{\"url-filter\":\"^content:\","
                               "\"url-filter-is-case-sensitive\":false},"
                               "\"action\":{\"type\":\"block\"}}]";
            WKContentRuleListStore *store = [WKContentRuleListStore defaultStore];
            [store compileContentRuleListForIdentifier:identifier
                               encodedContentRuleList:rules
                                    completionHandler:^(WKContentRuleList *list, NSError *error) {
                // Only native retained state is captured; never the C++ adapter.
                // Completion is delivered on the main thread by WebKit.
                if (!list || error) {
                    guard.failure = @"WebKit could not disable local-file resources.";
                    [guard close];
                } else if (guard.view && guard.view.navigationDelegate == guard.navigationDelegate
                           && guard.view.UIDelegate == guard) {
                    [guard.view.configuration.userContentController addContentRuleList:list];
                    // A disposed session stays closed even if compilation finishes
                    // later. rulesReady does not itself reopen a closed guard.
                    guard.rulesReady = YES;
                } else {
                    guard.failure = @"The native web view disappeared during policy setup.";
                }
                [store removeContentRuleListForIdentifier:identifier completionHandler:^(NSError *removeError) {
                    Q_UNUSED(removeError);
                }];
            }];
            return {};
        }
    }

private:
    ch::MobileWebViewPolicyResult fail(const QString &message)
    {
        failure_ = message;
        [guard_ close];
        return {ch::MobileWebViewPolicyState::Failed, failure_};
    }

    QPointer<QWindow> window_;
    NSString *__strong marker_;
    CHWebViewPolicyGuard *__strong guard_;
    QString failure_;
};
} // namespace

namespace ch {
std::unique_ptr<MobileWebViewPolicyAdapter> createMobileWebViewPolicyAdapter(
    QWindow *window, const QUrl &marker)
{
    auto adapter = std::make_unique<DarwinWebViewPolicyAdapter>(window, marker);
    adapter->poll();
    return adapter;
}
} // namespace ch
