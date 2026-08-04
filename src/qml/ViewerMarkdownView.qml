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
    property var retainedSubresources: []
    property string errorText: ""
    property bool pageReady: false

    readonly property string remotePath: root.url.toString().length > 0
        ? RemotePath.fileUrlToPath(root.url.toString()) : ""

    function fileUrlFor(path) {
        return RemotePath.pathToFileUrl(path)
    }

    function resolveImagePath(relativePath) {
        const value = String(relativePath || "")
        if (value.length === 0 || /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(value))
            return ""
        const withoutFragment = value.split(/[?#]/, 1)[0]
        const base = root.remotePath.endsWith("/")
            ? root.remotePath.substring(0, root.remotePath.length - 1)
            : root.remotePath.substring(0, root.remotePath.lastIndexOf("/"))
        const combined = withoutFragment.charAt(0) === "/"
            ? withoutFragment : base + "/" + withoutFragment
        const parts = []
        for (const part of combined.split("/")) {
            if (part.length === 0 || part === ".")
                continue
            if (part === "..") {
                if (parts.length > 0)
                    parts.pop()
                continue
            }
            parts.push(part)
        }
        return "/" + parts.join("/")
    }

    function releaseInternalUrl() {
        if (root.viewerModel) {
            for (const subresource of root.retainedSubresources) {
                root.viewerModel.releaseInternalUrl(String(subresource))
            }
        }
        root.retainedSubresources = []
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
        webView.runJavaScript("if (typeof window.applyTheme === 'function')"
                              + " window.applyTheme(" + roles + ");")
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
    QtObject {
        id: markdownBridge
        function resolveImage(relativePath) {
            const path = root.resolveImagePath(relativePath)
            if (path.length === 0 || !root.viewerModel)
                return ""
            const mapped = root.viewerModel.internalUrlFor(root.fileUrlFor(path))
            if (root.viewerModel.retainInternalUrl(mapped.toString())) {
                root.retainedSubresources = root.retainedSubresources.concat([
                    mapped.toString()
                ])
            }
            return mapped.toString()
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
        // Fetching codeharbor-internal:// is explicitly required for the
        // server document and images. CSP below allows only that custom scheme;
        // client file:// and arbitrary network origins remain denied.
        settings.localContentCanAccessRemoteUrls: true
        settings.javascriptCanOpenWindows: false
        webChannel: markdownChannel

        onLoadingChanged: function(request) {
            if (request.status === WebEngineView.LoadSucceededStatus) {
                root.pageReady = true
                root.applyTheme()
            } else if (request.status === WebEngineView.LoadStartedStatus
                       || request.status === WebEngineView.LoadFailedStatus
                       || request.status === WebEngineView.LoadStoppedStatus) {
                root.pageReady = false
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
