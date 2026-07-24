import QtQuick
import QtQuick.Controls.Basic

// Terminal region (SPEC 4.4, 4.5): a recursive split tree of xterm.js terminal
// panes. Panes never migrate into the viewer region. Bootstrap placeholder; the
// split-tree model and WebChannel bridge land in workstream T.
Rectangle {
    color: "#11111b"

    Label {
        anchors.centerIn: parent
        text: qsTr("Terminal region")
        color: "#6c7086"
        font.pixelSize: 13
    }
}
