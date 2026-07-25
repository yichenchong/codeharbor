import QtQuick
import QtQuick.Controls.Basic

// Viewer region (SPEC 4.3, 4.5): a recursive split tree of viewer panes. The
// tree is self-referential, so branch children are instantiated through a
// url-sourced Loader ("ViewerRegion.qml"): QML rejects a component that
// instantiates its own type by name, which is what makes recursion legal here.
// Panes never migrate into the terminal region; this tree owns only the layout.
Rectangle {
    id: region
    color: "#181825"

    // Split-tree node. A leaf carries `paneId` (and an optional `url` to load);
    // a branch carries `orientation` ("horizontal" | "vertical") and `children`.
    //
    // Deliberately null until assigned: this type is recursive, so a non-null
    // default would make every branch child instantiate a leaf pane for the
    // default node before its real node arrived. The app supplies the SPEC 4.5
    // "always one pane" default (see Main.qml).
    property var node: null

    function isLeaf(n) {
        return !n || !n.children || n.children.length === 0;
    }

    Loader {
        anchors.fill: parent
        sourceComponent: !region.node
                         ? null
                         : (region.isLeaf(region.node) ? leafComponent : branchComponent)
    }

    Component {
        id: leafComponent
        ViewerPane {
            paneId: region.node && region.node.paneId ? region.node.paneId : ""
            url: region.node && region.node.url ? region.node.url : ""
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

            Repeater {
                id: childRepeater
                model: region.node ? region.node.children : []
                // Re-apply on any change to the child set (a new pane added by a
                // split, or a whole new tree): without resetting the latch a pane
                // added after first layout would get no preferred size and render
                // zero-extent - the defect this sizing exists to prevent.
                onItemAdded: split.applyRatios()
                onCountChanged: { split.ratiosApplied = false; split.applyRatios(); }
                onModelChanged: { split.ratiosApplied = false; split.applyRatios(); }
                delegate: Loader {
                    id: childLoader
                    required property var modelData
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    // `node` must be set at creation: a declarative `source`
                    // would instantiate the child with the default single-pane
                    // node first, transiently building a stray leaf pane.
                    Component.onCompleted: setSource("ViewerRegion.qml",
                                                     { node: childLoader.modelData })
                    onModelDataChanged: if (item) item.node = childLoader.modelData
                }
            }
        }
    }
}
