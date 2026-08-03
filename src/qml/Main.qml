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
    color: Theme.surface

    // --- window chrome (SPEC 4.1) -------------------------------------------
    //
    // The window paints its own title bar (AppTitleBar) and its own resize
    // edges, so the desktop environment's bar — the one light strip left on an
    // otherwise fully dark, hand-painted window — is gone. That means going
    // frameless, which hands this file everything the window manager used to do:
    // moving, maximise/restore and resizing. All three are delegated to the
    // platform (ApplicationWindow.startSystemMove() / startSystemResize()), never
    // re-implemented from mouse deltas — a hand-rolled move fights the
    // compositor's own pointer grab and drifts, and on Wayland there are no
    // global window coordinates to add a delta to at all.
    //
    // EXCEPT on macOS. Qt's Cocoa plugin implements startSystemMove() but NOT
    // startSystemResize() (QCocoaWindow declares only the former), so a frameless
    // window there could be dragged around and then never resized again — far
    // worse than a plain native bar. macOS therefore keeps its native frame,
    // which follows the system's dark appearance anyway, and gets the Dev Session
    // in the window TITLE rather than in a bar of our own.
    readonly property bool customChrome: Qt.platform.os !== "osx"
    flags: window.customChrome ? (Qt.Window | Qt.FramelessWindowHint) : Qt.Window

    // The human label for the Dev Session in front of the user. AppController
    // publishes the session's ID (a server-minted UUID) and its repository root,
    // but not its name, so the repository directory's own name is the label —
    // the same string the sidebar shows as a row subtitle.
    readonly property string sessionLabel: app.activeSessionId.length > 0
                                           ? window.baseName(app.activeSessionRepoRoot)
                                           : ""

    // One application name, used by the window title AND by the title bar the
    // window draws for itself. The window title additionally carries the Dev
    // Session, because that is the string the desktop's task switcher shows.
    readonly property string appName: qsTr("CodeHarbor")

    title: window.sessionLabel.length > 0
           ? qsTr("%1 — %2").arg(window.sessionLabel).arg(window.appName)
           : window.appName

    // Last segment of a remote POSIX path, trailing slashes ignored.
    function baseName(path) {
        const trimmed = String(path).replace(/\/+$/, "");
        const cut = trimmed.lastIndexOf("/");
        return cut < 0 ? trimmed : trimmed.substring(cut + 1);
    }

    // SPEC 4.5 fallback trees for a region with no server tree yet. Declared as
    // properties with dependency-free bindings so each is constructed EXACTLY
    // ONCE and keeps a stable identity for the whole run; see the ViewerRegion /
    // TerminalRegion comment below for why identity matters.
    readonly property var viewerFallbackNode: ({ paneId: "viewer-1", url: "", children: [] })
    // TWO terminal panes, stacked one above the other, because that is what a
    // terminal region is FOR: something long-running above, something being
    // typed below. "vertical" stacks children top-to-bottom (TerminalRegion maps
    // it to SplitView's Qt.Vertical), and the even `ratios` split the height.
    // Still ONE object, constructed once: handing a region a new `node` identity
    // rebuilds its panes, which kills a live terminal.
    readonly property var terminalFallbackNode: ({
        orientation: "vertical",
        children: [{ paneId: "terminal-1", children: [] },
                   { paneId: "terminal-2", children: [] }],
        ratios: [0.5, 0.5]
    })

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
    //
    // `app.uiState` is NOT null-checked, here or at the three sites below that
    // used to check it. It is a CONSTANT Q_PROPERTY backed by a UiStateStore
    // built in AppController's own initialiser list (src/app/AppController.cpp),
    // so it exists for as long as `app` does and can never become null. The
    // guards were doing nothing except advertising a nullability the type does
    // not have — which is worse than no guard, because the next reader has to
    // go and check. `app.layouts` is a genuinely different case: it is an
    // INJECTED QPointer that is null until setServices() runs, and every one of
    // its uses is checked.
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

    // The self-drawn title bar. Hidden (and the native frame kept) where
    // frameless is the wrong trade — see `customChrome` above.
    AppTitleBar {
        id: titleBar
        objectName: "titleBar"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: window.customChrome
        height: titleBar.visible ? titleBar.implicitHeight : 0
        win: window
        // The production context supplies the Windows-only native style and
        // hit-test bridge. `typeof` keeps headless QML fixtures, which mirror
        // the older context-property set, on the ordinary portable path.
        nativeHelper: typeof windowChrome === "undefined" ? null : windowChrome
        title: window.appName
        sessionLabel: window.sessionLabel
    }

    SplitView {
        id: outer
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        orientation: Qt.Horizontal
        handle: AppSplitHandle {}

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
            // The status footer exposes this control at every connection state.
            // Reopen the sheet so saved servers can be edited or selected again.
            onServerSettingsRequested: connectSheet.shown = true
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
            id: viewerRegion
            objectName: "viewerRegion"
            node: (app.layouts && app.layouts.viewerTree)
                  ? app.layouts.viewerTree
                  : window.viewerFallbackNode
            // Production writes carry the session/load stamp captured by the
            // region or pane. The region's legacy signals remain test-only.
            sessionId: app.activeSessionId
            layoutGeneration: app.layouts ? app.layouts.generation : 0
            hostStampsWrites: true
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
            // The region header's own split/close buttons, and every pane's
            // header close button, arrive here: a region cannot publish a tree,
            // so it raises a REQUEST and the host runs the same command the
            // palette does. Without these handlers those buttons are drawn,
            // hovered, pressed — and do nothing at all.
            onSplitRequested: (orientation) => window.splitActivePane("viewer", orientation)
            onClosePaneRequested: (paneId) => window.closeRequestedPane("viewer", paneId)
            // A divider INSIDE the region was dragged. Persisted per Dev Session
            onSplitRequestedForSession: (sessionId, generation, orientation) =>
                window.splitActivePaneForSession("viewer", sessionId,
                                                 generation, orientation)
            onClosePaneRequestedForSession: (sessionId, generation, paneId) =>
                window.closeRequestedPaneForSession("viewer", sessionId,
                                                    generation, paneId)
            onSplitRatiosAdjustedForSession: (sessionId, generation,
                                              pathIndexes, ratios) =>
                window.persistSplitRatiosForSession("viewer", sessionId,
                                                    generation, pathIndexes,
                                                    ratios)
            onPaneUrlReportedForSession: (sessionId, generation, paneId, url) =>
                window.persistPaneUrlForSession(sessionId, generation,
                                                "viewer", paneId, url)
            // (SPEC 4.5) so the proportions the user set survive a reopen; the
            // regions already restore them.
            onSplitRatiosAdjusted: (pathIndexes, ratios) =>
                window.persistSplitRatios("viewer", pathIndexes, ratios)
            // The region reports the pane the user last interacted with; record
            // it so pane commands act on THAT pane instead of the region's first
            // leaf. The empty value is deliberately NOT filtered: a focused pane
            // that was closed must clear the selection rather than leave a
            // command pointing at a pane that no longer exists.
            onFocusedPaneIdChanged: if (app.activeSessionId.length > 0)
                                        app.uiState.setSelectedPane(app.activeSessionId,
                                                                    focusedPaneId)
            // Persist WHAT a pane is showing, so reopening a Dev Session restores
            // the files the user had open instead of a set of blank panes.
            // setPaneUrl deliberately does not re-publish the tree, so recording
            // this cannot rebuild the very pane that just opened the file.
            onPaneUrlReported: (paneId, url) => {
                if (app.layouts && app.activeSessionId.length > 0)
                    app.layouts.setPaneUrl("viewer", paneId, url);
            }
            onOpenInNewPaneRequested: (paneId, url, kind) =>
                window.openViewerInNewPane(paneId, url, kind)
            onMessageRequested: (message) => window.notifyUser(message)
            onOpenInNewPaneRequestedForSession: (sessionId, generation,
                                                 paneId, url, kind) =>
                window.openViewerInNewPaneForSession(sessionId, generation,
                                                    paneId, url, kind)
        }

        TerminalRegion {
            id: terminalRegion
            objectName: "terminalRegion"
            node: (app.layouts && app.layouts.terminalTree)
                  ? app.layouts.terminalTree
                  : window.terminalFallbackNode
            // devSessionId/workingDir are pushed as an ORDERED PAIR by
            // window.retargetTerminals(), not bound here; see that function.
            layoutGeneration: app.layouts ? app.layouts.generation : 0
            hostStampsWrites: true
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
            onFocusedPaneIdChanged: if (app.activeSessionId.length > 0)
                                        app.uiState.setSelectedPane(app.activeSessionId,
                                                                    focusedPaneId)
            onSplitRequested: (orientation) => window.splitActivePane("terminal", orientation)
            onClosePaneRequested: (paneId) => window.closeRequestedPane("terminal", paneId)
            onSplitRequestedForSession: (sessionId, generation, orientation) =>
                window.splitActivePaneForSession("terminal", sessionId,
                                                 generation, orientation)
            onClosePaneRequestedForSession: (sessionId, generation, paneId) =>
                window.closeRequestedPaneForSession("terminal", sessionId,
                                                    generation, paneId)
            onSplitRatiosAdjustedForSession: (sessionId, generation,
                                              pathIndexes, ratios) =>
                window.persistSplitRatiosForSession("terminal", sessionId,
                                                    generation, pathIndexes,
                                                    ratios)
            onPaneTitleReportedForSession: (sessionId, generation, paneId, title) =>
                app.layouts.setPaneTitleForSession(sessionId, generation,
                                                   "terminal", paneId, title)
            // Destructive, and the only route to it besides the palette command.
            onKillTerminalRequested: window.killActiveTerminal()
            onSplitRatiosAdjusted: (pathIndexes, ratios) =>
                window.persistSplitRatios("terminal", pathIndexes, ratios)
            // Persist the display title without republishing the tree: the
            // terminal item already applied it locally, and its identity is
            // still the unchanged server row id in the leaf. Stamp the write
            // with the current session/generation so a delayed header event
            // cannot edit a newly selected session.
            onPaneTitleReported: (paneId, title) => {
                if (app.layouts && app.activeSessionId.length > 0)
                    app.layouts.setPaneTitleForSession(app.activeSessionId,
                                                       app.layouts.generation,
                                                       "terminal", paneId, title);
            }
        }
    }

    // --- resize edges (SPEC 4.1) --------------------------------------------
    //
    // A frameless window has no resize border, so the eight grab strips a window
    // manager would have drawn are declared here, over everything else. Each one
    // hands the drag straight to the platform via startSystemResize(edges); the
    // corners pass BOTH of their edges, which is what makes a corner resize two
    // axes at once.
    //
    // Declared AFTER the regions so they hit-test above them (Quick tries the
    // last sibling first), and the four corners come after the four edges for the
    // same reason: at a corner the corner strip must win.
    //
    // Off entirely when the window is not resizable by dragging: while maximised,
    // and on the platforms that keep their native frame.
    Repeater {
        model: [
            // h/v: -1 = the left/top edge, 1 = the right/bottom edge, 0 = spans.
            { h: 0, v: -1, edges: Qt.TopEdge, cursor: Qt.SizeVerCursor },
            { h: 0, v: 1, edges: Qt.BottomEdge, cursor: Qt.SizeVerCursor },
            { h: -1, v: 0, edges: Qt.LeftEdge, cursor: Qt.SizeHorCursor },
            { h: 1, v: 0, edges: Qt.RightEdge, cursor: Qt.SizeHorCursor },
            { h: -1, v: -1, edges: Qt.LeftEdge | Qt.TopEdge, cursor: Qt.SizeFDiagCursor },
            { h: 1, v: 1, edges: Qt.RightEdge | Qt.BottomEdge, cursor: Qt.SizeFDiagCursor },
            { h: 1, v: -1, edges: Qt.RightEdge | Qt.TopEdge, cursor: Qt.SizeBDiagCursor },
            { h: -1, v: 1, edges: Qt.LeftEdge | Qt.BottomEdge, cursor: Qt.SizeBDiagCursor }
        ]

        delegate: MouseArea {
            id: resizeGrip
            required property var modelData

            // A corner needs a bigger target than an edge: it is aimed at, and it
            // is the only way to change both dimensions in one drag.
            readonly property int extent: (resizeGrip.modelData.h !== 0
                                           && resizeGrip.modelData.v !== 0)
                                          ? Theme.splitHandleThickness * 2
                                          : Theme.splitHandleThickness

            z: 2000
            enabled: window.customChrome && window.visibility === Window.Windowed
            cursorShape: resizeGrip.modelData.cursor
            acceptedButtons: Qt.LeftButton

            x: resizeGrip.modelData.h < 0
               ? 0
               : (resizeGrip.modelData.h > 0 ? parent.width - resizeGrip.extent : 0)
            y: resizeGrip.modelData.v < 0
               ? 0
               : (resizeGrip.modelData.v > 0 ? parent.height - resizeGrip.extent : 0)
            width: resizeGrip.modelData.h === 0 ? parent.width : resizeGrip.extent
            height: resizeGrip.modelData.v === 0 ? parent.height : resizeGrip.extent

            onPressed: window.startSystemResize(resizeGrip.modelData.edges)
        }
    }

    // Non-blocking error banner: surfaces app.error (RPC failures forwarded
    // verbatim, SPEC 10.3) as a transient toast so shell-level failures are
    // visible instead of silently swallowed.
    Rectangle {
        id: errorBanner
        objectName: "shellErrorBanner"
        z: 1000
        visible: opacity > 0
        opacity: 0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 12
        width: Math.min(errorLabel.implicitWidth + 32, parent.width - 24)
        height: errorLabel.implicitHeight + 20
        radius: Theme.radiusMedium
        color: Theme.danger

        Behavior on opacity { NumberAnimation { duration: 200 } }

        Label {
            id: errorLabel
            objectName: "shellErrorLabel"
            anchors.centerIn: parent
            width: parent.width - 32
            // SECURITY: a Label defaults to Text.AutoText, which promotes any
            // string that merely LOOKS like markup to StyledText — and
            // StyledText fetches <img src="http://..."> and turns <a href> into
            // a live link. What lands here is app.error / SessionLayouts.error,
            // i.e. RPC and libssh failure text forwarded VERBATIM from the
            // server (SPEC 10.3), so a hostile server must not be able to turn
            // this toast into a network callback. Same rule as every other
            // server-fed Label in this module.
            textFormat: Text.PlainText
            color: Theme.textOnAccent
            font.pixelSize: Theme.fontSizeLabel
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

    // Layout failures reach the user too. Without this every SessionLayouts
    // error is silent - "layout not loaded", "no pane X to close", a failed
    // persist - so Close Pane or Split Pane would simply appear to do nothing.
    Connections {
        target: app.layouts
        enabled: app.layouts !== null
        function onError(message) {
            window.notifyUser(message);
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
        diagnosticText: app.sshDiagnostics

        onConnectRequested: (profileId) => app.connectToProfile(profileId)
        onUpgradeRequested: (profileId) => app.upgradeRemoteService(profileId)
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
        onCredentialSubmitted: (secret, kind) => {
            // Cleared here BEFORE the secret is handed over, so the sheet is
            // never left holding a prompt for an answer already spent.
            pendingCredential = null;
            app.submitCredential(secret, kind);
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

    LogView {
        id: logView
        anchors.fill: parent
        z: 900
        visible: shown
        property bool shown: false
        logBuffer: app.logBuffer
        onDismissed: shown = false
    }
    SettingsWindow {
        id: settingsWindow
        anchors.fill: parent
        z: 910
        visible: shown
        property bool shown: false
        onDismissed: shown = false
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
        function onCredentialPrompt(user, host, prompt, kind) {
            connectSheet.pendingCredential = {
                user: user, host: host, prompt: prompt, kind: kind
            };
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
            return window.firstPaneId(tree.children[0], fallback);
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
        const selected = app.activeSessionId.length > 0
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

    // Open a directory target in a NEW viewer pane. SessionLayouts remains the
    // sole creator of split leaves; ViewerRegion only applies the target to the
    // freshly published pane so the original pane's content and identity stay
    // untouched.
    function openViewerInNewPane(sourcePaneId, url, kind) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            window.notifyUser(qsTr("Select a Dev Session before opening a new pane."));
            return;
        }
        const tree = window.regionTree("viewer");
        const source = sourcePaneId && window.treeHasPane(tree, sourcePaneId)
                       ? sourcePaneId : window.targetPaneId("viewer");
        const newPaneId = app.layouts.splitPane("viewer", source, "horizontal");
        if (!newPaneId)
            return;
        if (!viewerRegion.openPaneTarget(newPaneId, url, kind)) {
            window.notifyUser(qsTr("Could not open that target in a new pane."));
            return;
        }
        viewerRegion.noteFocus(newPaneId);
    }

    function openViewerInNewPaneForSession(sessionId, generation, sourcePaneId,
                                           url, kind) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        const tree = window.regionTree("viewer");
        const source = sourcePaneId && window.treeHasPane(tree, sourcePaneId)
                       ? sourcePaneId : window.firstPaneId(tree, "viewer-1");
        const newPaneId = app.layouts.splitPaneForSession(
            sessionId, generation, "viewer", source, "horizontal");
        if (!newPaneId)
            return;
        if (String(sessionId) !== String(app.activeSessionId))
            return; // C++ normally drops this; never touch a different cache.
        if (!viewerRegion.openPaneTarget(newPaneId, url, kind)) {
            window.notifyUser(qsTr("Could not open that target in a new pane."));
            return;
        }
        viewerRegion.noteFocus(newPaneId);
    }
    // Stamped region callbacks are the production path. C++ drops a callback
    // whose session or generation no longer matches, before it can target a
    // same-named pane in the newly selected session.
    function splitActivePaneForSession(region, sessionId, generation, orientation) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        app.layouts.splitPaneForSession(sessionId, generation, region,
                                        window.targetPaneId(region), orientation);
    }

    function closeActivePaneForSession(region, sessionId, generation) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        app.layouts.closePaneForSession(sessionId, generation, region,
                                        window.targetPaneId(region));
    }

    function closeRequestedPaneForSession(region, sessionId, generation, paneId) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        if (paneId.length === 0) {
            window.closeActivePaneForSession(region, sessionId, generation);
            return;
        }
        app.layouts.closePaneForSession(sessionId, generation, region, paneId);
    }

    function persistSplitRatiosForSession(region, sessionId, generation,
                                          pathIndexes, ratios) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        app.layouts.setRatiosForSession(sessionId, generation, region,
                                        pathIndexes, ratios);
    }

    function persistPaneUrlForSession(sessionId, generation, region, paneId, url) {
        if (!app.layouts || String(sessionId).length === 0)
            return;
        app.layouts.setPaneUrlForSession(sessionId, generation, region,
                                         paneId, url);
    }


    function splitActivePane(region, orientation) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            window.notifyUser(qsTr("Select a Dev Session before splitting a pane."));
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
            window.notifyUser(qsTr("Select a Dev Session before closing a pane."));
            return;
        }
        app.layouts.closePane(region, window.targetPaneId(region));
    }

    // Close the pane a REGION asked about. A pane's own header close button names
    // itself, and that name wins: the user pressed the button on THAT pane, which
    // is not necessarily the pane the region last reported as focused (the button
    // reports focus first, but a pane whose id is empty cannot).
    //
    // An empty paneId is the region header's "whichever pane you would pick
    // anyway", and it is also what the placeholder leaf of an emptied region
    // reports; both are served correctly by falling back to the focused/first
    // pane, which for a tree holding only the placeholder IS that placeholder.
    function closeRequestedPane(region, paneId) {
        if (!app.layouts || app.activeSessionId.length === 0) {
            window.notifyUser(qsTr("Select a Dev Session before closing a pane."));
            return;
        }
        if (paneId.length === 0) {
            window.closeActivePane(region);
            return;
        }
        app.layouts.closePane(region, paneId);
    }

    // Persist the split proportions a drag inside a region produced (SPEC 4.5).
    // setRatios is deliberately quiet — it does NOT re-publish the tree — so
    // recording a drag cannot rebuild the panes the drag just resized.
    function persistSplitRatios(region, pathIndexes, ratios) {
        if (!app.layouts || app.activeSessionId.length === 0)
            return;
        app.layouts.setRatios(region, pathIndexes, ratios);
    }

    // End the focused terminal's REMOTE tmux session. Closing or detaching a pane
    // deliberately leaves the remote shell running - that is what tmux is for - so
    // this is the only way to actually stop it, and it is destructive.
    function killActiveTerminal() {
        if (app.activeSessionId.length === 0) {
            window.notifyUser(qsTr("No active Dev Session."));
            return;
        }
        const paneId = window.targetPaneId("terminal");
        const pane = terminalRegion.paneCache ? terminalRegion.paneCache[paneId] : null;
        if (!pane) {
            window.notifyUser(qsTr("No live terminal pane to kill."));
            return;
        }
        // killSession() reports whether the kill actually reached the server: it
        // deliberately refuses (and keeps the remote session alive) when there is
        // no SSH connection, so announcing success unconditionally — which this
        // used to do — told the user their processes were gone while they were
        // still running. The pane's own sentence is the better message in that
        // case, because it names the session that survived.
        if (pane.killSession()) {
            window.notifyUser(qsTr("Killed the remote tmux session for \"%1\".").arg(paneId));
        } else {
            window.notifyUser(pane.statusText.length > 0
                              ? pane.statusText
                              : qsTr("Could not kill the remote session for \"%1\".").arg(paneId));
        }
    }

    readonly property var paletteCommands: [
        { id: "server.connect", title: qsTr("Connect to Server…"), shortcut: "Ctrl+Shift+O",
          invoke: () => { connectSheet.shown = true; } },
        { id: "server.disconnect", title: qsTr("Disconnect from Server"),
          invoke: () => app.disconnectServer() },
        { id: "app.logs", title: qsTr("Show Log"),
          invoke: () => { logView.shown = true; } },
        { id: "app.settings", title: qsTr("Settings…"), shortcut: "Ctrl+,",
          invoke: () => { settingsWindow.shown = true; } },
        // No profile argument: the controller uses the active profile, which is
        // the one the palette user is looking at in the sidebar.
        { id: "server.upgrade", title: qsTr("Update Remote Service on Server"),
          invoke: () => app.upgradeRemoteService("") },
        // Ctrl+Shift+R, not Ctrl+R: a global Shortcut is matched before the key
        // reaches the focused item, and Ctrl+R is reverse-history-search in every
        // shell a terminal pane hosts (and reload in a WebEngineView). Same rule
        // as Ctrl+Shift+W below.
        { id: "session.refresh", title: qsTr("Refresh Workspace"), shortcut: "Ctrl+Shift+R",
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
          } },
        // A frameless window has no window-manager menu of its own, so maximise
        // and close must be reachable without the pointer too. Ctrl+Shift+W
        // rather than the platform's Ctrl+W: every terminal pane is a real
        // shell, and Ctrl+W is delete-word in all of them.
        { id: "window.maximise", title: qsTr("Toggle Maximised Window"),
          invoke: () => titleBar.toggleMaximised() },
        { id: "window.close", title: qsTr("Close Window"), shortcut: "Ctrl+Shift+W",
          invoke: () => window.close() }
    ]

    CommandPalette {
        id: commandPalette
        commands: window.paletteCommands
    }
}
