import QtQuick
import QtQuick.Controls.Basic

// Sidebar group header (SPEC 4.2): group name with a collapse chevron. Bound to
// a SessionsModel group row via role names (name, collapsed, itemId). Clicking
// the header toggles the collapse via app.setGroupCollapsed(itemId, ...) and
// moves the sidebar's keyboard cursor here; dragging it reorders the groups
// through the sidebar (`host`), which owns every app.* call.
ItemDelegate {
    id: header

    required property int index
    required property string name
    required property bool collapsed
    required property string itemId // group's ch id (SessionsModel itemId role)

    // The SessionsSidebar that instantiated this header; null-guarded so the
    // header stays usable standalone.
    property var host: null

    readonly property bool selected: host !== null && host.currentIsGroup
                                     && host.currentId === header.itemId
    readonly property bool dragging: host !== null && host.dragItem === header

    objectName: "groupHeader:" + itemId

    Component.onCompleted: if (host) host.registerHeader(header)
    Component.onDestruction: if (host) host.unregisterHeader(header)

    width: ListView.view ? ListView.view.width : implicitWidth
    height: 32

    background: Rectangle {
        color: header.selected ? "#2a2a40" : "#181825"
        opacity: header.dragging ? 0.4 : 1.0

        // Keyboard focus ring. The selected wash alone is a 10% lightness step
        // against #181825 — invisible on a dim screen, and the only thing that
        // told a keyboard user where the cursor was.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            color: "transparent"
            border.width: 2
            border.color: "#89b4fa"
            visible: header.selected && header.host !== null && header.host.activeFocus
        }
    }

    contentItem: Row {
        spacing: 6
        leftPadding: 8
        opacity: (header.host !== null && header.host.stale === true) ? 0.55 : 1.0

        Label {
            id: chevron
            width: 16
            text: header.collapsed ? "\u25B8" : "\u25BE" // ▸ / ▾
            color: "#cdd6f4"
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            text: header.name
            color: "#cdd6f4"
            font.bold: true
            font.pixelSize: 13
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
        }
        // Collapsed groups hide their rows, so the header is the only thing
        // left saying there is anything in there at all.
        Label {
            text: qsTr("collapsed")
            visible: header.collapsed
            color: "#6c7086"
            font.pixelSize: 10
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    onClicked: {
        if (host)
            host.selectGroup(header);
        app.setGroupCollapsed(header.itemId, !header.collapsed);
    }

    // Vertical drag = reorder groups. Same grab discipline as SessionRow: the
    // header stays put and the sidebar draws the insertion line.
    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromItems
                         | PointerHandler.CanTakeOverFromHandlersOfDifferentType

        onActiveChanged: {
            if (!header.host)
                return;
            if (active) {
                header.host.beginDrag("group", header);
                header.host.updateDrag(centroid.scenePosition);
            } else {
                header.host.endDrag();
            }
        }
        onCentroidChanged: if (active && header.host) header.host.updateDrag(centroid.scenePosition)
    }
}
