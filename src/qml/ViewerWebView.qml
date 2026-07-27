import QtQuick
import QtWebEngine

// External web view (SPEC 7.2/7.3): arbitrary http/https pages run on the
// sandboxed EXTERNAL profile with persistent cookies/localStorage but NO
// WebChannel bridge and no remote-file or internal-API access.
Item {
    id: root
    property url url

    WebEngineView {
        anchors.fill: parent
        profile: viewers.externalProfile()
        url: root.url
        // Deliberately no webChannel: external pages get no privileged bridge.
    }
}
