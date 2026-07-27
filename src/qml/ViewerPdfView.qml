import QtQuick
import QtWebEngine

// Remote PDF view (SPEC 7.5). The PDF is streamed through the internal scheme
// on the INTERNAL profile and rendered by Chromium's built-in PDF viewer.
Item {
    id: root
    property url url

    WebEngineView {
        anchors.fill: parent
        profile: viewers.internalProfile()
        // SECURITY (SPEC 7.2): untrusted remote bytes. Disable page scripting
        // and any local/remote URL reach so a malicious document cannot run JS
        // to exfiltrate other files. The built-in PDF viewer renders in its own
        // internal context and is unaffected by disabling page JavaScript.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        settings.pdfViewerEnabled: true
        settings.pluginsEnabled: true
        url: root.url.toString().length > 0
             ? viewers.internalUrlFor(root.url)
             : ""
    }
}
