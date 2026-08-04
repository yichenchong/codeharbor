import QtQuick
import QtQuick.Controls.Basic

// Viewer region (SPEC 4.3, 4.5): a recursive split tree of viewer panes. The
// tree is self-referential, so branch children are instantiated through a
// url-sourced Loader ("ViewerRegion.qml"): QML rejects a component that
// instantiates its own type by name, which is what makes recursion legal here.
// Panes never migrate into the terminal region; this tree owns only the layout.
//
// PANE IDENTITY (SPEC 4.5). "Split this pane" republishes the WHOLE tree, and a
// region that rebuilt its leaves out of every republish would DESTROY the pane
// the user was working in. That is not a redraw: an editor's unsaved buffer
// lives in the Monaco page inside that very Item, so a same-id REPLACEMENT
// loses it for good. Pane Items therefore have identity — the ROOT region keeps
// a paneId -> Item cache and every leaf BORROWS from it, re-parenting the
// existing Item instead of building a new one. Nested regions reach the root
// through `rootRegion`, handed down beside `node` in the same setSource() call.
//
// The cache is the sole owner: sweepPanes() destroys exactly the panes whose
// paneId has left the tree, and nothing else may destroy one — no other code is
// in a position to tell "this pane moved" from "this pane was closed".
Rectangle {
    id: region
    color: Theme.surfaceDeep

    // Split-tree node. A leaf carries `paneId` (and an optional `url` to load);
    // a branch carries `orientation` ("horizontal" | "vertical") and `children`.
    //
    // Deliberately null until assigned: this type is recursive, so a non-null
    // default would make every branch child instantiate a leaf pane for the
    // default node before its real node arrived. The app supplies the SPEC 4.5
    // "always one pane" default (see Main.qml).
    property var node: null
    // The host stamps every write with the session and layout load generation
    // that was current when the gesture began. A delayed pane callback can
    // therefore be rejected in C++ even if QML has already switched sessions.
    property string sessionId: ""
    property double layoutGeneration: 0
    // Main.qml sets this for production. Leaving it false is only for the
    // standalone QML component tests, which exercise the legacy signals.
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

    // ---- pane actions (SPEC 4.3/4.5) ---------------------------------------
    //
    // A pane header cannot publish a split tree or collapse a layout itself,
    // so it raises a request and the host (Main.qml) runs the operation. The
    // pane id is carried all the way through: unlike a palette command, a pane
    // header already names the pane the user acted on.
    //
    // Both the plain and stamped variants are kept for the standalone component
    // tests and for production respectively. The stamp prevents a delayed
    // callback from being replayed into a different Dev Session's tree.
    signal splitRequested(string paneId, string orientation)
    signal closePaneRequested(string paneId)

    signal splitRequestedForSession(string sessionId, double generation,
                                    string paneId, string orientation)
    signal closePaneRequestedForSession(string sessionId, double generation,
                                         string paneId)
    // Forward a directory's request without letting a nested region mutate
    // SessionLayouts itself. Main.qml owns the split operation and persists the
    // chosen target through the ordinary viewer layout path.
    signal openInNewPaneRequested(string paneId, string url, string kind)
    signal openInNewPaneRequestedForSession(string sessionId, double generation,
                                            string paneId, string url, string kind)
    signal messageRequested(string message)

    // A handle INSIDE this region was dragged, so the branch at `pathIndexes`
    // now has these child fractions (one per child, each > 0, summing to 1).
    // Relayed to the host, which persists them through
    // ch::SessionLayouts::setRatios() — the counterpart of the `ratios` this
    // file already RESTORES in ratioFor(). Without it a drag inside a region is
    // forgotten the moment the Dev Session is reopened.
    //
    // Emitted by the ROOT region only, like the two requests above: nested
    // regions hand their reading up through reportRatios() on the owner, so the
    signal splitRatiosAdjusted(var pathIndexes, var ratios)
    signal splitRatiosAdjustedForSession(string sessionId, double generation,
                                         var pathIndexes, var ratios)

    function reportRatios(pathIndexes, ratios) {
        const owner = region.paneOwner;
        reportRatiosForSession(owner.sessionId, owner.layoutGeneration,
                               pathIndexes, ratios);
    }

    function reportRatiosForSession(sessionId, generation, pathIndexes, ratios) {
        const owner = region.paneOwner;
        if (owner.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no session to stamp, so do not write
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

    // ---- pane cache (the OWNER's copy of this state is the live one) -------

    // paneId -> pane Item. Mutated in place; nothing binds to it.
    property var paneCache: ({})
    // paneId -> { sessionId, generation }: which session's tree that pane is
    // currently part of. Lives beside the cache and for the same reason - the
    // cache reuses one Item across sessions, so where that Item's reports
    // should be delivered is not a property of the Item.
    property var paneStamps: ({})
    // A split publishes its tree before QML necessarily materialises the new
    // delegate. Keep a target briefly so Main.qml never has to create a second
    // pane path or race the recursive Loader.
    property var pendingOpenTargets: ({})

    // Where a pane waits between homes, so it never goes parentless and never
    // leaves the window — an Item pulled out of its window is detached from the
    // scene graph, and a pane is a web page, not a rectangle. Zero OPACITY
    // rather than `visible: false`, because a pane that is merely being moved
    // has not become hidden and must not be reported to its renderer as such.
    Item {
        id: parkingLot
        opacity: 0
    }

    // Hand `host` the pane for `paneId`, minting it only the first time that id
    // is seen; `props` are the leaf's initial content, applied at birth so a
    // fresh pane never renders its empty state first. Every later call
    // RE-PARENTS the very same Item — the whole point of this file.
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
            // Same reasoning for what the pane is SHOWING: wired once, at mint
            // time, so the report survives the re-parenting a split performs.
            //
            // Deliberately wired AFTER the paneId assignment, which is the one
            // subtlety here. Assigning the id makes the brand-new pane report
            // the url it was born with (ViewerPane reports on a paneId change
            // too, since it refuses to report while unnamed), and that report is
            // pure echo: the url came OUT of the leaf the host already has
            // stored. Delivering it would be worse than dropping it, because a
            // pane minted for the SPEC 4.5 fallback node — the window between
            // switching Dev Session and its layout arriving — would echo into a
            // SessionLayouts that has no tree yet, and the host would paint
            // "layout not loaded" at a user who merely clicked another session.
            // Every url the host actually needs is a later CHANGE, which this
            // connection does carry.
            // The stamp a pane's reports carry is kept in `paneStamps`, keyed
            // by pane id, and REFRESHED every time the pane is taken into a
            // tree (see below) rather than frozen here. Freezing looks safer
            // and is not: pane ids are per-Dev-Session labels, so "viewer-1"
            // exists in most sessions, the cache hands the SAME Item to the
            // next session, and a frozen stamp would make every later write
            // from that pane look stale and be dropped for good. A pane
            // sitting in this session's tree is this session's pane, and its
            // reports belong to the session it is currently showing.
            pane.urlOpened.connect((id, url) =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneUrlForSession(
                        sessionId, generation, id, url)));
            pane.closeRequested.connect(id =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneCloseForSession(
                        sessionId, generation, id)));
            pane.splitRequested.connect((id, orientation) =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.notePaneSplitForSession(
                        sessionId, generation, id, orientation)));
            pane.openInNewPaneRequested.connect((id, url, kind) =>
                region.reportForPane(id, (sessionId, generation) =>
                    region.paneOwner.noteOpenInNewPaneForSession(
                        sessionId, generation, id, url, kind)));
            // Straight to the OWNER's signal: the owner is the region Main.qml
            // listens to, and a nested region's own signal reaches nobody.
            pane.messageRequested.connect(
                message => region.paneOwner.messageRequested(message));
        }
        // Refreshed on EVERY take, not only at mint: this is the moment the
        // pane becomes part of whatever tree is on screen now, so it is the
        // moment its reports start belonging to that session.
        region.paneOwner.paneStamps[paneId] = {
            sessionId: String(region.paneOwner.sessionId),
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

    // Deliver a pane's report under the stamp that pane currently carries. A
    // pane with no stamp is one no tree has taken (it cannot report yet) and a
    // pane whose stamp names no session has nothing a write could be addressed
    // to, so both drop the report rather than guessing at the current session -
    // guessing is precisely how one session's edit lands on another's layout.
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
    // lot, invisibly holding its page open.
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
        // A target may be queued for a pane whose recursive Loader has not
        // materialised yet. Drop it as soon as that leaf leaves the tree, or a
        // recycled pane id can consume the old open request later.
        for (const key in region.pendingOpenTargets) {
            if (!live[key])
                delete region.pendingOpenTargets[key];
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
            delete region.paneStamps[key];
            delete cache[key];
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
    function notePaneSplit(paneId, orientation) {
        if (region.hostStampsWrites)
            return; // production uses the immutable callback stamp below
        region.splitRequested(paneId, orientation);
    }

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
    // on: closing a pane is a layout change only the host can publish.
    function notePaneClose(paneId) {
        if (region.hostStampsWrites)
            return; // production uses the immutable callback stamp below
        region.closePaneRequested(paneId);
    }

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
    function noteOpenInNewPane(paneId, url, kind) {
        region.openInNewPaneRequested(paneId, url, kind);
    }
    function noteOpenInNewPaneForSession(sessionId, generation, paneId, url, kind) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return;
            region.openInNewPaneRequestedForSession(
                String(sessionId), Number(generation), paneId, url, kind);
            return;
        }
        region.openInNewPaneRequested(paneId, url, kind);
    }
    // Called after SessionLayouts::splitPane has published a new leaf. The
    // split path creates the pane synchronously in most cases; the pending map
    // covers a recursive Loader that materialises it on the next turn.
    function openPaneTarget(paneId, url, kind) {
        const pane = region.paneCache[paneId];
        if (!pane) {
            region.paneOwner.pendingOpenTargets[paneId] = {
                url: url,
                kind: kind,
                expectedUrl: ""
            };
            return true;
        }
        return pane.openUrlWithKind(url, kind);
    }

    // A pane reporting WHAT IT IS SHOWING, so the host can persist it and the
    // session reopens on the same files instead of a set of blank panes. Carried
    // as a signal rather than a property pair because the host writes it through
    // and holds no state of its own; like noteFocus this only ever fires on the
    // owner, since that is where takePane() connects it.
    signal paneUrlReported(string paneId, string url)
    signal paneUrlReportedForSession(string sessionId, double generation,
                                      string paneId, string url)

    function notePaneUrl(paneId, url) {
        if (region.hostStampsWrites)
            return; // production uses the callback stamp captured at mint time
        region.paneUrlReported(paneId, url);
    }

    function notePaneUrlForSession(sessionId, generation, paneId, url) {
        if (region.hostStampsWrites) {
            if (String(sessionId).length === 0)
                return; // production has no selected session to stamp
            region.paneUrlReportedForSession(String(sessionId),
                                              Number(generation), paneId, url);
            return;
        }
        region.paneUrlReported(paneId, url);
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
        const url = region.node.url ? region.node.url : "";
        const pane = region.paneOwner.takePane(key, paneHost, { url: url });
        if (!pane)
            return;
        // `forcedKind` is a transient "Open as" choice. A normal tree
        // publication must resolve the persisted URL through the registry.
        pane.forcedKind = "";
        pane.url = url;
        region.shownPaneId = key;
        region.showingPane = true;
        const pending = region.paneOwner.pendingOpenTargets[key];
        if (pending) {
            delete region.paneOwner.pendingOpenTargets[key];
            if (String(pending.expectedUrl || "") === String(url))
                pane.openUrlWithKind(pending.url, pending.kind);
        }
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

    // A Dev Session change replaces the whole tree, and a pane is not portable
    // between sessions: it holds a page, its scroll position and its own
    // navigation history, all of which belong to the session that opened it.
    // Pane ids are per-session labels, so "viewer-1" usually exists on both
    // sides of the switch and sweepPanes() - which only drops ids that LEFT the
    // tree - would hand the next session the previous one's pane. Dropping them
    // here also closes the window in which a report from an old pane could
    // still be delivered: the pane is gone before the new tree arrives.
    function resetPanesForNewTree() {
        if (region.rootRegion)
            return; // only the owner holds the cache
        region.destroyAllPanes();
        region.paneStamps = ({});
        region.pendingOpenTargets = ({});
        region.focusResetting = true;
        region.focusedPaneId = "";
        region.focusResetting = false;
    }

    onSessionIdChanged: region.resetPanesForNewTree()

    // A reload of the SAME session bumps the generation without changing the
    // session id, and a reloaded tree can name the same panes, so no take would
    // run and the old panes and their stamps would survive into a tree they no
    // longer belong to.
    onLayoutGenerationChanged: region.resetPanesForNewTree()

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
        ViewerPane {}
    }

    // The region header (SPEC 4.3): the column's own title strip. Pane
    // management controls live in each pane's header, so this strip is only the
    // region label and its divider.
    // Root region ONLY — see `isRootRegion`.
    //
    // PANE IDENTITY: this is a child of the REGION, not of a pane, and it is
    // never re-parented. The per-pane header is the opposite case and lives
    // inside ViewerPane, so it travels WITH the pane.
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
            text: qsTr("Viewers")
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
            // Capture the stamp when the drag starts. If a Dev Session switch
            // lands before the mouse release, the finishing event still names
            // the tree it resized rather than the new fallback tree.
            property string resizeSessionId: ""
            property double resizeGeneration: 0

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
            // Zero-argument on purpose: the stamp a drag reports under is the
            // one captured when the drag STARTED (see onResizingChanged), not
            // whatever is current when it ends, and keeping it on the split
            // rather than in the signature means no caller can report a drag
            // under the wrong session by passing the wrong pair.
            function publishRatios() {
                const sessionId = split.resizeSessionId;
                const generation = split.resizeGeneration;
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
                    sessionId, generation, region.nodePath, ratios);
            }

            onResizingChanged: {
                if (resizing) {
                    split.resizeSessionId = region.paneOwner.sessionId;
                    split.resizeGeneration = region.paneOwner.layoutGeneration;
                } else {
                    split.publishRatios();
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
                onCountChanged: { split.ratiosApplied = false; split.applyRatios(); }
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
                    // node first, transiently building a stray leaf pane.
                    function updateChildRegion() {
                        if (!item)
                            return;
                        item.node = childLoader.childNode;
                        item.nodePath = region.nodePath.concat(
                            [String(childLoader.index)]);
                    }
                    Component.onCompleted: setSource("ViewerRegion.qml",
                                                     { node: childLoader.childNode,
                                                       rootRegion: region.paneOwner,
                                                       nodePath: region.nodePath.concat(
                                                           [String(childLoader.index)]),
                                                       sessionId: Qt.binding(
                                                           () => region.paneOwner.sessionId),
                                                       layoutGeneration: Qt.binding(
                                                           () => region.paneOwner.layoutGeneration),
                                                       hostStampsWrites: Qt.binding(
                                                           () => region.paneOwner.hostStampsWrites) })
                    onLoaded: updateChildRegion()
                    onChildNodeChanged: updateChildRegion()
                }
            }
        }
    }
}
