import QtQuick
import QtWebEngine
import QtWebChannel

// Remote editor pane (SPEC 8.1). Hosts the TRUSTED, app-owned Monaco editor
// bundle (src/web/editor) in a WebEngineView and bridges it to the C++
// EditorController over Qt WebChannel.
//
// SECURITY (SPEC 7.2/8.1): JavaScript is ENABLED here — unlike the untrusted
// viewer WebEngineViews (image/pdf/binary) which disable it — because the page
// is our OWN code, not remote file bytes. File CONTENT never arrives as a
// navigated document; it flows in through the EditorController bridge signals
// (contentLoaded) and back out through its slots (save/reportContent). No
// codeharbor-internal:// file document is ever loaded into this view.
//
// WIRING (for main.cpp / the QML host):
//   * `editorController`  — context property: the ch::EditorController instance
//     exposed to JS under the WebChannel object name "editor".
//   * `viewers`           — context property (ViewerModel): supplies the
//     privileged WebEngine profile that permits a WebChannel bridge.
//
// Live rendering is display-deferred: `editorBundleUrl` points at the built
// bundle's HTML entry (which loads qwebchannel.js + editor.js and calls
// mountEditor(el, channel.objects.editor)). Wire it to the real qrc/app URL
// when the bundle is packaged; the WebChannel wiring below is complete.
Item {
    id: root

    // The ch::EditorController for this pane, created per-pane via the
    // `editorFactory` context property (owned by this pane); overridable for tests.
    property var controller: editorFactory.create(root)

    // URL of the trusted editor bundle entry page (app-owned). Packaged as a
    // qrc:// resource or app:// asset by the E-web build; blank until then.
    property url editorBundleUrl: ""

    // Remote file URL to open once the controller + bridge are ready.
    property url fileUrl: ""

    WebChannel {
        id: editorChannel
    }

    WebEngineView {
        id: view
        anchors.fill: parent

        // Privileged profile: permits the WebChannel bridge (the external
        // profile deliberately does not — SPEC 7.2). The bundle is trusted app
        // code, so scripting is intentionally on.
        profile: viewers.internalProfile()
        settings.javascriptEnabled: true
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false

        webChannel: editorChannel
        url: root.editorBundleUrl
    }

    // Register the controller under the EXACT object name "editor" required by
    // the frozen C3 contract (channel.objects.editor on the JS side).
    Component.onCompleted: {
        if (root.controller) {
            editorChannel.registerObject("editor", root.controller)
            if (root.fileUrl.toString().length > 0) {
                var p = decodeURIComponent(root.fileUrl.toString().replace(/^file:\/\//, ""))
                root.controller.open(p)
            }
        }
    }
}
