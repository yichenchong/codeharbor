import QtQuick
import QtQuick.Controls.Basic

// Sidebar group header (SPEC 4.2): group name with a collapse chevron. Bound to
// a SessionsModel group row via role names (name, collapsed, itemId). Clicking
// the header toggles the collapse via app.setGroupCollapsed(itemId, ...).
ItemDelegate {
    id: header

    required property int index
    required property string name
    required property bool collapsed
    required property string itemId // group's ch id (SessionsModel itemId role)

    width: ListView.view ? ListView.view.width : implicitWidth
    height: 32

    background: Rectangle { color: "#181825" }

    contentItem: Row {
        spacing: 6
        leftPadding: 8

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
    }

    onClicked: app.setGroupCollapsed(header.itemId, !header.collapsed)
}
