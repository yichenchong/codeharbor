import QtQuick
import QtQuick.Controls.Basic

// Terminal region (SPEC 4.4, 4.5): a recursive split tree of terminal panes.
// The type is self-referential — a branch node instantiates TerminalRegion for
// each child — so the region nests horizontally and vertically to any depth.
// Panes never migrate into the viewer region. The live xterm.js renderer is
// bound to a C++ TerminalController per pane (WebChannel wiring lands with the
// pane host); this tree owns only the layout.
Rectangle {
    id: region
    color: "#11111b"

    // Split-tree node. A leaf carries `paneId`; a branch carries `orientation`
    // ("horizontal" | "vertical") and `children`. Defaults to a single pane so
    // the region always shows at least one terminal (SPEC 4.5).
    property var node: ({ paneId: "terminal-1", children: [] })

    function isLeaf(n) {
        return !n || !n.children || n.children.length === 0;
    }

    Loader {
        anchors.fill: parent
        sourceComponent: region.isLeaf(region.node) ? leafComponent : branchComponent
    }

    Component {
        id: leafComponent
        TerminalPaneView {
            paneId: region.node && region.node.paneId ? region.node.paneId : ""
        }
    }

    Component {
        id: branchComponent
        SplitView {
            orientation: region.node && region.node.orientation === "vertical"
                         ? Qt.Vertical : Qt.Horizontal
            Repeater {
                model: region.node ? region.node.children : []
                delegate: TerminalRegion {
                    required property var modelData
                    node: modelData
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                }
            }
        }
    }
}
