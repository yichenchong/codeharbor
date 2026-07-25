import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window

// The fixed three-region outer layout (SPEC 2.3, 4.1). Region widths are
// adjustable via the SplitView handles and are persisted per client via
// app.uiState (SPEC 4.1): restored on startup and written back whenever a
// handle move resizes a region.
ApplicationWindow {
    id: window
    width: 1440
    height: 900
    visible: true
    title: qsTr("CodeHarbor")

    // Persist the current region widths. viewer is the fill region (0 = fill),
    // so only the sidebar and terminal fixed widths are meaningful to store.
    function persistRegionWidths() {
        app.uiState.setRegionWidths(Math.round(sidebarRegion.width),
                                    0,
                                    Math.round(terminalRegion.width));
    }

    // Persist ONLY user-driven handle drags, never layout-driven width changes.
    // Writing back on every onWidthChanged destroyed stored widths: a restored
    // width that does not fit the current window is clamped by SplitView (against
    // the neighbouring region's minimumWidth), and persisting that clamped value
    // overwrote the user's real preference permanently - open the app once on a
    // narrower screen and the stored width was gone for good.
    //
    // SplitView.resizing is true only while a handle is being dragged, so the
    // write happens exactly once, when a drag finishes.
    Connections {
        target: outer
        function onResizingChanged() {
            if (!outer.resizing)
                window.persistRegionWidths();
        }
    }

    SplitView {
        id: outer
        anchors.fill: parent
        orientation: Qt.Horizontal

        Component.onCompleted: {
            var sidebar = app.uiState.sidebarWidth();
            var terminal = app.uiState.terminalWidth();
            if (sidebar > 0)
                sidebarRegion.SplitView.preferredWidth = sidebar;
            if (terminal > 0)
                terminalRegion.SplitView.preferredWidth = terminal;
        }

        SessionsSidebar {
            id: sidebarRegion
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
            // Clicking (or Enter-ing) a session makes it current: AppController
            // loads both region layouts and remembers it for the next launch.
            onSessionActivated: (devSessionId) => app.activateSession(devSessionId)
        }

        // The regions are recursive split trees, inert until given a node. The
        // trees come from the server for the active Dev Session; until one
        // resolves (no server yet, or a load in flight) both are null, so the
        // literal fallback below keeps the SPEC 4.5 "always at least one pane"
        // invariant instead of showing an empty shell.
        ViewerRegion {
            node: (app.layouts && app.layouts.viewerTree)
                  ? app.layouts.viewerTree
                  : ({ paneId: "viewer-1", url: "", children: [] })
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }

        TerminalRegion {
            id: terminalRegion
            node: (app.layouts && app.layouts.terminalTree)
                  ? app.layouts.terminalTree
                  : ({ paneId: "terminal-1", children: [] })
            // A terminal pane only attaches once it knows which Dev Session it
            // belongs to; the repo root becomes the shell's working directory.
            devSessionId: app.activeSessionId
            workingDir: app.activeSessionRepoRoot
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
        }
    }

    // Non-blocking error banner: surfaces app.error (RPC failures forwarded
    // verbatim, SPEC 10.3) as a transient toast so shell-level failures are
    // visible instead of silently swallowed.
    Rectangle {
        id: errorBanner
        z: 1000
        visible: opacity > 0
        opacity: 0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 12
        width: Math.min(errorLabel.implicitWidth + 32, parent.width - 24)
        height: errorLabel.implicitHeight + 20
        radius: 6
        color: "#f38ba8"

        Behavior on opacity { NumberAnimation { duration: 200 } }

        Label {
            id: errorLabel
            anchors.centerIn: parent
            width: parent.width - 32
            color: "#11111b"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        MouseArea {
            anchors.fill: parent
            onClicked: errorBanner.opacity = 0
        }

        Timer {
            id: errorHideTimer
            interval: 6000
            onTriggered: errorBanner.opacity = 0
        }
    }

    Connections {
        target: app
        function onError(message) {
            errorLabel.text = message;
            errorBanner.opacity = 0.97;
            errorHideTimer.restart();
        }
    }

    // --- Server connection (cold start) -------------------------------------
    // With no profile stored there is no way to reach a server, so the sheet
    // opens itself on first run; afterwards it is reachable from the palette.
    ConnectSheet {
        id: connectSheet
        anchors.fill: parent
        z: 900
        visible: shown
        property bool shown: false

        profiles: app.serverProfiles ? app.serverProfiles.profiles : []
        activeId: app.serverProfiles ? app.serverProfiles.activeId : ""
        connectionState: app.connectionState
        errorText: app.connectionError

        onConnectRequested: (profileId) => app.connectToProfile(profileId)
        onProfileSaved: (fields) => {
            if (!app.serverProfiles)
                return;
            if (fields.id && fields.id.length > 0)
                app.serverProfiles.updateProfile(fields.id, fields);
            else
                app.serverProfiles.addProfile(fields);
        }
        onProfileRemoved: (id) => { if (app.serverProfiles) app.serverProfiles.removeProfile(id); }
        onHostKeyDecision: (accept) => { pendingHostKey = null; app.resolveHostKey(accept); }
        onDismissed: shown = false

        Component.onCompleted: {
            if (!app.serverProfiles || app.serverProfiles.profiles.length === 0) {
                // Nothing to connect to yet -> show the sheet rather than a dead
                // shell the user has no way out of.
                shown = true;
            } else if (app.serverProfiles.activeId.length > 0) {
                // Reconnect to the last server on launch. Deferred with
                // callLater so the window paints first: connectAndWire() is a
                // synchronous libssh handshake and would otherwise hold up the
                // very first frame.
                Qt.callLater(() => app.connectToProfile(app.serverProfiles.activeId));
            }
        }
    }

    Connections {
        target: app
        // An unknown host key refused the connect; surface the fingerprint and
        // let the user decide (accepting retries the connect).
        function onHostKeyPrompt(host, keyType, fingerprint) {
            connectSheet.pendingHostKey = { host: host, keyType: keyType, fingerprint: fingerprint };
            connectSheet.shown = true;
        }
    }

    // --- Command palette (SPEC 15) ------------------------------------------

    // Surface a message in the existing toast. QML cannot emit app.error (a C++
    // signal), so a command that cannot run says so here rather than no-oping.
    function notifyUser(message) {
        errorLabel.text = message;
        errorBanner.opacity = 0.97;
        errorHideTimer.restart();
    }

    // Deepest-first leaf of a split tree: the pane a split acts on when the user
    // has not explicitly selected one.
    function firstPaneId(tree, fallback) {
        if (!tree)
            return fallback;
        if (!tree.children || tree.children.length === 0)
            return tree.paneId ? tree.paneId : fallback;
        return firstPaneId(tree.children[0], fallback);
    }

    // The pane a region command targets: the user's selected pane when one is
    // recorded for this Dev Session, else that region's first leaf. Passing ""
    // would match no node and only error, so never do that.
    function targetPaneId(region) {
        const selected = (app.activeSessionId.length > 0 && app.uiState)
                       ? app.uiState.selectedPane(app.activeSessionId) : "";
        if (selected && selected.length > 0)
            return selected;
        return region === "viewer"
             ? firstPaneId(app.layouts ? app.layouts.viewerTree : null, "viewer-1")
             : firstPaneId(app.layouts ? app.layouts.terminalTree : null, "terminal-1");
    }

    function splitActivePane(region, orientation) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            notifyUser(qsTr("Select a Dev Session before splitting a pane."));
            return;
        }
        app.layouts.splitPane(region, targetPaneId(region), orientation);
    }

    readonly property var paletteCommands: [
        { id: "server.connect", title: qsTr("Connect to Server…"), shortcut: "Ctrl+Shift+O",
          invoke: () => { connectSheet.shown = true; } },
        { id: "server.disconnect", title: qsTr("Disconnect from Server"),
          invoke: () => app.disconnectServer() },
        { id: "session.refresh", title: qsTr("Refresh Workspace"), shortcut: "Ctrl+R",
          invoke: () => app.refresh() },
        { id: "viewer.split.h", title: qsTr("Split Viewer Pane Horizontally"),
          invoke: () => window.splitActivePane("viewer", "horizontal") },
        { id: "viewer.split.v", title: qsTr("Split Viewer Pane Vertically"),
          invoke: () => window.splitActivePane("viewer", "vertical") },
        { id: "terminal.split.h", title: qsTr("Split Terminal Pane Horizontally"),
          invoke: () => window.splitActivePane("terminal", "horizontal") },
        { id: "terminal.split.v", title: qsTr("Split Terminal Pane Vertically"),
          invoke: () => window.splitActivePane("terminal", "vertical") },
        { id: "agent.markSeen", title: qsTr("Mark Agent Output Seen"),
          invoke: () => {
              if (app.activeSessionId.length === 0)
                  window.notifyUser(qsTr("No active Dev Session."));
              else
                  agentMonitor.markSeen(app.activeSessionId);
          } }
    ]

    CommandPalette {
        id: commandPalette
        commands: window.paletteCommands
    }
}
