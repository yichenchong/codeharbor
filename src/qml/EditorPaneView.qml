import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine
import QtWebChannel
import "RemotePath.js" as RemotePath

// The EDITOR HANDLER (SPEC 8.1). Not a pane type of its own: SPEC 3.3 makes a
// viewer pane a browser addressed by a URL, and a handler is something that
// browser delegates a resolved resource to. ViewerPane keeps its own chrome —
// header, address bar, outside-project marker — around this view for exactly
// that reason, and this file must never grow chrome of its own or assume it
// owns the pane. The name is historical; the concept is a delegate.
//
// It hosts the TRUSTED, app-owned Monaco editor bundle (src/web/editor) in a
// WebEngineView and bridges it to the C++ EditorController over Qt WebChannel.
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
    // The pane's OTHER injected object, `viewers`, is guarded the same way, so a
    // bare load of this component degrades to inert chrome (a WebEngineView on
    // the default profile, no bridge) instead of a ReferenceError that aborts
    // the whole binding pass that was building the pane. Every host and every
    // test that drives a working pane installs it — the only thing that creates
    // an EditorPaneView is ViewerPane, which has already called
    // viewers.viewKind() to decide the file belongs here — but a bare
    // inspection load no longer has to.
    property var factory: (typeof editorFactory !== "undefined") ? editorFactory : null

    // The `viewers` context property (ViewerModel), resolved once and guarded
    // exactly like `factory` above: it supplies the privileged WebEngine profile
    // that permits the WebChannel bridge.
    property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // This pane's stable layout id, set by ViewerPane. Passed to the factory so
    // the controller keys its crash-recovery snapshot per pane (SPEC 11.3), and
    // two panes editing one path never share a snapshot.
    //
    // Deliberately NOT named `paneId`: that name identifies a leaf-pane WRAPPER
    // (ViewerPane/TerminalPaneView carry `paneId` and no `node`), and a content
    // view that also exposed `paneId` would be miscounted as a second leaf by
    // anything walking the region tree by that convention.
    property string recoveryPaneId: ""

    // The ch::EditorController for this pane, minted ONCE in Component.onCompleted
    // (below) rather than as a reactive binding: binding create() to a mutable
    // property such as recoveryPaneId would re-run it when that property settles,
    // minting a SECOND controller and orphaning the first — the one that already
    // opened the file and holds its revision guard. Owned by this pane (destroyed
    // with it); a test may pre-assign a stub before Component.onCompleted runs.
    property var controller: null

    // Entry page of the trusted editor bundle. Embedded into this QML module's
    // resources by src/qml/CMakeLists.txt; overridable by a host or test that
    // wants to serve the bundle from elsewhere.
    property url editorBundleUrl: "qrc:/codeharbor/web/editor/index.html"

    // Remote file URL to open once the controller + bridge are ready.
    property url fileUrl: ""

    // `fileUrl` as the plain remote path the RPC layer speaks (SPEC 8.3):
    // file:// inside CodeHarbor always means the remote SSH server.
    readonly property string remotePath: fileUrl.toString().length > 0
        ? RemotePath.fileUrlToPath(fileUrl.toString())
        : ""

    // Set once Component.onCompleted has registered the bridge object, so a
    // property change during initial binding evaluation cannot navigate early.
    property bool started: false

    // The trusted bundle installs `window.applyTheme` while its script loads.
    // Keep this false until WebEngine reports success so a theme change during
    // navigation cannot run JavaScript against the previous document.
    property bool pageReady: false

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
    // Theme.roles is a plain JavaScript object, so JSON is the one unambiguous
    // boundary between QML values and the page's applyTheme(roles) function.
    // The page validates each role and keeps its own dark fallback; this call
    // therefore remains safe if a future palette grows a role the old bundle
    // does not know yet.
    function applyTheme() {
        if (!root.pageReady)
            return
        const roles = JSON.stringify(Theme.roles)
        view.runJavaScript("if (typeof window.applyTheme === 'function')"
                           + " window.applyTheme(" + roles + ");")
    }
    // ViewerPane keeps handler objects alive when they provide reload(). The
    // editor is a live WebEngine document with unsaved text, so falling back to
    // Loader churn would destroy Monaco and could discard that buffer.
    function reload() {
        view.reload()
    }

    WebChannel {
        id: editorChannel
    }

    WebEngineView {
        id: view
        anchors.fill: parent

        // Privileged profile: permits the WebChannel bridge (the external
        // profile deliberately does not — SPEC 7.3, "Browser Profiles", which
        // is the section requiring the two-profile split). The bundle is
        // trusted app code, so scripting is intentionally on.
        profile: root.viewerModel ? root.viewerModel.internalProfile() : null
        settings.javascriptEnabled: true
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false

        webChannel: editorChannel
        // SECURITY: window.open() is the one navigation primitive that does not
        // pass through onNavigationRequested below. Nothing in the editor bundle
        // has any reason to open a window, and Monaco's built-in link opener
        // reaches for exactly it on a URL found in remote file text.
        settings.javascriptCanOpenWindows: false

        // Theme handoff belongs to the pane because the QML singleton is the
        // source of truth and the page is recreated whenever the language hint
        // changes. A failed/stopped load clears the guard so a later theme
        // change cannot run against a stale document.
        onLoadingChanged: function(request) {
            if (request.status === WebEngineView.LoadSucceededStatus) {
                root.pageReady = true
                root.pageError = ""
                root.applyTheme()
            } else if (request.status === WebEngineView.LoadStartedStatus) {
                root.pageReady = false
                root.pageError = ""
            } else if (request.status === WebEngineView.LoadFailedStatus) {
                root.pageReady = false
                root.pageError = request.errorString
            } else if (request.status === WebEngineView.LoadStoppedStatus) {
                root.pageReady = false
            }
        }

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

    Connections {
        target: Theme
        function onRolesChanged() {
            root.applyTheme()
        }
    }

    // A close can destroy this delegate immediately, so expose the controller's
    // unsaved states to ViewerPane before it forwards a close request. Saving
    // remains dirty until the confirmation arrives; conflict also means the
    // buffer cannot be discarded silently.
    readonly property bool dirty: root.fileState === "modified"
                                  || root.fileState === "saving"
                                  || root.fileState === "conflict"
                                  || root.fileState === "externally_modified"
    readonly property string fileState: root.controller ? root.controller.fileState : ""
    readonly property bool loading: (!root.pageReady && root.pageError.length === 0)
                                    || root.fileState === "loading"
    readonly property bool failed: root.fileState === "error"
                                 || root.pageError.length > 0
    property string pageError: ""
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

    // Binding notifications run before dependent bindings are refreshed. Event
    // handlers therefore use this direct conversion when they need the current
    // file path, rather than reading the potentially stale `remotePath` binding.
    function currentRemotePath() {
        const value = root.fileUrl.toString();
        return value.length > 0 ? RemotePath.fileUrlToPath(value) : "";
    }

    // Same treatment TerminalPaneView gives a dropped channel: a thin banner
    // across the top of the pane, so the warning is unmissable without hiding
    // the text the user is still working on.
    //
    // The geometry is explicit and load-bearing. A Rectangle with neither
    // anchors nor a size is 0x0 at the pane's top-left corner, which is what
    // this was: the banner was built, its `visible` binding flipped correctly,
    // and nothing was ever drawn. Anchored across the top with the same height
    // TerminalPaneView's banner uses, the two columns line up.
    Rectangle {
        objectName: "editorStateBanner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 22
        // Drawn over the editor page rather than beside it: this item is
        // declared after the WebEngineView, so it is painted above Monaco.
        //
        // A file that is merely LOADING says nothing here. This strip is for
        // the states in which the text on screen cannot be saved; a progress
        // message would cover the top of every file for the first moments of
        // every open. The header's busy indicator reports loading instead
        // (ViewerPane reads `root.loading` for exactly that).
        color: root.conflicted || root.failed ? Theme.errorSurface() : Theme.warningSurface()
        visible: root.dropped || root.conflicted || root.failed
        z: 1
        // These are dim fills behind danger/warning text and have no direct
        // Theme role; the palette's errorSurface() and warningSurface()
        // helpers keep both shades aligned across dark and light themes.
        // Red for the state that puts the user's edits at risk, amber for the
        // one that merely suspends them — the same split, and the same two
        // colours, TerminalPaneView uses for "error" versus a plain drop.

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
                  : root.failed
                    ? qsTr("The editor could not load this file.")
                    : qsTr("The connection to the server is down. This file cannot be saved or reloaded until it comes back.")
            color: root.conflicted || root.failed ? Theme.danger : Theme.warning
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    // ---- crash recovery (SPEC 11.3) ----------------------------------------
    //
    // While the user types, ch::EditorController snapshots the unsaved buffer
    // to a per-pane file on the SERVER. When the same file is opened again it
    // compares that snapshot with the bytes on disk, and a snapshot that
    // differs is unsaved work the application died holding: it announces it as
    // `recoveryAvailable`. Announcing is ALL the controller can do — keeping or
    // dropping the work is a decision only the person who typed it can make —
    // so the offer below is the other half of the feature. Without it every
    // recovered buffer was read back off the server and then dropped on the
    // floor, one round trip per file open, and the user was never told their
    // work had survived.

    // The revision the page's buffer is guarded at (SPEC 8.4/8.6), read off the
    // same `contentLoaded` the editor page takes its content from.
    //
    property string loadedRevision: ""
    // A recovered buffer may legitimately be empty (the user deleted every
    // character), and a new file may have an empty revision. Separate
    // readiness/presence flags keep those valid values from being mistaken for
    // "nothing recovered" or "the page has not loaded yet".
    property bool contentReadyForRecovery: false
    property string recoveredContent: ""
    property bool recoveryPending: false

    Connections {
        target: root.controller
        function onContentLoaded(content, revision) {
            root.loadedRevision = revision
            root.contentReadyForRecovery = true
            root.showRecoveryOffer()
        }
        // Named `recovered`, not `recoveredContent`: a parameter that shadows
        // the property it is assigned to reads as a self-assignment.
        function onRecoveryAvailable(recovered) {
            root.recoveredContent = recovered
            root.recoveryPending = true
            root.showRecoveryOffer()
        }
    }

    // Offer the recovered buffer, but never before the page can be GIVEN it.
    // ch::EditorController holds contentLoaded until the page reports ready(),
    // so the content-loaded flag means the page has taken the file even when
    // the revision itself is empty.
    function showRecoveryOffer() {
        if (root.recoveryPending && root.contentReadyForRecovery
                && !recoveryDialog.visible)
            recoveryDialog.open()
    }

    // Put the recovered text back into the pane, and keep it recoverable.
    //
    // TWO steps, both required. `contentLoaded` is the ONE way content reaches
    // the Monaco page (the frozen C3 bridge has no other), re-emitted here with
    // the revision the page already holds so the save guard is untouched. The
    // page treats a host-driven load as not-an-edit and therefore does NOT mark
    // the buffer dirty — so reportContent() states what is now true: the bytes
    // on screen are not the bytes on the server. That is what stops an external
    // change from silently reloading over the restored work (SPEC 8.7), and
    // what keeps the snapshot alive until the user actually saves.
    function restoreRecovered() {
        if (!root.controller || !root.recoveryPending || !root.contentReadyForRecovery)
            return
        const recovered = root.recoveredContent
        root.recoveredContent = ""
        root.recoveryPending = false
        root.controller.contentLoaded(recovered, root.loadedRevision)
        root.controller.reportContent(recovered)
    }

    // Keep what the server has. The snapshot itself is deliberately left where
    // it is: this answers "what should this pane show", and ch::EditorController
    // is what discards the snapshot, on the next successful save.
    function discardRecovered() {
        root.recoveredContent = ""
        root.recoveryPending = false
    }

    AppDialog {
        id: recoveryDialog
        objectName: "editorRecoveryDialog"
        title: qsTr("Unsaved changes were recovered")
        modal: true
        anchors.centerIn: Overlay.overlay
        // The frame has to hold the fixed-width content plus its own padding;
        // the Basic style's 320-pixel default would clip it. Same arithmetic as
        // the sidebar's dialogs.
        width: 380 + leftPadding + rightPadding
        // Deliberately NOT dismissible by Escape or by a click outside. Every
        // other dialog here asks about something the user can redo; this one
        // asks about work that exists in exactly one place, and a stray keypress
        // must not be able to answer it.
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.Discard | Dialog.Ok

        // "OK" beside a recovered buffer does not say which button keeps the
        // work, and the standard set has no Restore. The button is renamed
        // rather than replaced so it keeps the dialog's own styling and its
        // place in the platform's button order.
        onOpened: {
            const restore = recoveryDialog.standardButton(Dialog.Ok)
            if (restore)
                restore.text = qsTr("Restore")
        }

        onAccepted: root.restoreRecovered()
        onDiscarded: {
            root.discardRecovered()
            recoveryDialog.close()
        }

        ColumnLayout {
            // A Dialog sizes itself from its content item's IMPLICIT width, so
            // the layout states one; see the sidebar dialogs for the full note.
            implicitWidth: 380
            spacing: 8

            Label {
                Layout.preferredWidth: 380
                wrapMode: Text.WordWrap
                // A remote path: data, never markup, like every server-fed
                // string in this module.
                textFormat: Text.PlainText
                text: qsTr("CodeHarbor still had unsaved changes to %1 when it last closed. "
                           + "They are not in the file on the server.").arg(root.remotePath)
                color: Theme.text
                font.pixelSize: Theme.fontSizeBody
            }
            Label {
                Layout.preferredWidth: 380
                wrapMode: Text.WordWrap
                text: qsTr("Restore puts them back in this editor, unsaved, so you can look "
                           + "them over and save. Discard keeps the file exactly as it is on "
                           + "the server.")
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeSmall
            }
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
        const path = root.currentRemotePath()
        view.url = path.length > 0
            ? base + "?path=" + encodeURIComponent(path)
            : base
    }

    function start() {
        // A pane reused for another file carries nothing over: the revision
        // belongs to the file being left, and so does any recovery offer that
        // was still standing. Offering the previous file's unsaved work against
        // the new one would restore it into the wrong buffer.
        root.loadedRevision = ""
        root.contentReadyForRecovery = false
        root.recoveredContent = ""
        root.recoveryPending = false
        recoveryDialog.close()
        const path = root.currentRemotePath()
        if (root.controller && path.length > 0)
            root.controller.open(path)
        root.navigate()
    }

    // A pane reused for another file reopens the controller and reloads the
    // page so the language hint follows the new path.
    onFileUrlChanged: if (root.started) root.start()
    onEditorBundleUrlChanged: if (root.started) root.navigate()

    // The pane's layout id can settle (""->"viewer-1") AFTER the controller is
    // minted, so push every change through to the controller's recovery key
    // rather than capturing it once at create() time — otherwise two panes on
    // one file would key their crash-recovery snapshots by the same empty id
    // and share one (SPEC 11.3, ED15).
    onRecoveryPaneIdChanged: if (root.controller) root.controller.setRecoveryId(root.recoveryPaneId)

    // Register the controller under the EXACT object name "editor" required by
    // the frozen C3 contract (channel.objects.editor on the JS side) BEFORE the
    // first navigation, so the page's WebChannel handshake always finds it.
    Component.onCompleted: {
        // Mint the controller once, here, so create() is not in a reactive
        // binding (see `controller` above). A pre-assigned stub is kept.
        if (!root.controller && root.factory)
            root.controller = root.factory.create(root, root.recoveryPaneId)
        if (root.controller)
            editorChannel.registerObject("editor", root.controller)
        root.started = true
        root.start()
    }
}
