import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import QtWebChannel
import "RemotePath.js" as RemotePath

// Rendered Markdown handler. The page itself is trusted app code on the
// privileged INTERNAL profile, while the fetched document is hostile content:
// markdown.js sanitises it before insertion, and this view keeps file/network
// access disabled except for the controlled codeharbor-internal scheme. The
// external profile is deliberately not used because it has no internal scheme
// handler and no way to fetch the server-side document.
Item {
    id: root

    property url url
    property url markdownBundleUrl: "qrc:/codeharbor/web/markdown/index.html"
    signal navigated(url address)

    readonly property var viewerModel:
        (typeof viewers !== "undefined") ? viewers : null
    property url internalUrl: ""
    property bool retained: false
    // Internal address -> true, for every subresource address this view has
    // pinned for the document it is currently showing.
    //
    // A MAP, not a list. applyTheme() re-renders the document (deliberately —
    // see index.ts — so a live theme change never creates a second unsanitised
    // insertion path), the re-render re-requests EVERY image, and a list
    // appended to with concat grew by the whole image count on every theme
    // change: one more entry held, and one more pin taken on each address, for
    // as long as the document stayed open. Keyed by address, one retain is
    // taken per distinct address per document and exactly one release is owed.
    property var retainedSubresources: ({})
    property string errorText: ""
    property bool pageReady: false
    // The renderer page has started loading and has not settled yet, read by
    // the pane header's `busy`. Distinct from `pageReady`, which is about
    // whether the page can be SCRIPTED: a failed load leaves that false for
    // ever, and a header bound to it would spin for ever with it.
    property bool loading: false

    readonly property string remotePath: root.url.toString().length > 0
        ? RemotePath.fileUrlToPath(root.url.toString()) : ""

    function fileUrlFor(path) {
        return RemotePath.pathToFileUrl(path)
    }

    // ---- resolving a document-relative image reference ---------------------
    //
    // DUPLICATED, DELIBERATELY, and kept in step by hand. The same two rules —
    // "is this reference relative" and "resolve it against the document's
    // directory with POSIX `..` semantics" — are implemented in TypeScript in
    // src/web/markdown/src/renderer.ts (isRelativeResource / pathDirectory /
    // resolveRemotePath), which is the authority for the rendered page's own
    // link rewriting.
    //
    // QML cannot call into that module: it is bundled JavaScript running in a
    // separate Chromium world, and the only channel between them is the
    // WebChannel bridge, whose frozen contract passes the RAW document-relative
    // string. So this side has to resolve it too, and it resolves it against
    // ITS OWN `remotePath` rather than trusting anything the page computed.
    //
    // The pair must agree. tst_paneidentity's
    // theMarkdownImagePathResolverAgreesWithTheRendererBundle pins the shared
    // table; test/renderer.test.ts pins the same cases on the other side.
    //
    // TWO deliberate divergences, both marked below and both in the same
    // direction — this side refuses where the other side answers, because its
    // answer is fed to a remote file READ and the other side's is used as a
    // link target:
    //
    //   * a reference that climbs to the filesystem root. resolveRemotePath()
    //     answers "/"; this answers "".
    //   * no document to resolve against. resolveRemotePath() treats an empty
    //     document path as "/" and answers "/a.png"; this answers "". The page
    //     cannot reach that state at all — its document path is the `?path=`
    //     query this very file wrote — and this view can, because `remotePath`
    //     is empty until a url arrives.

    // renderer.ts: isRelativeResource(). A "#" or "?" prefix is a same-document
    // reference, not a resource — and it is not merely uninteresting here, it
    // is harmful: everything before the first "?" or "#" is stripped below, so
    // "#anchor" would otherwise resolve to the document's own DIRECTORY and
    // this view would mint a file read for it.
    function isRelativeResource(value) {
        return value.length > 0
            && !/^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(value)
            && value.charAt(0) !== "#"
            && value.charAt(0) !== "?"
    }

    // renderer.ts: pathDirectory(), except for DIVERGENCE 2. "" — and only "" —
    // means there is no document to resolve against; pathDirectory() would
    // answer "/" and resolve against the filesystem root. This view can reach
    // that state and the page cannot: `remotePath` is empty until a url
    // arrives, while the page's document path is the `?path=` query navigate()
    // wrote below.
    function documentDirectory() {
        const path = String(root.remotePath).split(/[?#]/, 1)[0]
        if (path.length === 0)
            return ""
        if (path.charAt(path.length - 1) === "/")
            return path.substring(0, path.length - 1) || "/"
        const separator = path.lastIndexOf("/")
        if (separator < 0)
            return "/"
        return path.substring(0, separator) || "/"
    }

    // renderer.ts: resolveRemotePath(), plus the two refusals above.
    function resolveImagePath(relativePath) {
        const value = String(relativePath === undefined || relativePath === null
                             ? "" : relativePath)
        if (!root.isRelativeResource(value))
            return ""
        const directory = root.documentDirectory()
        if (directory.length === 0)
            return ""
        const candidate = value.split(/[?#]/, 1)[0]
        if (candidate.length === 0)
            return ""
        const combined = candidate.charAt(0) === "/"
            ? candidate : directory + "/" + candidate
        const parts = []
        for (const part of combined.split("/")) {
            if (part.length === 0 || part === ".")
                continue
            if (part === "..") {
                // Clamped at the server root, exactly as resolveRemotePath()
                // clamps it: ".." can never climb above "/".
                if (parts.length > 0)
                    parts.pop()
                continue
            }
            parts.push(part)
        }
        // DIVERGENCE 1. resolveRemotePath() answers "/" here, which is a
        // perfectly good LINK target. It is not a file, and this function's
        // answer is fed straight to a remote read, so refusing is the honest
        // reply; "" is the page's documented "no image" value.
        if (parts.length === 0)
            return ""
        return "/" + parts.join("/")
    }

    function releaseInternalUrl() {
        if (root.viewerModel) {
            for (const subresource in root.retainedSubresources)
                root.viewerModel.releaseInternalUrl(subresource)
        }
        root.retainedSubresources = ({})
        if (root.retained && root.viewerModel)
            root.viewerModel.releaseInternalUrl(root.internalUrl.toString())
        root.retained = false
        root.internalUrl = ""
    }

    function retarget() {
        root.releaseInternalUrl()
        root.errorText = ""
        root.pageReady = false
        if (root.url.toString().length === 0 || !root.viewerModel)
            return
        root.internalUrl = root.viewerModel.internalUrlFor(root.url)
        root.retained = root.viewerModel.retainInternalUrl(root.internalUrl.toString())
    }

    function navigate() {
        root.pageReady = false
        if (root.internalUrl.toString().length === 0
                || root.markdownBundleUrl.toString().length === 0) {
            root.loading = false
            webView.url = ""
            return
        }
        const base = root.markdownBundleUrl.toString()
        webView.url = base + "?source="
            + encodeURIComponent(root.internalUrl.toString())
            + "&path=" + encodeURIComponent(root.remotePath)
    }

    function applyTheme() {
        if (!root.pageReady)
            return
        const roles = JSON.stringify(Theme.roles)
        // Theme changes re-render the markdown document. Preserve the user's
        // reading position instead of jumping back to the beginning whenever
        // the palette changes.
        webView.runJavaScript(
            "(function () {"
          + "var y = window.scrollY || document.documentElement.scrollTop || 0;"
          + "if (typeof window.applyTheme === 'function') window.applyTheme("
          + roles + ");"
          + "window.requestAnimationFrame(function () { window.scrollTo(0, y); });"
          + "})()")
    }

    onUrlChanged: {
        root.retarget()
        root.navigate()
    }
    onMarkdownBundleUrlChanged: root.navigate()
    Component.onCompleted: {
        root.retarget()
        root.navigate()
    }
    Component.onDestruction: root.releaseInternalUrl()

    // Only the narrow image capability is exposed to page JavaScript. The page
    // supplies a relative path from the current document; QML resolves it on
    // the remote server, mints an opaque internal URL, and pins that URL until
    // the page is retargeted or destroyed. There is no general read method.
    //
    // "" is the refusal, and the page understands it: requestImageUrl() in
    // src/web/markdown/src/bridge.ts settles on "" for a bridge that refuses,
    // throws or never answers, and renderCurrent() only sets an `src` for an
    // address that starts with "codeharbor-internal://file/". So every failure
    // here can be — and is — reported as "" rather than as a plausible-looking
    // address the page would try to load.
    QtObject {
        id: markdownBridge
        objectName: "markdownImageBridge"
        function resolveImage(relativePath) {
            const path = root.resolveImagePath(relativePath)
            if (path.length === 0 || !root.viewerModel)
                return ""
            const mapped = String(root.viewerModel.internalUrlFor(root.fileUrlFor(path)))
            if (mapped.length === 0)
                return ""
            // Already pinned for this document: hand back the same address
            // without taking a second pin. The page asks again for every image
            // on every theme-driven re-render, and each extra retain is one
            // more release this view would owe and never make.
            if (root.retainedSubresources[mapped] === true)
                return mapped
            if (!root.viewerModel.retainInternalUrl(mapped)) {
                // Unknown or malformed (ViewerModel.h): nothing was pinned, so
                // the address can be evicted while the page is still fetching
                // it. Handing it over anyway would produce an image that loads
                // sometimes; "" produces one that visibly does not.
                return ""
            }
            root.retainedSubresources[mapped] = true
            return mapped
        }
    }

    WebChannel {
        id: markdownChannel
        Component.onCompleted: markdownChannel.registerObject("markdown", markdownBridge)
    }

    WebEngineView {
        id: webView
        anchors.fill: parent
        visible: root.errorText.length === 0

        // This is the app-owned renderer page, not the raw Markdown document.
        // It needs the INTERNAL profile for the controlled custom scheme and
        // WebChannel. The sanitizer is the boundary between its script and the
        // untrusted fetched bytes.
        profile: root.viewerModel ? root.viewerModel.internalProfile() : null
        settings.javascriptEnabled: true
        settings.localContentCanAccessFileUrls: false
        // SECURITY — THIS SWITCHES A WHOLE BROWSER PROTECTION OFF, and it says
        // so here rather than reading as a small permission.
        //
        // WHAT IS DISABLED: Chromium's cross-origin enforcement for this
        // locally-loaded document. With it on, a page served from qrc: may
        // fetch nothing but its own origin; with it off, this page may issue
        // requests to ANY origin, the open network included.
        //
        // WHY IT CANNOT BE AVOIDED: the renderer page is app-owned code loaded
        // out of the binary's own resources, and the Markdown document it has
        // to render — plus every image in it — is served through the
        // codeharbor-internal:// scheme, which Chromium counts as a DIFFERENT
        // origin from the page. Leaving the enforcement on blocks the very
        // fetch this view exists to make, and there is no same-origin way to
        // hand a server-side document to the page (its content deliberately
        // never arrives as a navigated document).
        //
        // WHAT IS LEFT GUARDING IT: nothing in this file. The ONLY remaining
        // barrier between this privileged page and the network is the
        // Content-Security-Policy meta tag in src/web/markdown/index.html,
        // which permits the custom internal scheme and denies everything else.
        // The two are a PAIR: loosening that policy — or dropping it — silently
        // turns this line into "the rendering page may talk to the internet",
        // with a WebChannel image bridge and remote file reads on the other
        // side of it. A test pins that policy; keep it pinned.
        //
        // Client file:// access stays denied above, and is a separate rule.
        settings.localContentCanAccessRemoteUrls: true
        settings.javascriptCanOpenWindows: false
        webChannel: markdownChannel

        onLoadingChanged: function(request) {
            root.loading = request.status === WebEngineView.LoadStartedStatus
            if (request.status === WebEngineView.LoadSucceededStatus) {
                root.pageReady = true
                root.applyTheme()
            } else if (request.status === WebEngineView.LoadStartedStatus
                       || request.status === WebEngineView.LoadFailedStatus
                       || request.status === WebEngineView.LoadStoppedStatus) {
                root.pageReady = false
                // The renderer page itself failing to load leaves nothing on
                // screen at all. internalResourceError only covers refusals of
                // the DOCUMENT this page then fetches, so its wording wins when
                // it has already arrived.
                if (request.status === WebEngineView.LoadFailedStatus
                        && root.errorText.length === 0)
                    root.errorText = request.errorString
            }
        }

        // The bundle is pinned to one qrc document. A click in rendered
        // Markdown is routed through this signal rather than navigating the
        // privileged page itself. ViewerPane then handles file:// as remote
        // navigation and http(s) by switching to its sandboxed web handler.
        onNavigationRequested: function(request) {
            const allowed = root.markdownBundleUrl.toString().split(/[?#]/, 1)[0]
            const candidate = String(request.url).split(/[?#]/, 1)[0]
            if (allowed.length > 0 && candidate === allowed)
                return
            const address = String(request.url)
            if (/^(?:https?:|file:)/i.test(address))
                root.navigated(request.url)
            request.action = WebEngineNavigationRequest.IgnoreRequest
        }

        onNewWindowRequested: function(request) {
            console.warn("ViewerMarkdownView: refused a new window for",
                         request.requestedUrl)
        }
    }

    function ownsFailure(candidate) {
        return root.internalUrl.toString().length > 0
               && String(candidate) === root.internalUrl.toString()
    }

    Connections {
        target: root.viewerModel
        function onInternalResourceError(internalUrl, message) {
            if (root.ownsFailure(internalUrl))
                root.errorText = message
        }
    }

    Label {
        objectName: "markdownStatus"
        anchors.centerIn: parent
        width: parent.width - 48
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        textFormat: Text.PlainText
        visible: root.errorText.length > 0
        text: root.errorText.length > 0
            ? qsTr("Error: %1").arg(root.errorText) : ""
        color: Theme.danger
        font.pixelSize: Theme.fontSizeLabel
    }

    Connections {
        target: Theme
        function onRolesChanged() { root.applyTheme() }
    }
}
