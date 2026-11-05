package dev.codeharbor.mobile;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.GeolocationPermissions;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.concurrent.atomic.AtomicBoolean;

/** One marker-owned Qt WebView; all native view access runs on the Android UI thread. */
public final class GuardedWebView {
    public static final int PENDING = 0;
    public static final int READY = 1;
    public static final int FAILED = 2;
    private static final byte[] EMPTY_BYTES = new byte[0];

    private final WeakReference<Activity> activity;
    private final String marker;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AtomicBoolean queued = new AtomicBoolean();
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile int state = PENDING;
    private volatile String error = "";
    // UI-thread-only references. Never retain an Activity or view in a registry.
    private WeakReference<WebView> ownedView = new WeakReference<>(null);
    private WebViewClient qtClient;
    private WebChromeClient qtChrome;
    private GuardClient guardClient;
    private GuardChrome guardChrome;
    private final Runnable check = this::checkOnUiThread;
    private final View.OnAttachStateChangeListener attachment =
            new View.OnAttachStateChangeListener() {
                @Override public void onViewAttachedToWindow(View view) {}
                @Override public void onViewDetachedFromWindow(View view) {
                    // A Qt native view must not keep callbacks to an abandoned owner.
                    close();
                }
            };

    public GuardedWebView(Context context, String setupMarker) {
        activity = new WeakReference<>(context instanceof Activity ? (Activity) context : null);
        marker = setupMarker;
        if (marker == null || !marker.matches("about:blank#ch-policy-[0-9a-fA-F-]{36}")) {
            fail("Invalid native web-view setup marker.");
        } else if (!(context instanceof Activity)) {
            fail("The Android activity is unavailable for native web-view protection.");
        } else {
            requestCheck();
        }
    }

    // Called from the Qt GUI thread; JNI never blocks waiting for Android UI work.
    public void requestCheck() {
        if (state == FAILED || closed.get() || !queued.compareAndSet(false, true))
            return;
        if (!handler.post(check)) {
            queued.set(false);
            fail("The Android UI thread refused native web-view protection.");
        }
    }

    public int getState() { return closed.get() ? FAILED : state; }
    public String getError() { return error; }

    public void close() {
        // Policy callbacks see cancellation immediately, even before queued teardown.
        if (!closed.compareAndSet(false, true))
            return;
        if (state != FAILED)
            fail("The protected Android web view was detached or closed.");
        handler.removeCallbacks(check);
        if (Looper.myLooper() == Looper.getMainLooper())
            closeOnUiThread();
        else
            handler.post(this::closeOnUiThread);
    }

    private void checkOnUiThread() {
        queued.set(false);
        if (closed.get() || state == FAILED)
            return;
        if (state == READY)
            verifyInstalled();
        else
            findAndInstall();
    }

    private void verifyInstalled() {
        Activity owner = activity.get();
        WebView view = ownedView.get();
        try {
            if (owner == null || owner.isFinishing() || owner.isDestroyed()
                    || view == null || !view.isAttachedToWindow()) {
                fail("The protected Android web view is no longer attached to its activity.");
            } else {
                WebSettings settings = view.getSettings();
                if (view.getWebViewClient() == guardClient && view.getWebChromeClient() == guardChrome
                        && !settings.getAllowFileAccess() && !settings.getAllowContentAccess()
                        && !settings.getAllowFileAccessFromFileURLs()
                        && !settings.getAllowUniversalAccessFromFileURLs()
                        && !settings.supportMultipleWindows()
                        && !settings.getJavaScriptCanOpenWindowsAutomatically())
                    return;
                fail("Android native web-view protections were replaced or disabled.");
            }
        } catch (RuntimeException exception) {
            fail("The protected Android web view is no longer available.");
        }
        closeOnUiThread();
    }

    private void findAndInstall() {
        if (closed.get() || state != PENDING || guardClient != null)
            return;
        Activity owner = activity.get();
        if (owner == null || owner.isFinishing() || owner.isDestroyed()) {
            fail("The Android activity closed before web-view protection was installed.");
            return;
        }
        try {
            // Search only this activity's public view tree; never guess a global window.
            ArrayDeque<View> remaining = new ArrayDeque<>();
            remaining.add(owner.getWindow().getDecorView());
            WebView found = null;
            while (!remaining.isEmpty()) {
                View candidate = remaining.removeLast();
                if (candidate instanceof WebView && marker.equals(((WebView) candidate).getUrl())) {
                    if (found != null) {
                        fail("The native web-view setup marker is not unique.");
                        return;
                    }
                    found = (WebView) candidate;
                }
                if (candidate instanceof ViewGroup) {
                    ViewGroup group = (ViewGroup) candidate;
                    for (int index = 0; index < group.getChildCount(); ++index)
                        remaining.add(group.getChildAt(index));
                }
            }
            if (found == null)
                return; // Common owner polls with a bounded installation deadline.
            ownedView = new WeakReference<>(found);
            qtClient = found.getWebViewClient();
            qtChrome = found.getWebChromeClient();
            if (qtClient == null || qtChrome == null) {
                fail("The Android web-view backend has no Qt callback clients.");
                closeOnUiThread();
                return;
            }
            guardClient = new GuardClient();
            guardChrome = new GuardChrome();
            // The only document is still our exact about:blank marker. Install both
            // guards and settings in this one UI job before publishing READY.
            found.setWebViewClient(guardClient);
            found.setWebChromeClient(guardChrome);
            WebSettings settings = found.getSettings();
            settings.setAllowFileAccess(false);
            settings.setAllowContentAccess(false);
            settings.setAllowFileAccessFromFileURLs(false);
            settings.setAllowUniversalAccessFromFileURLs(false);
            // Android's documented false behavior: target=_blank/window.open become
            // top-level navigations in this view, subject to our GuardClient.
            settings.setSupportMultipleWindows(false);
            settings.setJavaScriptCanOpenWindowsAutomatically(false);
            found.addOnAttachStateChangeListener(attachment);
            if (closed.get()) {
                closeOnUiThread();
                return;
            }
            if (found.getWebViewClient() != guardClient || found.getWebChromeClient() != guardChrome
                    || settings.getAllowFileAccess() || settings.getAllowContentAccess()
                    || settings.getAllowFileAccessFromFileURLs()
                    || settings.getAllowUniversalAccessFromFileURLs()
                    || settings.supportMultipleWindows()
                    || settings.getJavaScriptCanOpenWindowsAutomatically()) {
                fail("Android did not retain the native web-view protections.");
                closeOnUiThread();
                return;
            }
            // Finish one known marker load under the installed callbacks before
            // READY. Qt's bootstrap frame/loading accounting must not race the
            // first real page (an interrupted marker may not emit pageFinished).
            found.loadUrl(marker);
        } catch (RuntimeException exception) {
            fail("Could not install Android web-view protection: " + exception.getClass().getSimpleName());
            closeOnUiThread();
        }
    }

    private void fail(String reason) {
        error = reason;
        state = FAILED;
    }

    private boolean active() { return !closed.get() && state == READY; }

    private boolean callbacksAllowed() { return !closed.get() && state != FAILED; }
    private boolean permitted(String url) {
        if (!callbacksAllowed())
            return false;
        NavigationDecision.Result decision = NavigationDecision.decide(url, marker, state == PENDING);
        return decision == NavigationDecision.Result.BOOTSTRAP
                || (active() && decision == NavigationDecision.Result.REMOTE);
    }

    private void closeOnUiThread() {
        WebView view = ownedView.get();
        qtClient = null;
        qtChrome = null;
        if (view == null)
            return;
        view.removeOnAttachStateChangeListener(attachment);
        // Never restore Qt's external-opening client after an untrusted document.
        // Both replacement callbacks remain permanently denying once closed/failed.
        try {
            if (guardClient == null)
                guardClient = new GuardClient();
            if (guardChrome == null)
                guardChrome = new GuardChrome();
            view.setWebViewClient(guardClient);
            view.setWebChromeClient(guardChrome);
            view.stopLoading();
            WebSettings settings = view.getSettings();
            settings.setBlockNetworkLoads(true);
            settings.setJavaScriptEnabled(false);
            settings.setAllowFileAccess(false);
            settings.setAllowContentAccess(false);
            settings.setAllowFileAccessFromFileURLs(false);
            settings.setAllowUniversalAccessFromFileURLs(false);
            settings.setSupportMultipleWindows(false);
            settings.setJavaScriptCanOpenWindowsAutomatically(false);
        } catch (RuntimeException exception) {
            // Qt may already have destroyed this native WebView. The wrappers also
            // check closed/state before dispatching any retained native callback.
        }
    }

    private static WebResourceResponse blockedResource() {
        return new WebResourceResponse("text/plain", "UTF-8", 403, "Blocked", null,
                new ByteArrayInputStream(EMPTY_BYTES));
    }

    private final class GuardClient extends WebViewClient {
        @Override public boolean shouldOverrideUrlLoading(WebView view, String url) {
            // Do NOT delegate even HTTP-looking input: Qt's URLUtil disagreement
            // could otherwise reach Intent.ACTION_VIEW for malformed addresses.
            return !permitted(url);
        }

        @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            return !permitted(request.getUrl().toString());
        }

        @Override public WebResourceResponse shouldInterceptRequest(WebView view, String url) {
            return permitted(url) ? null : blockedResource();
        }

        @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            String url = request.getUrl().toString();
            boolean inline = active()
                    && NavigationDecision.allowInlineResource(url, request.isForMainFrame());
            return permitted(url) || inline ? null : blockedResource();
        }

        @Override public void onPageStarted(WebView view, String url, Bitmap icon) {
            if (callbacksAllowed() && qtClient != null)
                qtClient.onPageStarted(view, url, icon);
        }

        @Override public void onPageFinished(WebView view, String url) {
            if (callbacksAllowed() && qtClient != null)
                qtClient.onPageFinished(view, url);
            if (!closed.get() && state == PENDING && marker.equals(url))
                state = READY;
        }

        @Override public void onLoadResource(WebView view, String url) {
            if (callbacksAllowed() && qtClient != null)
                qtClient.onLoadResource(view, url);
        }

        @Override public void onReceivedError(WebView view, int code, String description, String url) {
            if (callbacksAllowed() && qtClient != null)
                qtClient.onReceivedError(view, code, description, url);
            if (!closed.get() && state == PENDING && marker.equals(url)) {
                fail("The protected Android web-view bootstrap did not finish.");
                closeOnUiThread();
            }
        }
    }

    private final class GuardChrome extends WebChromeClient {
        @Override public void onProgressChanged(WebView view, int progress) {
            if (callbacksAllowed() && qtChrome != null)
                qtChrome.onProgressChanged(view, progress);
        }

        @Override public void onReceivedTitle(WebView view, String title) {
            if (callbacksAllowed() && qtChrome != null)
                qtChrome.onReceivedTitle(view, title);
        }

        @Override public void onReceivedIcon(WebView view, Bitmap icon) {
            if (callbacksAllowed() && qtChrome != null)
                qtChrome.onReceivedIcon(view, icon);
        }

        @Override public void onGeolocationPermissionsShowPrompt(String origin,
                GeolocationPermissions.Callback callback) {
            if (active() && qtChrome != null)
                qtChrome.onGeolocationPermissionsShowPrompt(origin, callback);
            else
                callback.invoke(origin, false, false);
        }

        @Override public boolean onCreateWindow(WebView view, boolean dialog,
                boolean userGesture, Message transport) {
            // With supportMultipleWindows(false), allowed links have already been
            // routed to this view. Never forward a backend request for another one.
            return false;
        }
    }
}
