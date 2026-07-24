import QtQuick
import QtQuick.Controls.Basic

// Sessions sidebar region (SPEC 4.2): collapsible groups, Dev Session rows with
// aggregate status. Bootstrap placeholder; model binding lands in workstream M/U.
Rectangle {
    color: "#1e1e2e"

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            text: qsTr("Sessions")
            color: "#cdd6f4"
            font.bold: true
            font.pixelSize: 14
        }
        Label {
            text: qsTr("No sessions yet")
            color: "#6c7086"
            font.pixelSize: 12
        }
    }
}
