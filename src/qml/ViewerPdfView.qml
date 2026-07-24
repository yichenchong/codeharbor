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
        settings.pdfViewerEnabled: true
        settings.pluginsEnabled: true
        url: root.url.toString().length > 0
             ? viewers.internalUrlFor(root.url)
             : ""
    }
}
