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
// BUNDLE URL: `editorBundleUrl` points at the packaged page
// qrc:/codeharbor/web/editor/index.html, embedded into the codeharbor_qml
// module's resources by src/qml/CMakeLists.txt from src/web/editor/dist (built
// by `npm run build --workspace src/web/editor`). qrc: resolves identically in
// the build tree, in a relocated install and inside a macOS bundle, so no
// filesystem path is ever plumbed through QML. The page loads Qt's own
// qrc:///qtwebchannel/qwebchannel.js plus the bundle, then calls
// mountEditor(el, channel.objects.editor).
//
// The pane's remote path rides along as a `path` query parameter (NOT the file
// content — that only ever arrives over the bridge). The bundle uses it solely
// to choose a Monaco syntax-highlighting language, since the frozen C3 contract
// carries no path.
Item {
    id: root

    // The ch::EditorController for this pane, created per-pane via the
    // `editorFactory` context property (owned by this pane); overridable for tests.
    property var controller: editorFactory.create(root)

    // Entry page of the trusted editor bundle. Embedded into this QML module's
    // resources by src/qml/CMakeLists.txt; overridable by a host or test that
    // wants to serve the bundle from elsewhere.
    property url editorBundleUrl: "qrc:/codeharbor/web/editor/index.html"

    // Remote file URL to open once the controller + bridge are ready.
    property url fileUrl: ""

    // `fileUrl` as the plain remote path the RPC layer speaks (SPEC 8.3):
    // file:// inside CodeHarbor always means the remote SSH server.
    readonly property string remotePath: fileUrl.toString().length > 0
        ? decodeURIComponent(fileUrl.toString().replace(/^file:\/\//, ""))
        : ""

    // Set once Component.onCompleted has registered the bridge object, so a
    // property change during initial binding evaluation cannot navigate early.
    property bool started: false

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
        // NOT bound to editorBundleUrl: navigation is driven by navigate()
        // below so it can never start before registerObject("editor").
    }

    // Navigate the view at the bundle entry, passing the pane's remote path as
    // a query parameter. The path is metadata only — a language hint for the
    // bundle; file CONTENT never reaches this view as a document, it arrives
    // over the bridge (SPEC 7.2/8.1).
    function navigate() {
        const base = root.editorBundleUrl.toString()
        if (base.length === 0) {
            view.url = ""
            return
        }
        view.url = root.remotePath.length > 0
            ? base + "?path=" + encodeURIComponent(root.remotePath)
            : base
    }

    function start() {
        if (root.controller && root.remotePath.length > 0)
            root.controller.open(root.remotePath)
        root.navigate()
    }

    // A pane reused for another file reopens the controller and reloads the
    // page so the language hint follows the new path.
    onFileUrlChanged: if (root.started) root.start()
    onEditorBundleUrlChanged: if (root.started) root.navigate()

    // Register the controller under the EXACT object name "editor" required by
    // the frozen C3 contract (channel.objects.editor on the JS side) BEFORE the
    // first navigation, so the page's WebChannel handshake always finds it.
    Component.onCompleted: {
        if (root.controller)
            editorChannel.registerObject("editor", root.controller)
        root.started = true
        root.start()
    }
}
