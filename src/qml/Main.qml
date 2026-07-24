import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window

// The fixed three-region outer layout (SPEC 2.3, 4.1). Region widths are
// adjustable via the SplitView handles and are persisted per client via
// app.uiState (SPEC 4.1): restored on startup and written back whenever a
// handle move resizes a region.
ApplicationWindow {
    id: window
    width: 1440
    height: 900
    visible: true
    title: qsTr("CodeHarbor")

    // Persist the current region widths. viewer is the fill region (0 = fill),
    // so only the sidebar and terminal fixed widths are meaningful to store.
    function persistRegionWidths() {
        app.uiState.setRegionWidths(Math.round(sidebarRegion.width),
                                    0,
                                    Math.round(terminalRegion.width));
    }

    SplitView {
        id: outer
        anchors.fill: parent
        orientation: Qt.Horizontal

        Component.onCompleted: {
            var sidebar = app.uiState.sidebarWidth();
            var terminal = app.uiState.terminalWidth();
            if (sidebar > 0)
                sidebarRegion.SplitView.preferredWidth = sidebar;
            if (terminal > 0)
                terminalRegion.SplitView.preferredWidth = terminal;
        }

        SessionsSidebar {
            id: sidebarRegion
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
            onWidthChanged: window.persistRegionWidths()
        }

        ViewerRegion {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }

        TerminalRegion {
            id: terminalRegion
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
            onWidthChanged: window.persistRegionWidths()
        }
    }
}
