import QtQuick
import QtWebEngine

// External web view (SPEC 7.2/7.3): arbitrary http/https pages run on the
// sandboxed EXTERNAL profile with persistent cookies/localStorage but NO
// WebChannel bridge and no remote-file or internal-API access.
Item {
    id: root
    property url url

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as ViewerPane and EditorPaneView guard it, and for the
    // same reason: an unguarded lookup of a context property the host did not
    // install THROWS a ReferenceError, and that aborts the whole binding pass
    // building this view rather than producing an inert one. A null profile is
    // what EditorPaneView degrades to as well: the default profile, with none
    // of the two named ones' settings.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    WebEngineView {
        anchors.fill: parent
        profile: root.viewerModel ? root.viewerModel.externalProfile() : null
        url: root.url
        // Deliberately no webChannel: external pages get no privileged bridge.
    }
}
