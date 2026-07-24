import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window

// The fixed three-region outer layout (SPEC 2.3, 4.1). Region widths are
// adjustable via the SplitView handles and are intended to be persisted per
// client (SPEC 4.1); persistence wiring lands in workstream U.
ApplicationWindow {
    id: window
    width: 1440
    height: 900
    visible: true
    title: qsTr("CodeHarbor")

    SplitView {
        id: outer
        anchors.fill: parent
        orientation: Qt.Horizontal

        SessionsSidebar {
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
        }

        ViewerRegion {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }

        TerminalRegion {
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
        }
    }
}
