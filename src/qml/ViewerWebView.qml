import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine

// Web view for both direct HTTP(S) pages and an explicitly selected remote
// HTML file. HTTP(S) stays in the sandboxed external profile. A remote file is
// first mapped to codeharbor-internal:// and rendered by the privileged profile
// with scripting and file/network reach disabled; passing file:// straight to
// Chromium would read a client-machine path (SPEC 7.4).
Item {
    id: root
    property url url
    // A link clicked inside Chromium is still a viewer navigation. Reporting
    // it lets ViewerPane put that address into the pane-local history instead
    // of leaving Back and Forward aware only of address-bar submissions.
    signal navigated(url address)

    property bool remoteFile: false

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as the other viewer handlers guard it.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null
    property url internalUrl: ""
    property bool retained: false
    property string errorText: ""

    function releaseInternalUrl() {
        if (root.retained && root.viewerModel)
            root.viewerModel.releaseInternalUrl(root.internalUrl.toString());
        root.retained = false;
        root.internalUrl = "";
    }

    function retarget() {
        root.releaseInternalUrl();
        root.errorText = "";
        if (!root.remoteFile || root.url.toString().length === 0 || !root.viewerModel)
            return;
        root.internalUrl = root.viewerModel.internalUrlFor(root.url);
        root.retained = root.viewerModel.retainInternalUrl(root.internalUrl.toString());
    }

    onUrlChanged: root.retarget()
    onRemoteFileChanged: root.retarget()
    Component.onCompleted: root.retarget()
    Component.onDestruction: root.releaseInternalUrl()

    Connections {
        target: root.viewerModel
        function onInternalResourceError(internalUrl, message) {
            if (root.remoteFile && String(internalUrl) === root.internalUrl.toString())
                root.errorText = message;
        }
    }

    WebEngineView {
        id: webView
        anchors.fill: parent
        visible: root.errorText.length === 0
        profile: root.remoteFile
                 ? (root.viewerModel ? root.viewerModel.internalProfile() : null)
                 : (root.viewerModel ? root.viewerModel.externalProfile() : null)
        // Remote HTML is untrusted bytes. It may render markup, but it must not
        // execute script or use either local files or network requests.
        settings.javascriptEnabled: !root.remoteFile
        settings.localContentCanAccessFileUrls: !root.remoteFile
        settings.localContentCanAccessRemoteUrls: !root.remoteFile
        // Popups are not part of a pane's browser contract. Keeping them off
        // also prevents an external page from escaping the pane's navigation
        // and opening an uncontrolled top-level view.
        settings.javascriptCanOpenWindows: false
        onNavigationRequested: function(request) {
            // Never let a link from an external page hand Chromium a
            // client-machine file URL. Reject it before the load starts and
            // route it back through ViewerPane, which maps remote files to a
            // locked-down internal handler.
            if (!root.remoteFile
                    && request.url.toString().startsWith("file:")) {
                request.action = WebEngineNavigationRequest.IgnoreRequest
                root.navigated(request.url)
            }
        }
        url: root.remoteFile ? root.internalUrl : root.url
        // Internal remote-file loads are implementation addresses, not pane
        // history entries. External page links, however, become real viewer
        // navigations and are handed back to ViewerPane.
        onUrlChanged: {
            if (!root.remoteFile && root.url.toString() !== webView.url.toString())
                root.navigated(webView.url)
        }
        onNewWindowRequested: function(request) {
            console.warn("ViewerWebView: refused a new window for",
                         request.requestedUrl)
        }
        // Deliberately no webChannel: external pages get no privileged bridge.
    }

    // Exposed for ViewerPane's browser reload control. Keeping the WebEngineView
    // alive preserves the pane's page object while still issuing a fresh
    // request for the current address.
    function reload() {
        webView.reload();
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 48
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        textFormat: Text.PlainText
        text: root.errorText.length > 0 ? qsTr("Error: %1").arg(root.errorText) : ""
        visible: text.length > 0
        color: Theme.danger
        font.pixelSize: Theme.fontSizeLabel
    }
}
