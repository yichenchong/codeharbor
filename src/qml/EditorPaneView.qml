import QtQuick
import QtQuick.Controls.Basic
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
//   * `editorFactory`     — context property: the ch::EditorFactory that mints
//     THIS pane's own ch::EditorController (one per pane, so two panes never
//     share a path and revision). That controller is what is exposed to JS over
//     the WebChannel under the object name "editor". There is deliberately no
//     single shared controller context property.
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

    // The `editorFactory` context property, resolved once. Guarded exactly like
    // TerminalPaneView guards `terminalFactory`, so a host that does not install
    // it gets a pane with no controller instead of a ReferenceError (an unguarded
    // context-property lookup throws, which aborts the whole binding pass that
    // was building the pane); also the seam a test injects a stub through.
    //
    // The pane's OTHER injected object, `viewers`, is deliberately used
    // unguarded, exactly as every viewer view does (ViewerImageView,
    // ViewerPdfView, ViewerBinaryView) — it supplies the WebEngine profile, and
    // there is no useful pane without one. Nothing can instantiate this pane
    // without it either: the only thing that creates an EditorPaneView is
    // ViewerPane, which has already called viewers.viewKind() to decide that the
    // file belongs here.
    property var factory: (typeof editorFactory !== "undefined") ? editorFactory : null

    // The ch::EditorController for this pane, created per-pane by that factory
    // and owned by this pane (destroyed with it); overridable for tests.
    property var controller: root.factory ? root.factory.create(root) : null

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

    // The ONE document this view may ever hold (the navigation guard below).
    // Compared as a STRING: `request.url` arrives as a QUrl and
    // `editorBundleUrl` as a QML url, and only their normalized text is
    // guaranteed to line up across the two conversions.
    //
    // Query and fragment are cut from BOTH sides before comparing. Our own
    // navigation carries a `?path=` language hint, and a percent-encoded path
    // does not survive QUrl's pretty-decoded round trip byte for byte, so an
    // exact-string pin would refuse the pane's own page. Ignoring the query
    // costs nothing: it selects no document — the bytes loaded are the trusted
    // qrc bundle either way — while remote, file:, data: and every other qrc
    // page still fail the comparison.
    function pinnedDocument(candidate) {
        let text = String(candidate)
        const query = text.indexOf("?")
        if (query >= 0)
            text = text.substring(0, query)
        const fragment = text.indexOf("#")
        if (fragment >= 0)
            text = text.substring(0, fragment)
        return text
    }

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
        // SECURITY: window.open() is the one navigation primitive that does not
        // pass through onNavigationRequested below. Nothing in the editor bundle
        // has any reason to open a window, and Monaco's built-in link opener
        // reaches for exactly it on a URL found in remote file text.
        settings.javascriptCanOpenWindows: false

        // SECURITY (SPEC 7.2): this view carries the WebChannel, and Qt injects
        // qt.webChannelTransport into EVERY document it loads — so a document
        // loaded here gets ch::EditorController, i.e. open()/save() against any
        // path on the remote host. The document rendered here is our own bundle,
        // but the CONTENT it displays is attacker-controlled remote bytes, so
        // the view is pinned to the bundle document. Anything else — remote,
        // file:, data:, another qrc page — is refused outright rather than
        // being allowed to inherit the bridge. Identical rule to
        // TerminalPaneView.qml.
        onNavigationRequested: function(request) {
            const allowed = root.pinnedDocument(root.editorBundleUrl)
            if (allowed.length > 0
                && root.pinnedDocument(request.url) === allowed)
                return
            request.action = WebEngineNavigationRequest.IgnoreRequest
            console.warn("EditorPaneView: refused navigation to", request.url)
        }

        // Belt and braces for the same rule: a new window would be a fresh view
        // outside the pinning above. Handling the signal and never calling
        // openIn() is what denies it.
        onNewWindowRequested: function(request) {
            console.warn("EditorPaneView: refused a new window for",
                         request.requestedUrl)
        }

        // NOT bound to editorBundleUrl: navigation is driven by navigate()
        // below so it can never start before registerObject("editor").
    }

    // ---- observable file state (SPEC 8.2) --------------------------------
    // The lifecycle word ch::EditorController publishes (see toString(FileState)
    // in src/models/SessionState.cpp): "loading", "clean", "modified", "saving",
    // "saved", "externally_modified", "conflict", "read_only", "error",
    // "disconnected".
    readonly property string fileState: root.controller ? root.controller.fileState : ""

    // The two states in which the buffer on screen is NOT what the server holds
    // and the next save cannot simply succeed. The editor page prints the state
    // word in its status line, which is far too quiet for either: a user who
    // does not notice keeps typing into a buffer that cannot be saved.
    //
    //   disconnected — the SSH transport is down (ch::EditorController leaves
    //                  the file here from onTransportClosed until a transport is
    //                  bound again). Nothing can be read or written.
    //   conflict     — the file changed on the server since it was loaded, so
    //                  the save was refused rather than overwriting someone
    //                  else's work (SPEC 8.4/8.6). Only the user can resolve it.
    // The path check matters: a freshly created ch::EditorController starts in
    // FileState::Disconnected and only leaves it when a file is opened, so
    // without it a pane that has no file yet would advertise a connection
    // failure that has not happened.
    readonly property bool dropped: root.remotePath.length > 0
                                    && root.fileState === "disconnected"
    readonly property bool conflicted: root.fileState === "conflict"

    // Same treatment TerminalPaneView gives a dropped channel: a thin banner
    // across the top of the pane, so the warning is unmissable without hiding
    // the text the user is still working on.
    Rectangle {
        objectName: "editorStateBanner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 22
        visible: root.dropped || root.conflicted
        // Red for the state that puts the user's edits at risk, amber for the
        // one that merely suspends them — the same split, and the same two
        // colours, TerminalPaneView uses for "error" versus a plain drop.
        color: root.conflicted ? "#45222c" : "#3a2f1e"

        Label {
            objectName: "editorStateBannerLabel"
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            // Whole sentences: the state word alone ("conflict") names the
            // condition without telling anyone what it means for their file.
            text: root.conflicted
                  ? qsTr("This file changed on the server since it was opened, so the last save was refused. Reload to take the server's copy, or save again to overwrite it.")
                  : qsTr("The connection to the server is down. This file cannot be saved or reloaded until it comes back.")
            color: "#f9e2af"
            font.pixelSize: 11
        }
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
