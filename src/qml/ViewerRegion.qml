import QtQuick
import QtQuick.Controls.Basic

// Viewer region (SPEC 4.3, 4.5): a recursive split tree of viewer panes. Panes
// never migrate into the terminal region. Bootstrap placeholder; the split-tree
// model and handler registry binding land in workstream V.
Rectangle {
    color: "#181825"

    Label {
        anchors.centerIn: parent
        text: qsTr("Viewer region")
        color: "#6c7086"
        font.pixelSize: 13
    }
}
