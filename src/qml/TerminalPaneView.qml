import QtQuick
import QtQuick.Controls.Basic

// A single terminal pane (SPEC 3.4) — a leaf of the terminal split tree. The
// live xterm.js renderer is hosted via WebEngine + WebChannel and bound to a
// C++ TerminalController; this view provides the pane chrome and its identity
// within the split tree.
Rectangle {
    id: pane

    property string paneId: ""

    color: "#11111b"
    border.color: "#313244"
    border.width: 1
    implicitWidth: 160
    implicitHeight: 100

    Label {
        anchors.centerIn: parent
        text: pane.paneId.length > 0 ? pane.paneId : qsTr("Terminal")
        color: "#6c7086"
        font.pixelSize: 13
    }
}
