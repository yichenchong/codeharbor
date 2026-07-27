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

    // ---- focus reporting (SPEC 4.5) ----
    // The user is working in THIS pane. TerminalRegion connects this when it
    // mints the pane (takePane) and republishes it as the root region's
    // `focusedPaneId`; that is what the palette's split/close commands target,
    // instead of guessing at the region's first leaf.
    signal paneActivated(string paneId)

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
    // Whole sentences, because this is the only thing on screen when a terminal
    // fails to come up — "notconnected" is a state name, not an explanation.
    property string statusText: pane.factory ? qsTr("Not attached to a shell yet.")
                                             : qsTr("No terminal service in this window.")
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
            pane.statusText = qsTr("No Dev Session selected for this pane.")
            return
        }
        if (!pane.factory.connected()) {
            pane.statusText = qsTr("Not connected to a server.")
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
        pane.statusText = qsTr("Session killed. Connect to start a new one.")
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
                // The one URL this view may ever hold. Compared as a STRING:
                // `request.url` arrives as a QUrl and `terminalBundleUrl` as a
                // QML url, and only their normalized text is guaranteed to line
                // up across the two conversions.
                readonly property string pinnedUrl: String(pane.terminalBundleUrl)

                anchors.fill: parent
                url: pane.terminalBundleUrl

                // Privileged profile: permits the WebChannel bridge (the external
                // profile deliberately does not — SPEC 7.2). The bundle is trusted
                // app code, so scripting is intentionally on.
                profile: viewers.internalProfile()
                settings.javascriptEnabled: true
                settings.localContentCanAccessFileUrls: false
                settings.localContentCanAccessRemoteUrls: false
                // SECURITY: window.open() is the one navigation primitive that
                // does not pass through onNavigationRequested below, and xterm's
                // built-in OSC 8 handler reaches for exactly it. Nothing in this
                // page has any reason to open a window.
                settings.javascriptCanOpenWindows: false

                webChannel: terminalChannel

                // SECURITY (SPEC 7.2): this view carries the WebChannel, and Qt
                // injects qt.webChannelTransport into EVERY document it loads —
                // so a document loaded here gets ch::TerminalBridge, i.e. a
                // direct line into the C++ process. The page's CSP does not help:
                // CSP constrains subresources, not top-level navigation. The
                // bytes rendered in this pane come off a remote PTY and are
                // wholly attacker-controlled, so the view must be pinned to the
                // one URL it was created for. Anything else — remote, file:,
                // data:, another qrc page — is refused outright rather than
                // being allowed to inherit the bridge.
                onNavigationRequested: function(request) {
                    if (String(request.url) === pinnedUrl)
                        return
                    request.action = WebEngineNavigationRequest.IgnoreRequest
                    console.warn("TerminalPaneView: refused navigation to", request.url)
                }

                // Belt and braces for the same rule: a new window would be a
                // fresh view outside the pinning above. Handling the signal and
                // never calling openIn() is what denies it.
                onNewWindowRequested: function(request) {
                    console.warn("TerminalPaneView: refused a new window for",
                                 request.requestedUrl)
                }

                onLoadingChanged: function(request) {
                    if (request.status === WebEngineView.LoadSucceededStatus) {
                        pane.pageLoaded = true
                    } else if (request.status === WebEngineView.LoadFailedStatus) {
                        pane.pageLoaded = false
                        pane.statusText = qsTr("The terminal renderer failed to load: %1")
                                          .arg(request.errorString)
                    }
                }
            }
        }
    }

    // Full-pane placeholder while there is no renderer to look at: never a blank
    // rectangle, always what this pane IS, why it is not live, and the one
    // action that might fix it.
    Rectangle {
        anchors.fill: parent
        anchors.margins: pane.border.width
        color: "#11111b"
        visible: !pane.pageLoaded

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 340)
            spacing: 8

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "\u25ae"
                color: "#45475a"
                font.pixelSize: 26
                font.family: "Monospace"
            }
            Label {
                objectName: "paneTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                // The headline is what this pane IS. The id it is keyed by is
                // plumbing and belongs in the small print below.
                text: qsTr("Terminal")
                color: "#cdd6f4"
                font.pixelSize: 14
            }
            Label {
                objectName: "paneReason"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                // SECURITY: a Label defaults to Text.AutoText, which promotes
                // any string that merely LOOKS like markup to StyledText — and
                // StyledText renders <img src="http://..."> by fetching it.
                // statusText carries libssh/WebEngine failure text, which a
                // hostile server has a hand in. It is data, so it is drawn as
                // data.
                textFormat: Text.PlainText
                text: pane.statusText.length > 0 ? pane.statusText : pane.connectionState
                color: "#6c7086"
                font.pixelSize: 12
            }
            Label {
                objectName: "paneIdentityLabel"
                anchors.horizontalCenter: parent.horizontalCenter
                // Same rule: terminalId comes from server data.
                textFormat: Text.PlainText
                text: pane.terminalId
                visible: pane.terminalId.length > 0
                color: "#45475a"
                font.pixelSize: 10
                font.family: "Monospace"
            }
            Button {
                id: attachButton
                anchors.horizontalCenter: parent.horizontalCenter
                visible: pane.factory !== null && !pane.attached
                text: qsTr("Connect")
                implicitHeight: 30
                leftPadding: 14
                rightPadding: 14
                focusPolicy: Qt.StrongFocus
                contentItem: Label {
                    text: attachButton.text
                    color: "#cdd6f4"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: attachButton.down ? "#45475a"
                         : attachButton.hovered ? "#3a3a52" : "#313244"
                    border.width: attachButton.visualFocus ? 2 : 1
                    border.color: attachButton.visualFocus ? "#89b4fa" : "#45475a"
                }
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
                objectName: "paneBannerLabel"
                anchors.verticalCenter: parent.verticalCenter
                // Same rule as the placeholder chrome above: never markup.
                textFormat: Text.PlainText
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

    // A CLICK is the only reliable evidence that the user is working here. The
    // terminal the user types into is an xterm.js page inside a WebEngineView,
    // and focus in that page is Chromium's own state: it never surfaces as QML
    // activeFocus, so watching activeFocus would see nothing for a pane being
    // typed into all day. The click is what PUT the focus in the page, and it
    // is delivered as a real Qt press first. The page's own focus events are
    // deliberately NOT used: routing them here would mean a new slot on
    // ch::TerminalBridge, whose contract with the bundle is frozen.
    //
    // The press is observed and then DECLINED, so it goes on to whatever is
    // underneath — the renderer, the Connect button, the banner's Retry — and
    // this stays a sniffer rather than an input-eating overlay. Declared last
    // because delivery is topmost-first: a sniffer under the placeholder chrome
    // would never see a click on a pane that has not come up yet.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: function(mouse) {
            pane.paneActivated(pane.paneId)
            mouse.accepted = false
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
