import QtQuick
import QtQuick.Controls.Basic

// Viewer region (SPEC 4.3, 4.5): a recursive split tree of viewer panes. The
// type is self-referential — a branch node instantiates ViewerRegion for each
// child — so the region nests horizontally and vertically to any depth. Panes
// never migrate into the terminal region; this tree owns only the layout.
Rectangle {
    id: region
    color: "#181825"

    // Split-tree node. A leaf carries `paneId` (and an optional `url` to load);
    // a branch carries `orientation` ("horizontal" | "vertical") and `children`.
    // Defaults to a single empty pane so the region always shows one viewer
    // (SPEC 4.5).
    property var node: ({ paneId: "viewer-1", url: "", children: [] })

    function isLeaf(n) {
        return !n || !n.children || n.children.length === 0;
    }

    Loader {
        anchors.fill: parent
        sourceComponent: region.isLeaf(region.node) ? leafComponent : branchComponent
    }

    Component {
        id: leafComponent
        ViewerPane {
            paneId: region.node && region.node.paneId ? region.node.paneId : ""
            url: region.node && region.node.url ? region.node.url : ""
        }
    }

    Component {
        id: branchComponent
        SplitView {
            orientation: region.node && region.node.orientation === "vertical"
                         ? Qt.Vertical : Qt.Horizontal
            Repeater {
                model: region.node ? region.node.children : []
                delegate: ViewerRegion {
                    required property var modelData
                    node: modelData
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                }
            }
        }
    }
}
