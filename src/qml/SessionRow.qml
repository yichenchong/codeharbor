import QtQuick
import QtQuick.Controls.Basic

// A Dev Session sidebar row (SPEC 4.2): name + repository subtitle + a status
// dot colored by the aggregate rowState. Right-click opens a context menu of
// workspace mutations wired to app.* invokables, addressed by the row's ch id.
// Role names consumed: name, subtitle, rowState, itemId, groupId.
ItemDelegate {
    id: row

    required property int index
    required property string name
    required property string subtitle
    required property int rowState
    required property string itemId  // this session's ch id (SessionsModel role)
    required property string groupId // containing group's ch id (for moves)

    // Width is assigned by the instantiating parent (SessionsSidebar sets it to
    // the sessions column width); fall back to the parent's width otherwise.
    // This is a Repeater/Column delegate, never a ListView delegate, so there
    // is no ListView.view to size against.
    width: parent ? parent.width : implicitWidth
    height: 44

    // SPEC 4.2 precedence: Error red > WaitingForInput amber > Running green >
    // FinishedUnseen blue > Idle gray > Disconnected dark. rowState is the int
    // value of ch::SessionRowState.
    function stateColor(state) {
        switch (state) {
        case 0: return "#f38ba8"; // Error - red
        case 1: return "#f9e2af"; // WaitingForInput - amber
        case 2: return "#a6e3a1"; // Running - green
        case 3: return "#89b4fa"; // FinishedUnseen - blue
        case 4: return "#6c7086"; // Idle - gray
        case 5: return "#313244"; // Disconnected - dark
        default: return "#6c7086";
        }
    }

    background: Rectangle {
        color: row.hovered ? "#232338" : "transparent"
    }

    contentItem: Row {
        spacing: 8
        leftPadding: 24

        Rectangle {
            id: dot
            width: 10
            height: 10
            radius: 5
            color: row.stateColor(row.rowState)
            anchors.verticalCenter: parent.verticalCenter
        }

        Column {
            spacing: 2
            anchors.verticalCenter: parent.verticalCenter

            Label {
                text: row.name
                color: "#cdd6f4"
                font.pixelSize: 13
                elide: Text.ElideRight
            }
            Label {
                text: row.subtitle
                color: "#6c7086"
                font.pixelSize: 11
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.open()
        }
        MenuItem {
            text: qsTr("Duplicate")
            onTriggered: app.duplicateSession(row.itemId)
        }
        MenuItem {
            text: qsTr("Move to top")
            // Move within the current group to position 0.
            onTriggered: app.moveSession(row.itemId, row.groupId, 0)
        }
        MenuItem {
            text: qsTr("Delete")
            onTriggered: app.deleteSession(row.itemId)
        }
    }

    Dialog {
        id: renameDialog
        title: qsTr("Rename session")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        TextField {
            id: renameField
            width: 240
            text: row.name
            placeholderText: qsTr("Session name")
        }

        onAccepted: {
            if (renameField.text.length > 0)
                app.renameSession(row.itemId, renameField.text);
        }
    }
}
