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

    // SPEC 4.5 fallback trees for a region with no server tree yet. Declared as
    // properties with dependency-free bindings so each is constructed EXACTLY
    // ONCE and keeps a stable identity for the whole run; see the ViewerRegion /
    // TerminalRegion comment below for why identity matters.
    readonly property var viewerFallbackNode: ({ paneId: "viewer-1", url: "", children: [] })
    readonly property var terminalFallbackNode: ({ paneId: "terminal-1", children: [] })

    // Point the terminal region at the active Dev Session, WORKING DIRECTORY
    // FIRST.
    //
    // AppController notifies activeSessionId and activeSessionRepoRoot with the
    // same activeSessionChanged signal, and TerminalPaneView attaches its PTY on
    // devSessionId (onDevSessionIdChanged -> retarget) while only adopting a
    // working directory it is not yet attached with (onWorkingDirChanged -> attach
    // if !attached). Two independent bindings therefore raced: whenever the id
    // landed first the pane attached with an empty working directory — a shell in
    // $HOME instead of the repo — and the later directory update was ignored.
    // Writing both here, in order, makes the pane see the directory before the id
    // it attaches on. (The pane-side fix, adopting a late working directory, is
    // TerminalPaneView's; this removes the race from the host regardless.)
    function retargetTerminals() {
        terminalRegion.workingDir = app.activeSessionRepoRoot;
        terminalRegion.devSessionId = app.activeSessionId;
    }

    Component.onCompleted: window.retargetTerminals()

    Connections {
        target: app
        function onActiveSessionChanged() { window.retargetTerminals(); }
    }

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
        //
        // The fallbacks are ONE stable object each (window.viewerFallbackNode /
        // window.terminalFallbackNode), never a fresh literal in the binding. A
        // literal here handed the region a brand new `node` identity on every
        // re-evaluation of this binding — and every viewerTreeChanged /
        // terminalTreeChanged fires one, including the clearTrees() that starts
        // each load. A changed node rebuilds the region's delegates, which
        // destroys and recreates its panes: a live terminal killed and a dirty
        // editor buffer dropped, for a node that did not actually change.
        ViewerRegion {
            node: (app.layouts && app.layouts.viewerTree)
                  ? app.layouts.viewerTree
                  : window.viewerFallbackNode
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
            // The region reports the pane the user last interacted with; record
            // it so pane commands act on THAT pane instead of the region's first
            // leaf. The empty value is deliberately NOT filtered: a focused pane
            // that was closed must clear the selection rather than leave a
            // command pointing at a pane that no longer exists.
            onFocusedPaneIdChanged: if (app.uiState && app.activeSessionId.length > 0)
                                        app.uiState.setSelectedPane(app.activeSessionId,
                                                                    focusedPaneId)
        }

        TerminalRegion {
            id: terminalRegion
            node: (app.layouts && app.layouts.terminalTree)
                  ? app.layouts.terminalTree
                  : window.terminalFallbackNode
            // devSessionId/workingDir are pushed as an ORDERED PAIR by
            // window.retargetTerminals(), not bound here; see that function.
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
            onFocusedPaneIdChanged: if (app.uiState && app.activeSessionId.length > 0)
                                        app.uiState.setSelectedPane(app.activeSessionId,
                                                                    focusedPaneId)
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
        onCredentialSubmitted: (secret) => {
            // Cleared here BEFORE the secret is handed over, so the sheet is
            // never left holding a prompt for an answer already spent.
            pendingCredential = null;
            app.submitCredential(secret);
        }
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
                // very first frame. Re-checked at fire time: a launch that is
                // ALREADY wired (the CH_LIVE_SSH env path in main.cpp brings a
                // session up before the engine loads) must not be torn down and
                // redialled underneath itself.
                Qt.callLater(() => {
                    if (app.connectionState === "connected"
                        || app.connectionState === "connecting")
                        return;
                    if (app.serverProfiles && app.serverProfiles.activeId.length > 0)
                        app.connectToProfile(app.serverProfiles.activeId);
                });
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
        // ssh-agent and the default keys could not authenticate; the connect was
        // refused so we could ask instead of blocking inside the handshake.
        function onCredentialPrompt(user, host, prompt) {
            connectSheet.pendingCredential = { user: user, host: host, prompt: prompt };
            connectSheet.shown = true;
        }
        // A successful connect is the sheet's exit condition. It fills the
        // window at z 900, so leaving it up after the workspace is reachable
        // hides the very thing the user just connected to behind a dialog they
        // have to discover how to close.
        function onConnectionStateChanged() {
            if (app.connectionState === "connected") {
                connectSheet.pendingHostKey = null;
                connectSheet.pendingCredential = null;
                connectSheet.shown = false;
            }
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
    //
    // An EMPTY paneId is a real answer, not a missing one: closing a region's
    // last pane leaves a single placeholder leaf with paneId "", and passing that
    // "" back to splitPane() is exactly how SessionLayouts fills it in place.
    // Substituting the region default there ("viewer-1") named a pane the tree
    // does not contain, splitPane rejected it, and the user could never get a
    // pane back into a region they had emptied. `fallback` is only for a region
    // with NO tree at all.
    function firstPaneId(tree, fallback) {
        if (!tree)
            return fallback;
        if (tree.children && tree.children.length > 0)
            return firstPaneId(tree.children[0], fallback);
        return (tree.paneId === undefined || tree.paneId === null) ? fallback : tree.paneId;
    }

    // Does `tree` hold a leaf with this paneId?
    function treeHasPane(tree, paneId) {
        if (!tree)
            return false;
        if (!tree.children || tree.children.length === 0)
            return tree.paneId === paneId;
        for (var i = 0; i < tree.children.length; ++i) {
            if (window.treeHasPane(tree.children[i], paneId))
                return true;
        }
        return false;
    }

    function regionTree(region) {
        if (!app.layouts)
            return null;
        return region === "viewer" ? app.layouts.viewerTree : app.layouts.terminalTree;
    }

    // The pane a region command targets: the user's selected pane when it is
    // actually a pane OF THIS REGION, else that region's first leaf.
    //
    // UiStateStore records ONE selected pane per Dev Session, not one per region.
    // Handing it over unchecked meant that with a terminal pane selected, "Split
    // Viewer Pane" passed a terminal paneId to the viewer tree; splitPane found
    // no such node, emitted an error and split nothing.
    function targetPaneId(region) {
        const tree = window.regionTree(region);
        const selected = (app.activeSessionId.length > 0 && app.uiState)
                       ? app.uiState.selectedPane(app.activeSessionId) : "";
        if (selected && selected.length > 0 && window.treeHasPane(tree, selected))
            return selected;
        return window.firstPaneId(tree, region === "viewer" ? "viewer-1" : "terminal-1");
    }

    // Leaf count of a split tree: how many panes a region command could have
    // meant.
    function paneCount(tree) {
        if (!tree)
            return 0;
        if (!tree.children || tree.children.length === 0)
            return 1;
        var n = 0;
        for (var i = 0; i < tree.children.length; ++i)
            n += window.paneCount(tree.children[i]);
        return n;
    }

    function splitActivePane(region, orientation) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            notifyUser(qsTr("Select a Dev Session before splitting a pane."));
            return;
        }
        // Pane focus IS tracked now: each region reports the pane the user last
        // clicked (ViewerRegion/TerminalRegion.focusedPaneId), and the handlers on
        // those regions record it via UiStateStore.setSelectedPane, so
        // targetPaneId()'s selected-pane branch is live and a split lands on the
        // pane the user was working in. Detection is click-based: focus moved
        // purely by keyboard does not report, so a future "focus next pane"
        // command must tell the region directly rather than rely on this.
        app.layouts.splitPane(region, window.targetPaneId(region), orientation);
    }

    // Close the pane the user last worked in. SessionLayouts collapses the parent
    // branch and leaves an empty leaf behind if the region ends up bare, so a
    // region is never left with nothing to show.
    function closeActivePane(region) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            notifyUser(qsTr("Select a Dev Session before closing a pane."));
            return;
        }
        app.layouts.closePane(region, window.targetPaneId(region));
    }

    // End the focused terminal's REMOTE tmux session. Closing or detaching a pane
    // deliberately leaves the remote shell running - that is what tmux is for - so
    // this is the only way to actually stop it, and it is destructive.
    function killActiveTerminal() {
        if (app.activeSessionId.length === 0) {
            notifyUser(qsTr("No active Dev Session."));
            return;
        }
        const paneId = window.targetPaneId("terminal");
        const pane = terminalRegion.paneCache ? terminalRegion.paneCache[paneId] : null;
        if (!pane) {
            notifyUser(qsTr("No live terminal pane to kill."));
            return;
        }
        pane.killSession();
        notifyUser(qsTr("Killed the remote tmux session for \"%1\".").arg(paneId));
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
        { id: "pane.close.viewer", title: qsTr("Close Focused Viewer Pane"),
          invoke: () => window.closeActivePane("viewer") },
        { id: "pane.close.terminal", title: qsTr("Close Focused Terminal Pane"),
          invoke: () => window.closeActivePane("terminal") },
        // Detaching leaves the remote tmux session running (that is the point of
        // tmux); killing it is the only way to actually end the remote shell, and
        // until now nothing in the UI could reach TerminalPaneView.killSession().
        { id: "terminal.kill", title: qsTr("Kill Focused Terminal's Remote Session"),
          invoke: () => window.killActiveTerminal() },
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
