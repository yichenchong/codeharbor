import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import QtWebChannel

// A single terminal pane (SPEC 3.4, 5.1) — a leaf of the terminal split tree.
// Hosts the TRUSTED, app-owned xterm.js bundle (src/web/terminal) in a
// WebEngineView and bridges it to this pane's C++ TerminalController through a
// per-pane ch::TerminalBridge over Qt WebChannel.
//
// SECURITY (SPEC 7.2): JavaScript is ENABLED here — unlike the untrusted viewer
// WebEngineViews — because the page is our OWN code. Terminal bytes never
// arrive as a navigated document: they flow in over the bridge (write) and back
// out through its slots (sendInput/resize/notifyViewVisible).
//
// WIRING (for main.cpp / the QML host):
//   * `terminalFactory` — context property: the ch::TerminalFactory that mints
//     this pane's controller + bridge and opens its PTY channel. Absent (as in
//     a bare QML load) the pane stays inert chrome instead of erroring.
//   * `viewers`         — context property (ViewerModel): supplies the
//     privileged WebEngine profile that permits a WebChannel bridge.
//   * `devSessionId` / `workingDir` must be bound by whatever builds the split
//     tree; they are what the pane's tmux target and cwd are made of (SPEC 5.2).
//
// BUNDLE URL: `terminalBundleUrl` points at the packaged page
// qrc:/codeharbor/web/terminal/index.html, embedded into the codeharbor_qml
// module's resources by src/qml/CMakeLists.txt from src/web/terminal/dist (built
// by `npm run build --workspace src/web/terminal`). qrc: resolves identically in
// the build tree, in a relocated install and inside a macOS bundle. The page
// loads Qt's own qrc:///qtwebchannel/qwebchannel.js plus the bundle, then mounts
// xterm.js against channel.objects.terminal.
//
// RESIZE: the pane never converts pixels to cells — only the renderer knows its
// cell metrics. A pane resize resizes the WebEngineView, the page's
// ResizeObserver re-fits xterm.js, and the fitted cols x rows come back through
// bridge.resize() -> TerminalController::resize() -> SSH window-change.
Rectangle {
    id: pane

    // ---- split-tree identity ----
    property string paneId: ""
    // Stable ids the tmux target is built from (SPEC 5.2). A terminal leaf's
    // pane id IS its terminal id unless the host overrides it.
    property string devSessionId: ""
    property string terminalId: pane.paneId
    property string workingDir: ""

    // The `terminalFactory` context property, resolved once. Guarded so a host
    // that does not install it (a bare QML load) gets inert chrome rather than
    // a ReferenceError; also the seam a test injects a stub through.
    property var factory: (typeof terminalFactory !== "undefined") ? terminalFactory : null

    // Per-pane C++ objects, owned by this pane (destroyed with it).
    property var controller: pane.factory ? pane.factory.create(pane) : null
    property var bridge: pane.controller ? pane.factory.createBridge(pane.controller, pane) : null

    // Entry page of the trusted terminal bundle; overridable by a host or test
    // that wants to serve the bundle from elsewhere.
    property url terminalBundleUrl: "qrc:/codeharbor/web/terminal/index.html"

    // ---- observable pane state ----
    readonly property string connectionState: pane.bridge ? pane.bridge.connectionState : "unloaded"
    property bool attached: false
    property bool pageLoaded: false
    // Human-readable reason the pane is not live; shown instead of a blank pane.
    property string statusText: pane.factory ? qsTr("not connected") : qsTr("no terminal service")
    // Live means: a PTY is attached AND the renderer is showing it.
    readonly property bool live: pane.attached && pane.pageLoaded
                                 && pane.connectionState === "ready"

    color: "#11111b"
    border.color: "#313244"
    border.width: 1
    implicitWidth: 160
    implicitHeight: 100

    // Open (or re-open) this pane's PTY. Idempotent while attached; safe to call
    // from the host whenever the session or the SSH connection changes.
    function attachNow() {
        if (pane.attached || !pane.factory || !pane.controller)
            return
        if (pane.devSessionId.length === 0 || pane.terminalId.length === 0) {
            pane.statusText = qsTr("no session selected")
            return
        }
        if (!pane.factory.connected()) {
            pane.statusText = qsTr("not connected")
            return
        }
        // The renderer's fitted size when it has already mounted; 0 lets the
        // factory open at the conventional 80x24 until the first fit arrives.
        const cols = pane.bridge ? pane.bridge.columns : 0
        const rows = pane.bridge ? pane.bridge.rows : 0
        pane.attached = pane.factory.attach(pane.controller, pane.devSessionId,
                                            pane.terminalId, pane.workingDir, cols, rows)
        if (pane.attached)
            pane.statusText = ""
    }

    // Release the PTY channel; the remote tmux session keeps running.
    function detachNow() {
        if (pane.factory && pane.controller)
            pane.factory.detach(pane.controller)
        pane.attached = false
    }

    // Destroy the remote tmux session for this pane, processes and all.
    function killSession() {
        if (pane.factory && pane.controller)
            pane.factory.kill(pane.controller)
        pane.attached = false
        pane.statusText = qsTr("session killed")
    }

    // A pane retargeted at another session/terminal must follow it: drop the
    // old PTY (the remote tmux session stays alive for whoever else wants it)
    // and open the new one.
    function retarget() {
        if (pane.attached)
            pane.detachNow()
        pane.attachNow()
    }

    onDevSessionIdChanged: pane.retarget()
    onTerminalIdChanged: pane.retarget()
    onWorkingDirChanged: if (!pane.attached) pane.attachNow()
    // Hidden panes keep their PTY but must stop being treated as renderable, so
    // output buffers instead of being flushed at a view nobody can see (SPEC 5.4).
    onVisibleChanged: if (pane.bridge) pane.bridge.notifyViewVisible(pane.visible)

    Connections {
        target: pane.factory
        // Channel diagnostics are reported for every pane on this factory; only
        // this pane's own failures belong in its chrome.
        function onError(controller, message) {
            if (controller === pane.controller)
                pane.statusText = message
        }
    }

    Connections {
        target: pane.bridge
        // The first fit of a renderer that mounted after the PTY was opened is
        // pushed to the live PTY by the controller; a pane still waiting on a
        // session takes it as the size to attach at.
        function onGeometryChanged() { pane.attachNow() }
    }

    // A pane whose channel dropped is no longer attached: surface the retry
    // affordance instead of leaving a dead terminal behind.
    onConnectionStateChanged: {
        if (pane.connectionState === "disconnected" || pane.connectionState === "error")
            pane.attached = false
    }

    WebChannel {
        id: terminalChannel
    }

    // Activated only once the bridge is registered (and only when there IS one),
    // so the page's WebChannel handshake can never start before its object
    // exists — and a host without the factory spawns no renderer at all.
    Loader {
        id: viewLoader
        anchors.fill: parent
        active: false
        sourceComponent: Component {
            WebEngineView {
                anchors.fill: parent
                url: pane.terminalBundleUrl

                // Privileged profile: permits the WebChannel bridge (the external
                // profile deliberately does not — SPEC 7.2). The bundle is trusted
                // app code, so scripting is intentionally on.
                profile: viewers.internalProfile()
                settings.javascriptEnabled: true
                settings.localContentCanAccessFileUrls: false
                settings.localContentCanAccessRemoteUrls: false

                webChannel: terminalChannel

                onLoadingChanged: function(request) {
                    if (request.status === WebEngineView.LoadSucceededStatus) {
                        pane.pageLoaded = true
                    } else if (request.status === WebEngineView.LoadFailedStatus) {
                        pane.pageLoaded = false
                        pane.statusText = qsTr("renderer failed to load: %1").arg(request.errorString)
                    }
                }
            }
        }
    }

    // Full-pane placeholder while there is no renderer to look at: never a blank
    // rectangle, always the pane's identity plus why it is not live.
    Rectangle {
        anchors.fill: parent
        anchors.margins: pane.border.width
        color: "#11111b"
        visible: !pane.pageLoaded

        Column {
            anchors.centerIn: parent
            spacing: 8

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: pane.terminalId.length > 0 ? pane.terminalId : qsTr("Terminal")
                color: "#cdd6f4"
                font.pixelSize: 13
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: pane.statusText.length > 0 ? pane.statusText : pane.connectionState
                color: "#6c7086"
                font.pixelSize: 11
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: pane.factory !== null && !pane.attached
                text: qsTr("Connect")
                onClicked: pane.attachNow()
            }
        }
    }

    // Loaded but not live: a thin banner, so a drop or an attach failure is
    // visible without hiding the scrollback the user still wants to read.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: pane.border.width
        height: 22
        color: pane.connectionState === "error" ? "#45222c" : "#3a2f1e"
        visible: pane.pageLoaded && !pane.live

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: pane.statusText.length > 0
                      ? pane.statusText
                      : qsTr("terminal %1").arg(pane.connectionState)
                color: "#f9e2af"
                font.pixelSize: 11
            }
            Button {
                anchors.verticalCenter: parent.verticalCenter
                visible: pane.factory !== null && !pane.attached
                text: qsTr("Retry")
                onClicked: pane.attachNow()
            }
        }
    }

    // Register the bridge under the EXACT object name "terminal" the page's
    // bootstrap looks up (channel.objects.terminal) BEFORE the view is created,
    // so the handshake always finds it.
    Component.onCompleted: {
        if (pane.bridge) {
            terminalChannel.registerObject("terminal", pane.bridge)
            viewLoader.active = true
        }
        pane.attachNow()
    }

    // Deterministic release: the controller and its channel die with the pane
    // anyway, but detaching first closes the SSH channel instead of leaving it
    // to destruction order.
    Component.onDestruction: pane.detachNow()
}
