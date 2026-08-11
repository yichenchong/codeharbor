import QtQuick
import QtQuick.Layouts
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
//     tree. With `paneId` they name the pane's layout slot, which the factory
//     resolves to the server row holding this pane's tmux target (SPEC 5.2);
//     `workingDir` is the cwd a newly created session is rooted at.
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
    // Which Dev Session this pane belongs to, and which LAYOUT SLOT of it this
    // pane occupies (SPEC 4.5). `terminalId` is the layout pane id —
    // "terminal-1", "terminal-2", … from ch::SessionLayouts — and that is ALL
    // it is: a slot LABEL. It is not the terminal's identity, it is recycled
    // (closing a pane frees the label while its remote shell keeps running),
    // and the tmux target is never built from it.
    property string devSessionId: ""
    property string terminalId: pane.paneId
    // Optional user-chosen title stored in the terminal layout leaf. It never
    // participates in slot lookup or tmux identity: an empty value falls back
    // to the client-generated terminalId label.
    property string customTitle: ""
    // Keep this in lock-step with SplitNode::kMaxCustomTitleLength. The cap is
    // intentionally small because this compact header should remain readable
    // and layout payloads should not grow without bound.
    readonly property int maxCustomTitleLength: 128

    function normalizeCustomTitle(value) {
        var title = String(value === undefined || value === null ? "" : value).trim()
        return title.length > pane.maxCustomTitleLength
               ? title.substring(0, pane.maxCustomTitleLength) : title
    }

    function beginRename() {
        pane.paneActivated(pane.paneId)
        renameDialog.open()
    }

    // The pane's close control. Closing a terminal pane now ENDS its remote
    // tmux session too, so it is confirmed first. Activation still happens
    // before the dialog opens, so the host records this pane as the source of
    // the destructive action.
    function beginClose() {
        pane.paneActivated(pane.paneId)
        killDialog.open()
    }

    // Main.qml uses the same entry point for a palette request and a pane
    // header request. Keeping the confirmation here prevents the palette from
    // silently turning the destructive action into a layout-only close.
    function requestClose() {
        pane.beginClose()
    }

    // What the confirmation actually does, in the order that matters. The kill
    // request goes FIRST, while this pane is still in its region's cache and
    // still owns the controller Main.qml has to reach the server through; the
    // close request that follows republishes the layout and takes the pane
    // away. Reversed, the host would look up a pane that no longer exists and
    // the remote session would survive a close the user was told was final.
    function closeAndKill() {
        pane.killRequested(pane.paneId)
        pane.closeRequested(pane.paneId)
    }

    function commitRename(value) {
        const title = pane.normalizeCustomTitle(value)
        // Update the live pane before the persistence request: the user should
        // not wait for a remote round trip to see the title they just entered.
        pane.customTitle = title
        pane.titleChangedRequested(pane.paneId, title)
    }
    property string workingDir: ""

    // The terminal's REAL identity: the id of its row in the server's
    // `terminal_panes` table, carried in this pane's layout leaf
    // (SplitNode::terminalPaneId) and minted by the server when the leaf was
    // created. Never recycled, shared through the stored layout, so every
    // client agrees which shell this pane owns.
    //
    // Empty means one of exactly two things, and `terminalLegacy` is what tells
    // them apart:
    //
    //   * legacy — this leaf was stored before layouts carried row ids. The
    //     slot label genuinely IS its historical key, so it may be resolved by
    //     label ONCE; ch::SessionLayouts then writes the answer into the leaf.
    //
    //   * otherwise — the row is still being minted for a pane that was just
    //     created. The pane attaches NOTHING until the id arrives. Falling back
    //     to the label here is precisely how a new pane used to adopt a closed
    //     pane's still-running shell, so the wait is deliberate and the safe
    //     default is to wait: an unset property leaves a pane visibly stuck
    //     rather than silently attached to the wrong terminal.
    property string terminalPaneId: ""
    property bool terminalLegacy: false

    // The remote tmux session this pane attaches to, as minted by the server
    // (SPEC 5.2). Empty until ch::TerminalFactory::resolveTarget() answers, and
    // cleared whenever the pane is pointed at a different slot or Dev Session.
    //
    // Never composed here, and deliberately not composable here: layout pane
    // ids recycle within a Dev Session, so a client that built its own target
    // out of one could hand a brand new pane the name of a shell another client
    // left running — and `tmux new-session -A` would silently attach it.
    property string tmuxTarget: ""
    // A resolveTarget() round trip is outstanding. Its answer arrives on the
    // factory's targetResolved signal, which re-enters attachNow().
    property bool resolving: false
    // The slot the outstanding resolution was started FOR, as
    // "<devSessionId>/<terminalId>/<terminalPaneId>". A pane can be pointed at
    // another Dev Session while its lookup is still travelling, and the answer
    // to the old question must never be adopted as the new slot's target —
    // which is two panes on one shell, the exact failure this whole path
    // removes.
    property string resolvingFor: ""
    // The pane was retargeted while that lookup was in flight, so the answer
    // still travelling names a slot this pane has LEFT.
    //
    // Comparing `resolvingFor` against slotKey() cannot catch that on its own.
    // If a retarget were allowed to ask its new question straight away, TWO
    // lookups would be outstanding at once, `resolvingFor` would already hold
    // the NEW key, and the OLD answer — which carries nothing but a controller
    // and a target string — would sail through the comparison and attach this
    // pane to the previous Dev Session's shell. So exactly ONE lookup is ever
    // outstanding: a retarget marks the flight stale instead of starting a
    // second one, and the answer, once it lands, asks the current question.
    property bool resolveStale: false

    // ---- focus reporting (SPEC 4.5) ----
    // The user is working in THIS pane. TerminalRegion connects this when it
    // mints the pane (takePane) and republishes it as the root region's
    // `focusedPaneId`; that is what the palette's split/close commands target,
    // instead of guessing at the region's first leaf.
    signal paneActivated(string paneId)

    // This is the pane the user is working in. Written by the region that owns
    // the pane cache (TerminalRegion.applyFocusFlags) rather than derived here:
    // focus is a property of the REGION — exactly one of its panes has it — and
    // a pane cannot see its siblings.
    property bool paneActive: false

    // Pane-header requests carry this pane's id all the way to Main.qml.
    // Closing a terminal pane is destructive: it kills the remote tmux session
    // AND removes the pane, which is why closeAndKill() emits both and is
    // reached only from the confirmation dialog below.
    //
    // That dialog is the ONLY emitter of `killRequested` in this file, and that
    // is the safety boundary: every other way a pane can disappear — the
    // application window closing, a Dev Session switch, a layout republished or
    // repaired by the server, a region or group closed from the sidebar, a
    // dropped or reconnecting SSH connection, a Loader tearing its pane down —
    // arrives at Component.onDestruction, which calls detachNow() and leaves
    // the remote session running. Killing must stay wired to the button press,
    // never to pane destruction.
    signal splitRequested(string paneId, string orientation)
    signal closeRequested(string paneId)
    signal killRequested(string paneId)

    // The host persists the proposed title in SessionLayouts. The pane also
    // applies the normalized value locally so the header changes before the
    // asynchronous layout write returns.
    signal titleChangedRequested(string paneId, string title)

    // The `terminalFactory` context property, resolved once. Guarded so a host
    // that does not install it (a bare QML load) gets inert chrome rather than
    // a ReferenceError; also the seam a test injects a stub through.
    property var factory: (typeof terminalFactory !== "undefined") ? terminalFactory : null

    // The pane's OTHER injected object, `viewers` (ch::ViewerModel), which
    // supplies the privileged WebEngine profile the WebChannel bridge needs.
    // Guarded the same way, for the same reason.
    property var viewerModel: (typeof viewers !== "undefined") ? viewers : null
    // Mint the controller and bridge once, rather than binding creation to
    // mutable properties. A factory or session binding can settle again while
    // a pane is being rehomed; a reactive create() would orphan the live
    // controller (and its PTY) and leave the WebChannel pointing at a
    // different object.
    property var controller: null
    property var bridge: null
    property bool started: false
    property bool bridgeRegistered: false

    function initializeController() {
        if (!pane.controller && pane.factory)
            pane.controller = pane.factory.create(pane)
        if (!pane.bridge && pane.controller && pane.factory)
            pane.bridge = pane.factory.createBridge(pane.controller, pane)
        if (pane.bridge && !pane.bridgeRegistered) {
            terminalChannel.registerObject("terminal", pane.bridge)
            pane.bridgeRegistered = true
            viewLoader.active = true
        }
    }

    onFactoryChanged: if (pane.started) pane.initializeController()
    property var settingsObject: (typeof app !== "undefined" && app && app.settings)
                                 ? app.settings : null

    // A restore may arrive before the xterm page has mounted. Keep the request
    // until the page is ready, but cancel it when the region gives focus to a
    // different pane so a late load cannot fight the user's click.
    property bool focusPending: false
    onPaneActiveChanged: if (!pane.paneActive) pane.focusPending = false



    // Entry page of the trusted terminal bundle; overridable by a host or test
    // that wants to serve the bundle from elsewhere.
    property url terminalBundleUrl: "qrc:/codeharbor/web/terminal/index.html"

    // ---- observable pane state ----
    readonly property string connectionState: pane.bridge ? pane.bridge.connectionState : "unloaded"
    property bool attached: false
    property bool pageLoaded: false
    // Human-readable reason the pane is not live; shown instead of a blank pane.
    // Whole sentences, because this is the only thing on screen when a terminal
    // fails to come up — "unloaded" is a state name, not an explanation.
    property string statusText: pane.factory ? qsTr("Not attached to a shell yet.")
                                             : qsTr("No terminal service in this window.")
    // Live means: a PTY is attached AND the renderer is showing it.
    readonly property bool live: pane.attached && pane.pageLoaded
                                 && pane.connectionState === "ready"

    // This pane has no shell to attach and no way to name one: no tmux target
    // yet, no `terminal_panes` row id, and no permission to fall back to its
    // slot label. Somebody is expected to be minting a row for it right now
    // (ch::SessionLayouts does, for every terminal leaf it creates), and the id
    // arrives through `terminalPaneId`.
    //
    // A FUNCTION, deliberately, and not a derived property. The one caller that
    // must never be wrong is onTerminalPaneIdChanged -> attachNow(), which runs
    // from inside the notification for `terminalPaneId` itself: a bound property
    // reading the same three inputs is refreshed by that same notification, and
    // whether it has been refreshed BEFORE this handler runs is an ordering
    // detail of the QML engine, not something this pane may depend on. Getting
    // it wrong strands the pane for ever - it declines to attach because it
    // still believes it has no identity, and nothing calls it again. Evaluated
    // on demand, it cannot be stale.
    function awaitingIdentity() {
        return pane.tmuxTarget.length === 0 && pane.terminalPaneId.length === 0
               && !pane.terminalLegacy
    }
    // The wait above ran out. Waiting is the RIGHT thing to do — resolving by
    // the recyclable slot label instead is the defect this whole scheme removes
    // — but it must not be unbounded AND silent. A mint that never answers, or
    // a host that mints nothing at all, would otherwise leave the pane saying
    // "setting up" for ever, with no way for the user to tell that nothing is
    // coming and no Retry that could help (this pane genuinely has no identity
    // and cannot invent one; only the host can mint another row). So the wait
    // is bounded and then REPORTED. Still recoverable: a late id fires
    // onTerminalPaneIdChanged and the pane attaches after all.
    property bool identityStalled: false
    Timer {
        id: identityWatchdog
        // Generous next to a mint's single round trip, so a merely slow server
        // never trips it; short enough that a pane that will never come up says
        // so while the user is still looking at it.
        interval: 20000
        repeat: false
        onTriggered: {
            pane.identityStalled = true
            pane.statusText = qsTr("This terminal has no identity on the server yet, so there "
                                   + "is nothing safe to attach to. Close the pane and split "
                                   + "again once the server is reachable.")
        }
    }

    // Leave the waiting state: the id arrived, or the pane was pointed
    // somewhere else. attachNow() restarts the watchdog if the pane is still
    // waiting after all.
    function clearIdentityWait() {
        identityWatchdog.stop()
        pane.identityStalled = false
    }

    // The pane is on its way up. `attaching` alone cannot answer this: it brackets
    // the SYNCHRONOUS factory.attach() call, so it is true for a blink and never
    // observably so. The slow part is the CONTROLLER's — opening the SSH channel
    // and running `tmux new-session -A` on it, which is where its multi-second
    // budget goes — and those are the two states it publishes while doing it
    // (ch::toString(TerminalState), src/models/SessionState.cpp). `resolving` is
    // the step before all of them: the server round trip that tells this pane
    // WHICH tmux session is its own.
    readonly property bool comingUp: pane.attaching
                                     || pane.resolving
                                     // Spelled out rather than calling
                                     // awaitingIdentity(): a binding must name
                                     // the properties it depends on, or it is
                                     // never re-evaluated when they change.
                                     || (pane.tmuxTarget.length === 0
                                         && pane.terminalPaneId.length === 0
                                         && !pane.terminalLegacy
                                         && !pane.identityStalled)
                                     || pane.connectionState === "opening_channel"
                                     || pane.connectionState === "attaching_tmux"

    // The pane's state in ONE word, for the header. Deliberately not
    // `connectionState` verbatim: that publishes machine words ("unloaded",
    // "opening_channel") which mean nothing beside a terminal's name. The whole
    // sentence still exists — `statusText`, drawn in the placeholder and the
    // banner below — and this is the glance version of it.
    //
    // "attaching…" is the shell coming up; "starting…" is the shell already up
    // (or not started yet) with the RENDERER still loading, which is the other
    // half of `live`.
    readonly property string stateLabel: pane.live ? qsTr("live")
        : pane.comingUp ? qsTr("attaching\u2026")
        : pane.attached ? qsTr("starting\u2026")
        : pane.connectionState === "error" ? qsTr("error")
        : pane.connectionState === "disconnected" ? qsTr("disconnected")
        : qsTr("not attached")

    color: Theme.surfaceSunken
    border.color: Theme.borderSubtle
    border.width: 1
    implicitWidth: 160
    implicitHeight: 100

    // True only while pane.factory.attach() is running. `attached` cannot serve
    // as the guard against re-entering attachNow(): it is assigned from
    // attach()'s RETURN value, so it is still false for the whole duration of
    // the call. Anything the C++ side emits synchronously from inside attach()
    // that lands back here — bridge.geometryChanged is wired to attachNow()
    // below — would therefore open a SECOND PTY for this pane and leak the
    // first. The bridge is documented not to emit that signal from inside
    // attach(), and this is the belt to that braces: the invariant is enforced
    // on the QML side too, so a future change on either side cannot reintroduce
    // the double attach silently. Not part of the pane's observable contract.
    property bool attaching: false

    // The user ended this pane's remote session ON PURPOSE. Nothing may bring
    // it back except the user asking again: the attach command is
    // `tmux new-session -A`, which CREATES the session when it is missing, so a
    // single automatic attachNow() — the renderer's next refit, a visibility
    // change, a reconnect — would silently resurrect exactly what they just
    // destroyed. Lifted by reconnectNow(), which is what this pane's Connect
    // and Retry controls call, and by retarget(), which points the pane at a
    // different terminal altogether.
    property bool sessionKilled: false

    // The identity an outstanding resolution was started FOR: Dev Session, slot
    // label and row id. Used to tell an answer meant for what this pane is NOW
    // from one meant for what it was when the question was asked.
    function slotKey() {
        return pane.devSessionId + "/" + pane.terminalId + "/" + pane.terminalPaneId
    }

    // Open (or re-open) this pane's PTY. Idempotent while attached; safe to call
    // from the host whenever the session or the SSH connection changes.
    //
    // TWO phases, because a terminal's identity lives on the server. The first
    // call for a pane resolves its `terminal_panes` row to a tmux target and
    // returns; the factory's targetResolved signal calls this function again
    // with `tmuxTarget` filled in, and that call does the attaching. Once
    // resolved, the target stays on the pane, so a reconnect or a Retry goes
    // straight to the attach.
    function attachNow() {
        if (pane.attaching || pane.attached || pane.resolving || pane.sessionKilled
                || !pane.factory || !pane.controller)
            return
        if (pane.devSessionId.length === 0 || pane.terminalId.length === 0) {
            pane.statusText = qsTr("No Dev Session selected for this pane.")
            return
        }
        if (!pane.factory.connected()) {
            pane.statusText = qsTr("Not connected to a server.")
            return
        }
        if (pane.tmuxTarget.length === 0) {
            if (pane.awaitingIdentity()) {
                // This pane's row is still being minted. Resolving by the slot
                // label instead would be the old, unsafe key: labels are reused
                // across clients, and `tmux new-session -A` would silently hand
                // this brand new pane a shell some closed pane left running.
                // ch::SessionLayouts republishes the tree with the id in it, and
                // onTerminalPaneIdChanged brings us straight back here.
                //
                // Bounded, not for ever: attachNow() is re-entered from several
                // handlers, so the watchdog is only STARTED, never restarted,
                // and the window is measured from the first attempt.
                if (!pane.identityStalled) {
                    if (!identityWatchdog.running)
                        identityWatchdog.start()
                    pane.statusText = qsTr("Setting up this terminal on the server\u2026")
                }
                return
            }
            // Phase one. The factory answers on targetResolved (always
            // asynchronously, so `resolving` is set before the answer can
            // arrive); a false return means it refused outright and has already
            // written the reason through error(). An EMPTY terminalPaneId here
            // is the legacy case checked just above: resolve by label, once.
            pane.resolvingFor = pane.slotKey()
            pane.resolving = pane.factory.resolveTarget(pane.controller, pane.devSessionId,
                                                        pane.terminalId, pane.terminalPaneId,
                                                        pane.workingDir)
            if (pane.resolving)
                pane.statusText = qsTr("Finding this terminal on the server\u2026")
            return
        }
        // The renderer's fitted size when it has already mounted; 0 lets the
        // factory open at the conventional 80x24 until the first fit arrives.
        const cols = pane.bridge ? pane.bridge.columns : 0
        const rows = pane.bridge ? pane.bridge.rows : 0
        // try/finally, not two plain assignments: attach() reaching into libssh
        // can throw back into QML, and a guard left stuck at true would make the
        // pane permanently unattachable — including through its own Retry button.
        pane.attaching = true
        try {
            pane.attached = pane.factory.attach(pane.controller, pane.tmuxTarget,
                                                pane.workingDir, cols, rows)
        } finally {
            pane.attaching = false
        }
        if (pane.attached) {
            pane.statusText = ""
            return
        }
        // REFUSED. The factory only lets a pane attach a target IT resolved for
        // THIS controller (ch::TerminalFactory::attach), and it drops that
        // authorization whenever the pane's remote session is killed or the
        // client is pointed at another server. `tmuxTarget` outlives the
        // authorization, and the branch above treats a non-empty target as the
        // whole answer — so a pane that keeps one re-offers the same dead name
        // for ever and is refused every single time, its own Retry button
        // included. That is a terminal with no shell, no keyboard and no way
        // back, which is worse than the round trip this costs.
        //
        // Dropping the target sends the NEXT attempt through resolveTarget(),
        // which asks the server for this pane's ROW again. It is emphatically
        // not the slot-label fallback: a leaf with no `terminal_panes` row id
        // and no legacy marker still waits (awaitingIdentity above), so no
        // freshly minted pane can reach a closed pane's shell through here.
        //
        // Deliberately not retried inline. The re-resolution is a server round
        // trip, and driving it from inside the failure would spin against a
        // server that keeps refusing; the pane asks again the next time
        // something asks it to attach.
        pane.tmuxTarget = ""
    }

    // Release the PTY channel; the remote tmux session keeps running.
    function detachNow() {
        if (pane.factory && pane.controller)
            pane.factory.detach(pane.controller)
        pane.attached = false
    }

    // Destroy the remote tmux session for this pane, processes and all. Returns
    // whether the kill actually reached the server, so a caller does not tell the
    // user their session is gone when it is still running.
    //
    // ch::TerminalFactory::kill() returns nothing and reports a refusal through
    // its error() signal — SYNCHRONOUSLY, from inside the call, which lands in
    // this pane's onError handler and writes `statusText`. Assigning a cheerful
    // "Session killed" afterwards (which this used to do unconditionally)
    // overwrote that explanation with its exact opposite: a kill attempted with
    // no SSH connection deliberately KEEPS the tmux target and says the session
    // "is still running and was not killed", and the user was shown "Session
    // killed. Connect to start a new one."
    //
    // What the factory does publish is `targetFor()`: the tmux session this pane
    // last aimed at, cleared ONLY once the kill command has actually been handed
    // to the server (documented on TerminalFactory::kill/targetFor) and kept
    // otherwise so the pane can try again. Comparing it across the call is
    // therefore how QML learns what happened.
    function killSession() {
        if (!pane.factory || !pane.controller) {
            pane.statusText = qsTr("No terminal service in this window.")
            return false
        }
        const target = pane.factory.targetFor(pane.controller)
        pane.factory.kill(pane.controller)
        // kill() detaches first in every case, so the channel is gone either way.
        pane.attached = false
        if (target.length === 0) {
            // Never attached: there was no remote session to destroy, and the
            // factory stays silent about it rather than reporting a failure.
            pane.statusText = qsTr("This pane has no remote session to kill yet.")
            return false
        }
        if (pane.factory.targetFor(pane.controller).length > 0) {
            // Refused. The factory has already written the reason into
            // `statusText` through error(); leave it there.
            return false
        }
        // The factory has thrown this target away — the emptiness just checked
        // is what proves the kill reached the server — so the pane must not
        // keep a copy of it. An authorization nobody holds any more is exactly
        // what makes every later attach refuse (see attachNow), and the flag is
        // what stops the very next automatic attempt from running
        // `tmux new-session -A` and recreating the session the user just ended.
        // Together they make the sentence below true: nothing happens until the
        // user presses Connect, and then a NEW session comes up.
        pane.tmuxTarget = ""
        pane.sessionKilled = true
        pane.statusText = qsTr("Session killed. Connect to start a new one.")
        return true
    }

    // What this pane's Connect and Retry controls do, and the ONLY difference
    // from attachNow() is that this is the USER asking: it lifts the guard that
    // keeps a deliberately killed session from coming back on its own.
    function reconnectNow() {
        pane.sessionKilled = false
        pane.attachNow()
    }

    // A pane retargeted at another session/terminal must follow it: drop the
    // old PTY (the remote tmux session stays alive for whoever else wants it),
    // forget the target — it belongs to the slot this pane has just left, and
    // attaching the new slot to it would put two panes on one shell — and open
    // the new one.
    function retarget() {
        if (pane.attached)
            pane.detachNow()
        pane.tmuxTarget = ""
        // A lookup still in flight was asked about the slot this pane has just
        // left. It is NOT cancelled and no second one is started beside it:
        // marking it stale keeps exactly one answer outstanding, which is the
        // only way an answer that carries no slot of its own can be told apart
        // from the one this pane wants now. onTargetResolved discards it and
        // asks the current question.
        if (pane.resolving)
            pane.resolveStale = true
        // The new slot gets a fresh window, and a stalled report about the old
        // one must not stick to it. Neither may "the user killed this one":
        // that verdict belongs to the terminal this pane has just left, and
        // keeping it would leave the pane it is now showing permanently inert.
        pane.sessionKilled = false
        pane.clearIdentityWait()
        pane.attachNow()
    }

    onDevSessionIdChanged: pane.retarget()
    onTerminalIdChanged: pane.retarget()
    // The last non-empty row id this pane was pointed at. A change from "" to a
    // real id is this pane's identity ARRIVING (a freshly minted row, or a
    // legacy leaf being backfilled while it is already attached to that very
    // row), which must not tear down a live PTY. A change between two real ids
    // is a genuine retarget onto a different terminal.
    // Deliberately NOT bound to `terminalPaneId`: a binding would track it and
    // every change would look like "no change".
    property string boundTerminalPaneId: ""
    onTerminalPaneIdChanged: {
        const previous = pane.boundTerminalPaneId
        pane.boundTerminalPaneId = pane.terminalPaneId
        pane.clearIdentityWait()
        if (previous.length > 0 && previous !== pane.terminalPaneId) {
            pane.retarget()
            return
        }
        // Was pending, now has a row: attachNow() is a no-op if this pane is
        // already up, and the first real attempt if it was waiting.
        pane.attachNow()
    }
    // A leaf the load marked as pre-migration may now resolve by its label.
    onTerminalLegacyChanged: {
        pane.clearIdentityWait()
        pane.attachNow()
    }
    // Deliberately NOT retarget() like the two above, and not an oversight: the
    // working directory only reaches the server as `tmux new-session -A -c <dir>`
    // (TerminalController), and tmux honours -c only when it CREATES the session.
    // Re-attaching a live pane would therefore drop its channel and rebuild it in
    // the very same directory as before — all of the cost of a retarget and none
    // of the effect. So a live pane keeps its directory (the user can `cd`), and
    // only a pane that has not attached yet adopts a late one, which is what
    // makes the host's ordered devSessionId/workingDir push work (see Main.qml's
    // retargetTerminals).
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
        // The server's answer to resolveTarget(). An EMPTY target means the
        // lookup failed; the reason has already arrived through onError above,
        // so this only has to stop the pane waiting for an answer that is not
        // coming — the Retry button then asks again.
        function onTargetResolved(controller, target) {
            if (controller !== pane.controller)
                return
            // The one outstanding lookup is over either way. Clearing this
            // FIRST is what lets the stale branch below ask again: attachNow()
            // declines while a resolution is in flight, so a stale answer that
            // left `resolving` set would strand the pane for good — its own
            // Retry button included.
            pane.resolving = false
            if (pane.resolveStale || pane.resolvingFor !== pane.slotKey()) {
                // Answer to a question this pane no longer has: it was pointed
                // at another slot while the lookup travelled. Adopting it would
                // attach this pane to the OTHER slot's shell. Ask the question
                // it has now instead — nothing else will.
                pane.resolveStale = false
                pane.attachNow()
                return
            }
            if (target.length === 0)
                return
            pane.tmuxTarget = target
            pane.attachNow()
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

    // The per-pane header (SPEC 4.4). The SAME component the viewer panes use,
    // so the two columns line up rather than each inventing its own strip. It
    // lives INSIDE the pane, which is what makes it travel with the pane when a
    // split re-parents it: a header owned by the region would have to be rebuilt
    // to follow, and rebuilding a terminal pane closes its PTY and throws away
    // the scrollback (see the PANE IDENTITY comment in TerminalRegion.qml).
    AppPaneHeader {
        id: paneHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: pane.border.width
        anchors.rightMargin: pane.border.width
        anchors.topMargin: pane.border.width

        // terminalId is NOT server data: it is the client-minted layout pane id
        // (`terminalId: pane.paneId` above), handed out by
        // ch::SessionLayouts::splitPane as "terminal-<n>". It is a LABEL for the
        // slot, and only that — the pane's remote tmux session is named by the
        // server (`tmuxTarget`), so a number reused after a pane was closed
        // shows the user the same familiar label without any chance of adopting
        // the closed pane's still-running shell. A custom title is display-only
        // and never replaces this label in identity or lookup code.
        title: pane.customTitle.length > 0
               ? pane.customTitle
               : (pane.terminalId.length > 0 ? pane.terminalId : qsTr("Terminal"))
        subtitle: pane.stateLabel
        active: pane.paneActive
        // `comingUp`, not `attaching`: the latter is a blink around a synchronous
        // call, so the dot never actually appeared while the pane was coming up.
        busy: pane.comingUp
        onTitleRenameRequested: pane.beginRename()

        actions: [
            AppPaneHeader.Action {
                objectName: "terminalSplitHorizontalButton"
                toolbarId: "pane.split.horizontal"
                text: qsTr("Split this pane side by side")
                glyph: "\u25eb"
                onClicked: {
                    pane.paneActivated(pane.paneId)
                    pane.splitRequested(pane.paneId, "horizontal")
                }
            },
            AppPaneHeader.Action {
                objectName: "terminalSplitVerticalButton"
                toolbarId: "pane.split.vertical"
                text: qsTr("Split this pane top and bottom")
                glyph: "\u229f"
                onClicked: {
                    pane.paneActivated(pane.paneId)
                    pane.splitRequested(pane.paneId, "vertical")
                }
            },
            AppPaneHeader.Action {
                objectName: "terminalCloseButton"
                toolbarId: "pane.close"
                // ONE control, deliberately. The separate kill button beside
                // this one told the user nothing that close did not — both made
                // the pane go away, and only a tiny skull glyph distinguished
                // the one that also ended their shell. It carries no size of
                // its own either: the shared AppPaneHeader.Action metrics are
                // what make it the same comfortable target as split and rename.
                text: qsTr("Close this pane and kill its remote session")
                glyph: "\u00d7"
                onClicked: pane.beginClose()
            },
            AppPaneHeader.Action {
                objectName: "terminalRenameButton"
                text: qsTr("Rename this pane")
                glyph: "R"
                onClicked: pane.beginRename()
            }
        ]
    }
    AppDialog {
        id: renameDialog
        objectName: "terminalPaneRenameDialog"
        title: qsTr("Rename terminal pane")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: renameField.Layout.preferredWidth + leftPadding + rightPadding

        onOpened: {
            renameField.text = pane.customTitle
            renameField.forceActiveFocus()
            renameField.selectAll()
        }

        ColumnLayout {
            implicitWidth: 300
            spacing: 8

            TextField {
                id: renameField
                objectName: "terminalPaneRenameField"
                Layout.preferredWidth: 300
                text: pane.customTitle
                placeholderText: qsTr("Pane title")
                maximumLength: pane.maxCustomTitleLength
            }
        }

        onAccepted: pane.commitRename(renameField.text)
    }

    // Closing a terminal pane is confirmed here, at the pane that owns the
    // destructive action, because it ends the user's remote work and not just
    // their view of it. Declining or dismissing emits nothing at all — not even
    // the plain close — so a mis-click on a one-click destructive control costs
    // the user nothing.
    AppDialog {
        id: killDialog
        objectName: "terminalPaneKillDialog"
        title: qsTr("Close terminal pane")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: 400

        ColumnLayout {
            implicitWidth: 360

            Label {
                objectName: "terminalPaneKillMessage"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                // Both consequences, spelled out, because the pane closing is
                // the visible half and the session ending is the irreversible
                // one.
                text: qsTr("Close \"%1\"? The remote tmux session ends and its running "
                           + "processes are lost, and then the pane closes. This cannot be undone.")
                      .arg(pane.customTitle.length > 0
                           ? pane.customTitle
                           : (pane.terminalId.length > 0
                              ? pane.terminalId : qsTr("this terminal")))
            }
        }

        onAccepted: pane.closeAndKill()
    }


    // ---- keyboard focus handoff --------------------------------------------

    // The xterm page owns keyboard focus inside Chromium. QML focus on this
    // rectangle only changes the shell's bookkeeping, so the explicit web
    // entrypoint below calls xterm.js' own focus method.
    function acceptFocus() {
        pane.focusPending = true;
        pane.forceActiveFocus();
        pane.focusTerminalPage();
    }

    function focusTerminalPage() {
        if (!pane.focusPending || !pane.paneActive
                || !pane.pageLoaded || !viewLoader.item)
            return;
        viewLoader.item.runJavaScript(
            "(function () {"
          + "if (typeof window.codeharborFocusTerminal === 'function') "
          + "window.codeharborFocusTerminal();"
          + "})()");
        pane.focusPending = false;
    }

    // ---- renderer preferences and theme ------------------------------------

    // Push the two client-local terminal preferences into the trusted page.
    // WebEngineView is a separate JavaScript world from QML, so a direct
    // binding cannot reach xterm; the page installs this tiny function during
    // mount and keeps the terminal instance alive while it applies settings.
    function applyTerminalSettings() {
        if (!pane.settingsObject || !pane.pageLoaded || !viewLoader.item)
            return
        const fontPoints = Number(pane.settingsObject.terminalFontSize)
        const pixelRatio = Number(pane.settingsObject.terminalPixelRatio)
        if (!isFinite(fontPoints) || !isFinite(pixelRatio))
            return
        const script = "(function () {"
                     + "if (typeof window.codeharborSetTerminalPreferences === 'function') "
                     + "window.codeharborSetTerminalPreferences("
                     + fontPoints + "," + pixelRatio + ");"
                     + "})()"
        viewLoader.item.runJavaScript(script)
    }

    // Theme.roles is the one palette payload shared by the QML shell and both
    // trusted web bundles. WebEngine has a separate JavaScript world, so pass
    // a JSON object rather than attempting a direct binding. The page keeps a
    // dark standalone default and makes this operation safe to repeat.
    function applyTerminalTheme() {
        if (!pane.pageLoaded || !viewLoader.item || !Theme.roles)
            return
        const roles = JSON.stringify(Theme.roles)
        if (!roles)
            return
        const script = "(function (roles) {"
                     + "if (typeof window.applyTheme === 'function') "
                     + "window.applyTheme(roles);"
                     + "})(" + roles + ")"
        viewLoader.item.runJavaScript(script)
    }

    Connections {
        target: Theme
        function onRolesChanged() { pane.applyTerminalTheme() }
    }

    Connections {
        target: pane.settingsObject
        function onTerminalFontSizeChanged() { pane.applyTerminalSettings() }
        function onTerminalPixelRatioChanged() { pane.applyTerminalSettings() }
    }

    WebChannel {
        id: terminalChannel
    }

    // Activated only once the bridge is registered (and only when there IS one),
    // so the page's WebChannel handshake can never start before its object
    // exists — and a host without the factory spawns no renderer at all.
    Loader {
        id: viewLoader
        // Below the header, not over it. The renderer is a web page: covering it
        // with chrome would work visually and then eat the clicks the terminal
        // needs.
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: pane.border.width
        anchors.rightMargin: pane.border.width
        anchors.bottomMargin: pane.border.width
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
                // profile deliberately does not — SPEC 7.3, "Browser Profiles",
                // which is the section requiring the two-profile split). The
                // bundle is trusted app code, so scripting is intentionally on.
                // Resolved through the pane's guarded handle, not a bare
                // `viewers` lookup: an unguarded lookup of a context property
                // the host did not install throws, exactly as it would for
                // `terminalFactory` above.
                profile: pane.viewerModel ? pane.viewerModel.internalProfile() : null
                settings.javascriptEnabled: true
                settings.localContentCanAccessFileUrls: false
                settings.localContentCanAccessRemoteUrls: false
                // SECURITY: window.open() is the one navigation primitive that
                // does not pass through onNavigationRequested below, and xterm's
                // built-in OSC 8 handler reaches for exactly it. Nothing in this
                // page has any reason to open a window.
                settings.javascriptCanOpenWindows: false

                // CLIPBOARD: the pane's own right-click menu has a Paste
                // command, and reading the clipboard from JavaScript needs both
                // of these — the first allows access at all, the second allows
                // the read half. Writing was already permitted, because a copy
                // triggered by the user's own keypress always is.
                //
                // The page this applies to is the application's own bundle and
                // nothing else: its Content-Security-Policy admits no script but
                // this one, and the view is pinned to that single URL below, so
                // the remote shell's output cannot reach the clipboard through
                // it. Terminal output is drawn, never executed.
                settings.javascriptCanAccessClipboard: true
                settings.javascriptCanPaste: true

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

                // The view's own menu is refused as well. The page already
                // answers the right button with the terminal's menu and calls
                // preventDefault(), but Qt WebEngine offers a second menu of its
                // own (Reload, Back, View Source), and accepting the request
                // here is what stops it appearing on top of the page's one.
                onContextMenuRequested: function(request) {
                    request.accepted = true
                }

                // Belt and braces for the same rule: a new window would be a
                // fresh view outside the pinning above. Handling the signal and
                // never calling openIn() is what denies it.
                onNewWindowRequested: function(request) {
                    console.warn("TerminalPaneView: refused a new window for",
                                 request.requestedUrl)
                }

                onLoadingChanged: function(request) {
                    if (request.status === WebEngineView.LoadStartedStatus) {
                        pane.pageLoaded = false
                    } else if (request.status === WebEngineView.LoadSucceededStatus) {
                        pane.pageLoaded = true
                        Qt.callLater(pane.applyTerminalTheme)
                        Qt.callLater(pane.applyTerminalSettings)
                        Qt.callLater(pane.focusTerminalPage)
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
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: pane.border.width
        anchors.rightMargin: pane.border.width
        anchors.bottomMargin: pane.border.width
        color: Theme.surfaceSunken
        visible: !pane.pageLoaded

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 340)
            spacing: 8

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "\u25ae"
                color: Theme.textFaint
                font.pixelSize: 26
                font.family: Theme.monoFamily
            }
            Label {
                objectName: "paneTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                // The headline is what this pane IS. The id it is keyed by is
                // plumbing and belongs in the small print below.
                text: qsTr("Terminal")
                color: Theme.text
                font.pixelSize: Theme.fontSizeTitle
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
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeBody
            }
            Label {
                objectName: "paneIdentityLabel"
                anchors.horizontalCenter: parent.horizontalCenter
                // Same rule: terminalId is the client-minted layout pane id
                // (see the header above), drawn as plain text like every other
                // identifier on this pane.
                textFormat: Text.PlainText
                text: pane.terminalId
                visible: pane.terminalId.length > 0
                color: Theme.textFaint
                font.pixelSize: Theme.fontSizeSmall
                font.family: Theme.monoFamily
            }
            Button {
                id: attachButton
                objectName: "terminalConnectButton"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: pane.factory !== null && !pane.attached
                text: qsTr("Connect")
                implicitHeight: 30
                leftPadding: 14
                rightPadding: 14
                focusPolicy: Qt.StrongFocus
                contentItem: Label {
                    text: attachButton.text
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeBody
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radiusSmall
                    // #3a3a52 is a lighter hover step ON Theme.surfaceRaised and
                    // has no Theme role; Theme.surfaceHover is the DARKER row
                    // hover, so substituting it here would invert the state.
                    color: attachButton.down ? Theme.border
                         : attachButton.hovered ? Theme.controlHoverSurface() : Theme.surfaceRaised
                    border.width: attachButton.visualFocus ? 2 : 1
                    border.color: attachButton.visualFocus ? Theme.accent : Theme.border
                }
                onClicked: pane.reconnectNow()
            }
        }
    }

    // Loaded but not live: a thin banner, so a drop or an attach failure is
    // visible without hiding the scrollback the user still wants to read.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.leftMargin: pane.border.width
        anchors.rightMargin: pane.border.width
        height: 22
        // #45222c / #3a2f1e are dim FILLS behind danger/warning text and have no
        // Theme role (Theme.danger and Theme.warning are the text colours, far
        // too bright to fill with).
        color: pane.connectionState === "error" ? Theme.errorSurface() : Theme.warningSurface()
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
                color: Theme.warning
                font.pixelSize: Theme.fontSizeSmall
            }
            // Sized and coloured to sit INSIDE a 22-pixel banner. The Basic
            // style's default is a light plate at its own metrics, which both
            // overflowed the banner and read as somebody else's chrome.
            AppPaneHeader.Action {
                objectName: "terminalRetryButton"
                anchors.verticalCenter: parent.verticalCenter
                visible: pane.factory !== null && !pane.attached
                text: qsTr("Retry")
                onClicked: pane.reconnectNow()
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
        pane.initializeController()
        pane.started = true
        // The renderer is told the pane's CURRENT visibility once, at birth.
        // onVisibleChanged below only fires on a CHANGE, so a pane created
        // already hidden (minted into the region's parking lot, or built while
        // its window is not on screen) would otherwise leave the bridge on its
        // default assumption and have output flushed at a view nobody sees.
        if (pane.bridge)
            pane.bridge.notifyViewVisible(pane.visible)
        pane.attachNow()
    }

    // Deterministic release: the controller and its channel die with the pane
    // anyway, but detaching first closes the SSH channel instead of leaving it
    // to destruction order.
    Component.onDestruction: pane.detachNow()
}
