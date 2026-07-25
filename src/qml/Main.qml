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

    // Persist ONLY user-driven handle drags, never layout-driven width changes.
    // Writing back on every onWidthChanged destroyed stored widths: a restored
    // width that does not fit the current window is clamped by SplitView (against
    // the neighbouring region's minimumWidth), and persisting that clamped value
    // overwrote the user's real preference permanently - open the app once on a
    // narrower screen and the stored width was gone for good.
    //
    // SplitView.resizing is true only while a handle is being dragged, so the
    // write happens exactly once, when a drag finishes.
    Connections {
        target: outer
        function onResizingChanged() {
            if (!outer.resizing)
                window.persistRegionWidths();
        }
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
        }

        // The regions are recursive split trees and start out empty; the app
        // owns the SPEC 4.5 "always at least one pane" default so the recursive
        // type never instantiates a pane for a placeholder node.
        ViewerRegion {
            node: ({ paneId: "viewer-1", url: "", children: [] })
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }

        TerminalRegion {
            id: terminalRegion
            node: ({ paneId: "terminal-1", children: [] })
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 280
        }
    }

    // Non-blocking error banner: surfaces app.error (RPC failures forwarded
    // verbatim, SPEC 10.3) as a transient toast so shell-level failures are
    // visible instead of silently swallowed.
    Rectangle {
        id: errorBanner
        z: 1000
        visible: opacity > 0
        opacity: 0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 12
        width: Math.min(errorLabel.implicitWidth + 32, parent.width - 24)
        height: errorLabel.implicitHeight + 20
        radius: 6
        color: "#f38ba8"

        Behavior on opacity { NumberAnimation { duration: 200 } }

        Label {
            id: errorLabel
            anchors.centerIn: parent
            width: parent.width - 32
            color: "#11111b"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        MouseArea {
            anchors.fill: parent
            onClicked: errorBanner.opacity = 0
        }

        Timer {
            id: errorHideTimer
            interval: 6000
            onTriggered: errorBanner.opacity = 0
        }
    }

    Connections {
        target: app
        function onError(message) {
            errorLabel.text = message;
            errorBanner.opacity = 0.97;
            errorHideTimer.restart();
        }
    }
}
