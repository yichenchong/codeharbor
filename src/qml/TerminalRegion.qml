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
Rectangle {
    id: region
    color: "#11111b"

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

    // The region holding the pane cache — the ROOT of this tree. A nested region
    // is handed it through setSource(); a root region has none and is its own.
    // Typed `var` so the cache calls below stay ordinary dynamic lookups.
    property var rootRegion: null
    readonly property var paneOwner: region.rootRegion ? region.rootRegion : region

    // Liveness marker. A DESTROYED QObject reads back `undefined` for every
    // property (it only throws when one is CALLED), so this is how a region on
    // its way out asks "is my owner still there?" during a teardown that may
    // already have taken the owner with it.
    readonly property bool regionAlive: true

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

    // Where a pane waits between homes, so it never goes parentless and never
    // leaves the window — an Item pulled out of its window is detached from the
    // scene graph, and a pane is a live web page, not a rectangle. Zero OPACITY
    // rather than `visible: false`, because a pane that is merely being moved
    // has not become hidden: TerminalPaneView reports its visibility to the
    // renderer, which stops flushing output at a pane it believes nobody sees.
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
        }
        if (pane.parent !== host) {
            pane.anchors.fill = null;
            pane.parent = host;
            pane.anchors.fill = host;
        }
        return pane;
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
            pane.destroy();
        }
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
            pane.destroy();
        }
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
        const pane = region.paneOwner.takePane(key, region,
                                               { devSessionId: region.devSessionId,
                                                 workingDir: region.workingDir });
        if (!pane)
            return;
        region.shownPaneId = key;
        region.showingPane = true;
        region.applyPaneContext();
    }

    // The session context is this REGION's, not the pane's own: a borrowed pane
    // follows whichever leaf is showing it, and keeps following that leaf when
    // the session or the working directory changes underneath it.
    function applyPaneContext() {
        if (!region.showingPane)
            return;
        const pane = region.paneOwner.paneCache[region.shownPaneId];
        if (!pane)
            return;
        pane.devSessionId = region.devSessionId;
        pane.workingDir = region.workingDir;
    }

    // Stop showing this region's pane. The pane outlives the layout node that
    // named it, so this parks it on the owner rather than destroying it.
    function releasePane() {
        const owner = region.paneOwner;
        if (owner.regionAlive === true)
            owner.parkPane(region.shownPaneId, region);
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

    onDevSessionIdChanged: region.applyPaneContext()
    onWorkingDirChanged: region.applyPaneContext()

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

    Loader {
        anchors.fill: parent
        // Branches only. Leaves are not built here — their pane Items outlive
        // any Loader, so syncPane() borrows them from the cache instead. A null
        // node still keeps this inert, which is what stops a recursive child
        // from building anything at all before its real node arrives.
        sourceComponent: region.node && !region.isLeaf(region.node)
                         ? branchComponent : null
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
            for (let k = 0; k < count; ++k)
                sum += r[k] > 0 ? r[k] : 0;
            if (sum > 0 && r[i] > 0)
                return r[i] / sum;
        }
        return 1 / count;
    }

    Component {
        id: branchComponent
        SplitView {
            id: split
            orientation: region.node && region.node.orientation === "vertical"
                         ? Qt.Vertical : Qt.Horizontal

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

            // Every republish re-applies the node's persisted ratios. The
            // Repeater below is keyed on the child COUNT, so it no longer
            // notices a republish that changed only the ratios or only what the
            // children are; this connection is what keeps that visible.
            Connections {
                target: region
                function onNodeChanged() {
                    split.ratiosApplied = false;
                    split.applyRatios();
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
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    // `node` must be set at creation: a declarative `source`
                    // would instantiate the child with the default single-pane
                    // node first, transiently building a stray TerminalPaneView
                    // (and its controller) bound to paneId "terminal-1".
                    Component.onCompleted: setSource("TerminalRegion.qml",
                                                     { node: childLoader.childNode,
                                                       rootRegion: region.paneOwner,
                                                       devSessionId: Qt.binding(() => region.devSessionId),
                                                       workingDir: Qt.binding(() => region.workingDir) })
                    onChildNodeChanged: if (item) item.node = childLoader.childNode
                }
            }
        }
    }
}
