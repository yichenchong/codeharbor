import QtQuick
import QtWebEngine

// Remote image view (SPEC 7.4/7.5). The image is loaded through the privileged
// internal scheme (codeharbor-internal://file/<id>) on the INTERNAL profile,
// which the scheme handler resolves back to file.readFile. A QML Image cannot
// resolve the WebEngine-only internal scheme, so a WebEngineView renders it.
Item {
    id: root
    property url url

    WebEngineView {
        anchors.fill: parent
        profile: viewers.internalProfile()
        // SECURITY (SPEC 7.2): these bytes are untrusted remote file content.
        // An .svg/.html served top-level could run JS and exfiltrate other
        // files, so disable scripting and any local/remote URL reach. The
        // scheme handler additionally sends a locked-down CSP for such types.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        url: root.url.toString().length > 0
             ? viewers.internalUrlFor(root.url)
             : ""
    }
}
