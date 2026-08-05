import QtQuick
import QtQuick.Controls.Basic

// Terminal region (SPEC 4.4, 4.5): a recursive split tree of terminal panes.
// The tree is self-referential, so branch children are instantiated through a
// url-sourced Loader ("TerminalRegion.qml"): QML rejects a component that
// instantiates its own type by name, which is what makes recursion legal here.
// Panes never migrate into the viewer region. The live xterm.js renderer is
// bound to a C++ TerminalController per pane; this tree owns only the layout.
//
// PANE IDENTITY (SPEC 4.5). "Split this pane" republishes the WHOLE tree, and a
// region that rebuilt its leaves out of every republish would DESTROY the pane
// the user was working in. For a terminal that is not a redraw: the pane owns
// its TerminalController, so its PTY channel is closed, and the scrollback the
// user was reading lives in the xterm page inside that very Item — a rebuilt
// pane re-attaches to the same tmux session and looks right while having thrown
// all of it away. Pane Items therefore have identity: the ROOT region keeps a
// paneId -> Item cache and every leaf BORROWS from it, re-parenting the existing
// Item instead of building a new one. Nested regions reach the root through
// `rootRegion`, handed down beside `node` in the same setSource() call.
//
// The cache is the sole owner: sweepPanes() destroys exactly the panes whose
// paneId has left the tree, and nothing else may destroy one — no other code is
// in a position to tell "this pane moved" from "this pane was closed".
//
// A paneId addresses a SLOT IN THIS LAYOUT and nothing else. It is not the
// terminal's identity: the LEAF carries that, as `terminalPaneId` — the id of
// the pane's row in the server's `terminal_panes` table, minted by the server,
// never recycled and shared through the stored layout (see
// TerminalPaneView.terminalPaneId). That is why the labels may keep being
// recycled per Dev Session — ch::SessionLayouts hands out "terminal-1",
// "terminal-2", … so they stay short and stable — without a recycled number
// ever re-attaching a shell some earlier pane left running.
Rectangle {
    id: region
    color: Theme.surfaceSunken

    // Split-tree node. A leaf carries `paneId`; a branch carries `orientation`
    // ("horizontal" | "vertical") and `children`.
    //
    // Deliberately null until assigned: this type is recursive, so a non-null
    // default would make every branch child build a TerminalPaneView (and its
    // controller) for the default node before its real node arrived. The app
    // supplies the SPEC 4.5 "always one pane" default (see Main.qml).
    property var node: null

    // Session context every leaf pane needs to attach a real shell. Propagated
    // down the recursion so a nested pane is as attachable as a root one; a pane
    // stays detached while devSessionId is empty.
    property string devSessionId: ""
    property string workingDir: ""
    // Production Main.qml forwards these immutable values with every write.
    // The generation distinguishes a deliberate reload of the same session
    // from the load that a delayed terminal callback originally observed.
    property double layoutGeneration: 0
    // Main.qml sets this for production. Leaving it false is only for the
    // standalone QML component tests, which exercise legacy signals directly.
    property bool hostStampsWrites: false

    // The region holding the pane cache — the ROOT of this tree. A nested region
    // is handed it through setSource(); a root region has none and is its own.
    // Typed `var` so the cache calls below stay ordinary dynamic lookups.
    property var rootRegion: null
    readonly property var paneOwner: region.rootRegion ? region.rootRegion : region

    // Where this region's `node` sits in the ROOT region's tree, as the index
    // path ch::SessionLayouts::setRatios() addresses a branch by: [] is the
    // root node, ["1"] its second child, ["1","0"] that child's first child.
    // Handed down by setSource() beside `node`, so a nested region can name the
    // branch a drag just resized without the host having to search for it.
    property var nodePath: []

    // Liveness marker. A DESTROYED QObject reads back `undefined` for every
    // property (it only throws when one is CALLED), so this is how a region on
    // its way out asks "is my owner still there?" during a teardown that may
    // already have taken the owner with it.
    readonly property bool regionAlive: true

    // This region is the one the host created, i.e. the whole column. Only it
    // draws the REGION HEADER: a nested region is a subtree, and a header on one
    // would be a second title strip in the middle of the column.
    readonly property bool isRootRegion: !region.rootRegion

    // ---- pane actions (SPEC 4.4/4.5) ---------------------------------------
    //
    // A pane header cannot publish a split tree, collapse a layout or end a
    // remote session itself. It raises a request and the host (Main.qml) runs
    // the operation for the pane id it names.
    //
    // Both plain and stamped variants remain available: standalone component
    // tests use the plain signals, while production uses the immutable session
    // and layout-generation stamp to reject delayed callbacks.
    signal splitRequestedForSession(string sessionId, double generation,
                                    string paneId, string orientation)
    signal closePaneRequestedForSession(string sessionId, double generation,
                                         string paneId)
    signal killTerminalRequestedForSession(string sessionId, double generation,
                                           string paneId)
    signal paneTitleReportedForSession(string sessionId, double generation,
                                       string paneId, string title)
    signal splitRequested(string paneId, string orientation)
    signal closePaneRequested(string paneId)
    signal killTerminalRequested(string paneId)
    signal paneTitleReported(string paneId, string title)

    // A handle INSIDE this region was dragged, so the branch at `pathIndexes`
    // now has these child fractions (one per child, each > 0, summing to 1).
    // Relayed to the host, which persists them through
    // ch::SessionLayouts::setRatios() — the counterpart of the `ratios` this
    // file already RESTORES in ratioFor(). Without it a drag inside a region is
    // forgotten the moment the Dev Session is reopened.
    //
    // Emitted by the ROOT region only, like the three requests above: nested
    // regions hand their reading up through reportRatiosForSession() on the
    // owner, so the host has exactly one signal to listen to per region.
    signal splitRatiosAdjusted(var pathIndexes, var ratios)
    signal splitRatiosAdjustedForSession(string sessionId, double generation,
                                         var pathIndexes, var ratios)

    function reportRatiosForSession(sessionId, generation, pathIndexes, ratios) {
        const owner = region.paneOwner;
        if (owner.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no selected session to stamp
            owner.splitRatiosAdjustedForSession(String(sessionId),
                                                Number(generation),
                                                pathIndexes, ratios);
            return;
        }
        owner.splitRatiosAdjusted(pathIndexes, ratios);
    }

    function isLeaf(n) {
        return !n || !n.children || n.children.length === 0;
    }

    // A leaf's cache key. The EMPTY paneId is a real key, not a missing one:
    // closing a region's last pane leaves a single placeholder leaf with
    // paneId "" (SessionLayouts::closePane), and that placeholder is a pane.
    function paneKeyOf(n) {
        return !n || n.paneId === undefined || n.paneId === null ? "" : String(n.paneId);
    }

    // ---- pane cache (the OWNER's copy of this state) -------

    // paneId -> pane Item. Mutated in place; nothing binds to it.
    property var paneCache: ({})
    // Bumped by the OWNER every time the whole cache is thrown away
    // (resetPanesForNewTree). Unlike the cache itself this IS a bindable
    // property, and it is the one thing a nested region can watch to learn
    // that the pane it believes it is showing no longer exists.
    property int paneEpoch: 0
    // paneId -> { sessionId, generation }: which session's tree the pane is
    // currently part of. Beside the cache and for the same reason - one Item is
    // reused across sessions, so where its reports belong is not a property of
    // the Item itself.
    property var paneStamps: ({})

    // Where a pane waits between homes, so it never goes parentless and never
    // leaves the window — an Item pulled out of its window is detached from the
    // scene graph, and a pane is a live web page, not a rectangle. Zero OPACITY
    // rather than `visible: false`, because TerminalPaneView reports its
    // visibility to the renderer, which stops flushing output at a pane it
    // believes nobody sees.
    Item {
        id: parkingLot
        opacity: 0
    }

    // Hand `host` the pane for `paneId`, minting it only the first time that id
    // is seen; `props` are the leaf's session context, applied at birth so a
    // fresh pane attaches straight away instead of after a detour through the
    // "no session selected" state. Every later call RE-PARENTS the very same
    // Item — the whole point of this file.
    function takePane(paneId, host, props) {
        if (!host)
            return null;
        let pane = region.paneCache[paneId];
        if (!pane) {
            // Parked first and given its identity only afterwards: a pane must
            // already be in the tree when it announces a paneId, or an observer
            // watching the id history never sees the id it settles on.
            pane = paneComponent.createObject(parkingLot, props);
            if (!pane)
                return null;
            region.paneCache[paneId] = pane;
            pane.paneId = paneId;
            // The owner mints EVERY pane in the tree, so this is the one place
            // a pane's focus report has to be wired — and it is wired for the
            // pane's whole life. That is what carries focus across a split: the
            // Item is re-parented, never rebuilt, so the connection (and the id
            // it reports) outlive the republish that moved it.
            pane.paneActivated.connect(region.noteFocus);
            // The stamp these reports travel under lives in `paneStamps`, keyed
            // by pane id, and is refreshed every time the pane is taken into a
            // tree rather than frozen here. Pane ids are per-Dev-Session labels
            // ("terminal-1" exists in most sessions), so the cache hands the
            // SAME Item to the next session; a frozen stamp would make every
            // later close or rename from that pane look stale and be dropped
            // for good.
            pane.closeRequested.connect(id =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneCloseForSession(
                        sessionId, generation, id)));
            pane.splitRequested.connect((id, orientation) =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneSplitForSession(
                        sessionId, generation, id, orientation)));
            pane.killRequested.connect(id =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneKillForSession(
                        sessionId, generation, id)));
            pane.titleChangedRequested.connect((id, title) =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneTitleForSession(
                        sessionId, generation, id, title)));
        }
        // Refreshed on EVERY take, not only at mint: this is the moment the
        // pane joins whatever tree is on screen now, and therefore the moment
        // its reports start belonging to that session.
        region.paneOwner.paneStamps[paneId] = {
            sessionId: String(region.paneOwner.devSessionId),
            generation: Number(region.paneOwner.layoutGeneration)
        };
        if (pane.parent !== host) {
            pane.anchors.fill = null;
            pane.parent = host;
            pane.anchors.fill = host;
        }
        // A pane the user is already focused on (a re-homed one) must come back
        // wearing that mark, and a brand-new one must not wear it.
        region.applyFocusFlags();
        return pane;
    }

    // Deliver a pane's report under the stamp that pane currently carries. No
    // stamp means no tree has taken the pane, and an empty session means there
    // is nothing a write could be addressed to; both drop the report rather
    // than reading the root's current session, which is exactly how one
    // session's command would land on another's layout.
    function reportForPane(paneId, deliver) {
        // An unstamped pane is one no tree has taken yet; it has nothing to
        // report about. Otherwise deliver under the pane's own stamp and let
        // the owner decide what an empty session means - in production it
        // drops the write, and in a standalone region (a test with no session)
        // it falls back to the unstamped report.
        const stamp = region.paneOwner.paneStamps[paneId];
        deliver(stamp ? stamp.sessionId : "", stamp ? stamp.generation : 0);
    }

    // Take a pane out of the layout WITHOUT destroying it. Only the region that
    // is actually showing it may park it, so a pane its new home already adopted
    // is never stolen back by the old one on its way out — which is what makes
    // the scheme independent of the order in which the tree rebuilds.
    function parkPane(paneId, claimant) {
        const pane = region.paneCache[paneId];
        if (!pane || pane.parent !== claimant)
            return;
        pane.anchors.fill = null;
        pane.parent = parkingLot;
    }

    // Every paneId still present anywhere in `n`'s subtree.
    function collectPaneKeys(n, out) {
        if (!n)
            return out;
        if (region.isLeaf(n)) {
            out[region.paneKeyOf(n)] = true;
            return out;
        }
        for (let i = 0; i < n.children.length; ++i)
            region.collectPaneKeys(n.children[i], out);
        return out;
    }

    // Destroy every cached pane whose paneId has left the tree. This is the ONLY
    // place a pane dies: a pane the user closed must not linger in the parking
    // lot, invisibly holding a PTY channel open.
    function sweepPanes() {
        const cache = region.paneCache;
        const live = region.collectPaneKeys(region.node, {});
        for (const key in cache) {
            if (live[key])
                continue;
            const pane = cache[key];
            delete cache[key];
            delete region.paneStamps[key];
            pane.destroy();
            // The pane the user was working in has been CLOSED. Leaving its id
            // as the focus would aim the next split/close command at a pane
            // that no longer exists; "" is "nothing focused", which sends the
            // host back to its fallback.
            if (region.focusedPaneId === key)
                region.focusedPaneId = "";
        }
        region.applyFocusFlags();
    }

    // The owner is going away, so nothing outlives it. Explicit rather than
    // implicit: a re-parented Item is NOT deleted with the item it was parented
    // to (Qt detaches children instead), so without this the panes would be left
    // to the JavaScript collector.
    function destroyAllPanes() {
        const cache = region.paneCache;
        for (const key in cache) {
            const pane = cache[key];
            delete cache[key];
            delete region.paneStamps[key];
            pane.destroy();
        }
    }

    // ---- focused pane (SPEC 4.5) -------------------------------------------

    // paneId of the pane the user last interacted with: "" when none has been
    // touched yet, or when the focused pane has left the tree. Like the cache
    // above, the OWNER's copy is the live one — it is the only region that
    // outlives every republish, so focus set before a split is still set after
    // it. A nested region's own copy stays empty and means nothing; read this
    // on the region you created.
    //
    // HOST CONTRACT: handle `onFocusedPaneIdChanged` on that region and persist
    // the value (Main.qml -> app.uiState.setSelectedPane(devSessionId, id)),
    // which is what makes a split command act on the pane the user is in. Do
    // not filter the empty value: it is a real "selection cleared", not a
    // missing reading.
    //
    // The empty-paneId PLACEHOLDER leaf (SessionLayouts::closePane) reports ""
    // like everything else, so clicking it is indistinguishable from "nothing
    // focused". That is the right answer rather than a lost case: a tree whose
    // only leaf is the placeholder has nothing else a command could target, so
    // the host's first-leaf fallback picks that very pane.
    property string focusedPaneId: ""
    // Counts real user focus reports, including a second click on the already
    // focused pane. Main.qml snapshots this counter before a layout load and
    // drops a pending restore when the user interacts while that load is in
    // flight. Programmatic focusPane() deliberately does not increment it.
    property int userFocusSerial: 0
    // Resetting a tree clears the in-memory focus, but that transition is not a
    // user choice and must not overwrite the selected pane saved for the new
    // session before its tree arrives.
    property bool focusResetting: false

    function firstPaneId(n) {
        if (!n)
            return "";
        if (n.children && n.children.length > 0)
            return region.firstPaneId(n.children[0]);
        return region.paneKeyOf(n);
    }

    function hasPane(n, paneId) {
        if (!n)
            return false;
        if (!n.children || n.children.length === 0)
            return region.paneKeyOf(n) === paneId;
        for (let i = 0; i < n.children.length; ++i) {
            if (region.hasPane(n.children[i], paneId))
                return true;
        }
        return false;
    }

    // Give keyboard focus back to the requested pane. An empty request means
    // the first leaf in this region, which is also the silent fallback used
    // when a remembered pane is no longer in the session's tree. Returning
    // false means the tree has arrived but its recursive Loader has not
    // materialised the pane yet; Main.qml retries rather than dropping focus.
    function focusPane(paneId) {
        const owner = region.paneOwner;
        if (owner !== region)
            return owner.focusPane(paneId);
        let requested = paneId === undefined || paneId === null
                        || String(paneId).length === 0
                      ? region.firstPaneId(region.node)
                      : String(paneId);
        let pane = owner.paneCache[requested];
        if (!pane && !region.hasPane(region.node, requested)) {
            requested = region.firstPaneId(region.node);
            pane = owner.paneCache[requested];
        }
        if (!pane)
            return false;
        owner.focusedPaneId = requested;
        owner.applyFocusFlags();
        if (typeof pane.acceptFocus === "function")
            pane.acceptFocus();
        else if (typeof pane.forceActiveFocus === "function")
            pane.forceActiveFocus();
        return true;
    }

    // A pane reporting that the user is working in it. Only ever reached on the
    // owner, because takePane() — where it is connected — is only ever called
    // on the owner. Re-focusing the focused pane assigns the same string, so
    // the host is not woken for a no-op, but the serial still records the click
    // so a restore waiting on a layout cannot fight it.
    function noteFocus(paneId) {
        region.userFocusSerial += 1;
        region.focusedPaneId = paneId;
        region.applyFocusFlags();
    }

    // Push the focus mark onto the panes themselves, so each pane's header can
    // show whether it is the one the next command will act on. Done here rather
    // than by a binding inside the pane, because "am I focused" is a property of
    // the REGION: exactly one pane has it, and a pane cannot see its siblings.
    // Every pane in the cache is visited, including parked ones, so a pane that
    // comes back into the tree cannot arrive wearing a stale mark.
    function applyFocusFlags() {
        const cache = region.paneCache;
        for (const key in cache)
            cache[key].paneActive = (key === region.focusedPaneId);
    }

    // A pane asking to be split from its own header. Relayed rather than acted
    // on: changing a split tree is a layout operation only the host can publish.
    // Every pane report travels stamped with the session and layout generation
    // the pane was taken into (see reportForPane); a standalone region with no
    // session falls through to the plain signal below, which is what the
    // component tests listen to.
    function notePaneSplitForSession(sessionId, generation, paneId, orientation) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no selected session to stamp
            region.splitRequestedForSession(String(sessionId),
                                            Number(generation),
                                            paneId, orientation);
            return;
        }
        region.splitRequested(paneId, orientation);
    }

    // A pane asking to be closed from its own header. Relayed rather than acted
    // on: closing a pane is a layout change only the host can publish, and it
    // deliberately leaves the remote tmux session running.
    function notePaneCloseForSession(sessionId, generation, paneId) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production uses the immutable callback stamp below
            region.closePaneRequestedForSession(String(sessionId),
                                                Number(generation), paneId);
            return;
        }
        region.closePaneRequested(paneId);
    }

    function notePaneKillForSession(sessionId, generation, paneId) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no selected session to stamp
            region.killTerminalRequestedForSession(String(sessionId),
                                                   Number(generation), paneId);
            return;
        }
        region.killTerminalRequested(paneId);
    }

    function notePaneTitleForSession(sessionId, generation, paneId, title) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no selected session to stamp
            region.paneTitleReportedForSession(String(sessionId),
                                               Number(generation),
                                               paneId, title);
            return;
        }
        region.paneTitleReported(paneId, title);
    }


    // ---- this region's own leaf --------------------------------------------

    // Panes are minted from `paneComponent`, which does not exist until this
    // region is complete — and `node` arrives with the initial properties, i.e.
    // BEFORE completion. Nothing may borrow a pane until this is set.
    property bool paneHostReady: false
    // "" is a legitimate paneId, so "showing nothing" needs its own flag.
    property bool showingPane: false
    property string shownPaneId: ""

    // Show exactly the pane this region's node names, borrowed from the owner.
    // A branch shows none: its leaves are further down.
    function syncPane() {
        if (!region.paneHostReady)
            return;
        const leaf = region.node && region.isLeaf(region.node);
        const key = leaf ? region.paneKeyOf(region.node) : "";
        if (region.showingPane && (!leaf || key !== region.shownPaneId))
            region.releasePane();
        if (!leaf)
            return;
        const pane = region.paneOwner.takePane(key, paneHost,
                                               { devSessionId: region.devSessionId,
                                                 workingDir: region.workingDir,
                                                 customTitle: region.leafCustomTitle(),
                                                 terminalPaneId: region.leafTerminalPaneId(),
                                                 terminalLegacy: region.leafTerminalLegacy() });
        if (!pane)
            return;
        region.shownPaneId = key;
        region.showingPane = true;
        region.applyPaneContext();
    }

    // The server-minted `terminal_panes` row id this leaf owns
    // (SplitNode::terminalPaneId), or "" when it has none. THE pane's identity:
    // paneId beside it is a recyclable slot label and names nothing remote.
    function leafTerminalPaneId() {
        return region.node && region.node.terminalPaneId
               ? String(region.node.terminalPaneId) : "";
    }
    // The optional display title follows the same leaf as the row id, but is
    // intentionally absent from every identity lookup and tmux operation.
    function leafCustomTitle() {
        return region.node && region.node.customTitle
               ? String(region.node.customTitle) : "";
    }

    // Published by ch::SessionLayouts, never persisted: this id-less leaf was
    // stored before layouts carried row ids, so it — and only it — may resolve
    // by its slot label, once. An id-less leaf WITHOUT this is a pane whose row
    // is still being minted, and it must attach nothing until the id lands.
    function leafTerminalLegacy() {
        return region.node ? region.node.terminalLegacy === true : false;
    }

    // The session context is this REGION's, not the pane's own: a borrowed pane
    // follows whichever leaf is showing it, and keeps following that leaf when
    // the session or the working directory changes underneath it. The leaf's
    // terminal identity travels the same way, so a pane picks up the row id the
    // moment ch::SessionLayouts republishes the tree carrying it.
    function applyPaneContext() {
        if (!region.showingPane)
            return;
        const pane = region.paneOwner.paneCache[region.shownPaneId];
        if (!pane)
            return;
        // WORKING DIRECTORY FIRST, exactly as the host pushes the pair (see
        // Main.qml's retargetTerminals). Assigning devSessionId is what makes a
        // pane drop its PTY and re-attach, and `tmux new-session -c <dir>`
        // honours the directory only when it CREATES the session — so a pane
        // that saw the new id while still holding the old directory would open
        // its shell in the previous session's checkout, and the directory
        // arriving a line later could no longer move it.
        // The title is display state from the same leaf, independent of the
        // server row identity and the slot label.
        pane.customTitle = region.leafCustomTitle();
        pane.workingDir = region.workingDir;
        pane.devSessionId = region.devSessionId;
        pane.terminalLegacy = region.leafTerminalLegacy();
        pane.terminalPaneId = region.leafTerminalPaneId();
    }

    // Stop showing this region's pane. The pane outlives the layout node that
    // named it, so this parks it on the owner rather than destroying it.
    function releasePane() {
        const owner = region.paneOwner;
        if (owner.regionAlive === true)
            owner.parkPane(region.shownPaneId, paneHost);
        region.showingPane = false;
        region.shownPaneId = "";
    }

    onNodeChanged: {
        // Only the OWNER sweeps: its `node` IS the whole tree, while a nested
        // region's is a subtree — sweeping from one would destroy every pane
        // outside it.
        if (!region.rootRegion)
            region.sweepPanes();
        region.syncPane();
    }

    // A Dev Session change replaces the whole tree, and a terminal pane belongs
    // to the session that opened it: it holds a live PTY channel and its own
    // scrollback. Pane ids are per-session labels, so "terminal-1" usually
    // exists on both sides of a switch and sweepPanes() - which only drops ids
    // that LEFT the tree - would otherwise hand the next session the previous
    // one's shell. Dropping them here also closes the window in which a report
    // from an old pane could still be delivered.
    function resetPanesForNewTree() {
        if (region.rootRegion)
            return; // only the owner holds the cache
        region.destroyAllPanes();
        region.paneStamps = ({});
        region.focusResetting = true;
        region.focusedPaneId = "";
        region.focusResetting = false;
        // Announced LAST, so every region rebuilds against a cache that is
        // already empty and a focus that is already cleared.
        region.paneEpoch += 1;
    }

    onDevSessionIdChanged: {
        region.resetPanesForNewTree();
        region.applyPaneContext();
    }

    // A reload of the SAME session bumps the generation without changing the
    // session id, and a reloaded tree can name the same panes, so no take would
    // run and the old panes and their stamps would survive into a tree they no
    // longer belong to.
    onLayoutGenerationChanged: region.resetPanesForNewTree()
    onWorkingDirChanged: region.applyPaneContext()

    // Rebuild the leaf this region was showing after the owner threw the cache
    // away. Every region in the tree listens, the owner included: a region's
    // record of "I am showing pane X" outlives X itself, so without this it
    // goes on believing a destroyed pane is on screen and stays BLANK until
    // its `node` happens to change shape. `node` need not change at all — the
    // host binds `node` and `layoutGeneration` to the same object, and a reload
    // that republishes an identical tree only moves the generation.
    Connections {
        target: region.paneOwner
        function onPaneEpochChanged() {
            region.showingPane = false;
            region.shownPaneId = "";
            region.syncPane();
        }
    }

    Component.onCompleted: {
        region.paneHostReady = true;
        region.syncPane();
    }

    Component.onDestruction: {
        if (!region.rootRegion)
            region.destroyAllPanes();
        else if (region.showingPane)
            region.releasePane();
    }

    Component {
        id: paneComponent
        TerminalPaneView {}
    }

    // The region header (SPEC 4.4): the column's own title strip. Pane
    // management controls live in each pane's header, so this strip is only the
    // region label and its divider.
    // Root region ONLY — see `isRootRegion`.
    //
    // PANE IDENTITY: this is a child of the REGION, not of a pane, and it is
    // never re-parented. The per-pane header is the opposite case and lives
    // inside TerminalPaneView, so it travels WITH the pane and its live PTY.
    Rectangle {
        id: regionHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.headerHeight
        visible: region.isRootRegion
        color: Theme.surface

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Terminals")
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            font.bold: true
        }

    }

    // Everything the region header does NOT occupy, and the one parent every
    // pane in this region is re-homed into (see syncPane/releasePane). Panes are
    // parented HERE rather than to the region itself so the header cannot end up
    // underneath a pane that fills its whole region.
    Item {
        id: paneHost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: regionHeader.visible ? regionHeader.bottom : parent.top
        anchors.bottom: parent.bottom

        Loader {
            anchors.fill: parent
            // Branches only. Leaves are not built here — their pane Items outlive
            // any Loader, so syncPane() borrows them from the cache instead. A null
            // node still keeps this inert, which is what stops a recursive child
            // from building anything at all before its real node arrives.
            sourceComponent: region.node && !region.isLeaf(region.node)
                             ? branchComponent : null
        }
    }

    // Fraction of the split this child should occupy. Prefers the node's
    // persisted `ratios` (SPEC 4.5 - split ratios are per Dev Session state),
    // normalized so a stale or partial array cannot distort the layout; falls
    // back to an even division.
    function ratioFor(i, count) {
        if (count <= 0)
            return 1;
        const r = region.node && region.node.ratios ? region.node.ratios : null;
        if (r && r.length === count) {
            let sum = 0;
            let valid = true;
            for (let k = 0; k < count; ++k) {
                const value = Number(r[k]);
                if (!isFinite(value) || value <= 0) {
                    valid = false;
                    break;
                }
                sum += value;
            }
            if (valid && sum > 0)
                return Number(r[i]) / sum;
        }
        return 1 / count;
    }

    Component {
        id: branchComponent
        SplitView {
            id: split
            orientation: region.node && region.node.orientation === "vertical"
                         ? Qt.Vertical : Qt.Horizontal

            // The application's one divider (SPEC 4.1). Without it the Basic
            // style draws a filled plate in its own light palette, which is the
            // pale gutter that made this window look unfinished.
            handle: AppSplitHandle {}

            // SplitView stretches only the FIRST fillWidth/fillHeight item, so
            // every later child would fall back to a Loader's implicit size of 0
            // and render as a zero-extent pane. Each child therefore needs an
            // explicit preferred size along the split axis.
            //
            // Applied IMPERATIVELY, exactly once, on the first valid geometry: a
            // declarative binding would be broken by the first drag on the dragged
            // child only (SplitView assigns preferredWidth itself), leaving its
            // siblings still bound and resizing on a different rule. After this
            // one-shot, drag handles own the sizes.
            property bool ratiosApplied: false
            // Capture the stamp when the drag starts. If a session switch lands
            // before release, the write still names the tree that was resized.
            property string resizeSessionId: ""
            property double resizeGeneration: 0
            // ...and WHERE in that tree this branch sat. A republish during the
            // drag (another client splitting a pane, say) can move this branch
            // to a different index, and a reading filed under the new path
            // would resize a branch the user never touched. `null` means "no
            // drag captured one", which is the case when publishRatios() is
            // called directly; it then uses the path the branch has now.
            property var resizePath: null

            function applyRatios() {
                if (ratiosApplied || width <= 0 || height <= 0
                        || childRepeater.count === 0)
                    return;
                for (let i = 0; i < childRepeater.count; ++i) {
                    const child = childRepeater.itemAt(i);
                    if (!child)  // children still materializing; a later change re-runs this
                        return;
                }
                for (let i = 0; i < childRepeater.count; ++i) {
                    const child = childRepeater.itemAt(i);
                    const fraction = region.ratioFor(i, childRepeater.count);
                    if (orientation === Qt.Horizontal)
                        child.SplitView.preferredWidth = width * fraction;
                    else
                        child.SplitView.preferredHeight = height * fraction;
                }
                ratiosApplied = true;
            }

            onWidthChanged: applyRatios()
            onHeightChanged: applyRatios()
            Component.onCompleted: applyRatios()
            function updateNodePaths() {
                for (let i = 0; i < childRepeater.count; ++i) {
                    const child = childRepeater.itemAt(i);
                    if (child && child.item)
                        child.item.nodePath = region.nodePath.concat([String(i)]);
                }
            }

            // The write side of `ratios` (SPEC 4.5). SplitView.resizing is true
            // only while a handle is under the pointer, so this fires exactly
            // once, when a drag FINISHES. The stamp and index path a drag
            // reports under are the ones captured when the drag STARTED (see
            // onResizingChanged), not whatever is current when it ends; keeping
            // them on the split means no caller can report a drag under the
            // wrong session or against the wrong branch.
            function publishRatios() {
                const sessionId = split.resizeSessionId;
                const generation = split.resizeGeneration;
                const path = split.resizePath !== null ? split.resizePath
                                                       : region.nodePath;
                const count = childRepeater.count;
                if (count < 2)
                    return;
                const sizes = [];
                let total = 0;
                for (let i = 0; i < count; ++i) {
                    const child = childRepeater.itemAt(i);
                    if (!child)
                        return;
                    const size = orientation === Qt.Horizontal ? child.width : child.height;
                    // ch::SessionLayouts::setRatios() rejects a non-positive
                    // ratio, and a zero-extent child is a mid-layout reading
                    // rather than a proportion worth storing.
                    if (!(size > 0))
                        return;
                    sizes.push(size);
                    total += size;
                }
                if (!(total > 0))
                    return;
                const ratios = [];
                for (let k = 0; k < sizes.length; ++k)
                    ratios.push(sizes[k] / total);
                region.paneOwner.reportRatiosForSession(
                    sessionId, generation, path, ratios);
            }

            onResizingChanged: {
                if (resizing) {
                    split.resizeSessionId = region.paneOwner.devSessionId;
                    split.resizeGeneration = region.paneOwner.layoutGeneration;
                    split.resizePath = region.nodePath;
                } else {
                    split.publishRatios();
                    split.resizePath = null;
                }
            }

            // Every republish re-applies the node's persisted ratios. The
            // Repeater below is keyed on the child COUNT, so it no longer
            // notices a republish that changed only the ratios or only what the
            // children are; this connection is what keeps that visible.
            Connections {
                target: region
                function onNodeChanged() {
                    split.ratiosApplied = false;
                    split.applyRatios();
                    split.updateNodePaths();
                }
                function onNodePathChanged() {
                    split.updateNodePaths();
                }
            }

            Repeater {
                id: childRepeater
                // The child COUNT, not the child ARRAY. Handing the Repeater a
                // fresh array on every republish made it destroy and rebuild
                // every delegate — and with them every child region, which is
                // precisely how a split used to take the user's pane down. Keyed
                // on the count, a republish that only changes what the children
                // ARE re-points the surviving regions at their new nodes.
                model: region.node && region.node.children
                       ? region.node.children.length : 0
                // Re-apply on any change to the child set (a new pane added by a
                // split, or a whole new tree): without resetting the latch a pane
                // added after first layout would get no preferred size and render
                // zero-extent - the defect this sizing exists to prevent.
                onItemAdded: split.applyRatios()
                onCountChanged: {
                    split.ratiosApplied = false;
                    split.applyRatios();
                    split.updateNodePaths();
                }
                delegate: Loader {
                    id: childLoader
                    required property int index
                    readonly property var childNode:
                        region.node && region.node.children
                        ? region.node.children[childLoader.index] : null
                    // A divider may be dragged all the way to an edge unless
                    // each child declares a minimum on the active axis. Keep a
                    // live pane reachable and clickable even when the user
                    // drags aggressively; SplitView clamps the other axis
                    // harmlessly.
                    SplitView.minimumWidth: 120
                    SplitView.minimumHeight: 80
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    // `node` must be set at creation: a declarative `source`
                    // would instantiate the child with the default single-pane
                    // node first, transiently building a stray TerminalPaneView
                    // (and its controller) bound to paneId "terminal-1".
                    Component.onCompleted: setSource("TerminalRegion.qml",
                                                     { node: childLoader.childNode,
                                                       rootRegion: region.paneOwner,
                                                       nodePath: region.nodePath.concat(
                                                           [String(childLoader.index)]),
                                                       devSessionId: Qt.binding(
                                                           () => region.paneOwner.devSessionId),
                                                       workingDir: Qt.binding(
                                                           () => region.workingDir),
                                                       layoutGeneration: Qt.binding(
                                                           () => region.paneOwner.layoutGeneration),
                                                       hostStampsWrites: Qt.binding(
                                                           () => region.paneOwner.hostStampsWrites) })
                    onChildNodeChanged: {
                        if (!item)
                            return;
                        item.node = childLoader.childNode;
                        item.nodePath = region.nodePath.concat(
                            [String(childLoader.index)]);
                    }
                    onLoaded: {
                        split.applyRatios();
                        split.updateNodePaths();
                    }
                }
            }
        }
    }
}
