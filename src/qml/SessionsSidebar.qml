import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models

// Sessions sidebar region (SPEC 4.2): collapsible groups, Dev Session rows with
// aggregate status. Bound to app.sessionsModel (a two-level QAbstractItemModel)
// via a nested DelegateModel: the top level renders GroupHeader rows, each of
// which nests its group's SessionRow children (hidden while the group is
// collapsed). Role names consumed: name, subtitle, rowState, isGroup, collapsed,
// itemId, groupId.
Rectangle {
    id: sidebar
    color: "#1e1e2e"

    Component.onCompleted: app.refresh()

    // Header bar with the '+ New group' action.
    Rectangle {
        id: headerBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: "#181825"

        Label {
            text: qsTr("Sessions")
            color: "#cdd6f4"
            font.bold: true
            font.pixelSize: 14
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
        }
        Button {
            id: newGroupButton
            text: qsTr("+ New group")
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            onClicked: newGroupDialog.open()
        }
    }

    ListView {
        id: sessionsList
        anchors.top: headerBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: groupsDelegateModel

        ScrollBar.vertical: ScrollBar {}
    }

    DelegateModel {
        id: groupsDelegateModel
        model: app.sessionsModel

        delegate: Column {
            id: groupBlock
            required property int index
            required property string name
            required property bool isGroup
            required property bool collapsed
            required property string itemId

            width: sessionsList.width

            GroupHeader {
                width: parent.width
                index: groupBlock.index
                name: groupBlock.name
                collapsed: groupBlock.collapsed
                itemId: groupBlock.itemId
            }

            // Collapsed groups hide their sessions (SPEC 4.2).
            Column {
                id: sessionsColumn
                width: parent.width
                visible: !groupBlock.collapsed
                height: visible ? implicitHeight : 0

                Repeater {
                    model: DelegateModel {
                        model: app.sessionsModel
                        rootIndex: groupsDelegateModel.modelIndex(groupBlock.index)
                        delegate: SessionRow { width: sessionsColumn.width }
                    }
                }
            }
        }
    }

    Dialog {
        id: newGroupDialog
        title: qsTr("New group")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        TextField {
            id: newGroupField
            width: 240
            placeholderText: qsTr("Group name")
            text: qsTr("New group")
        }

        onAccepted: {
            var groupName = newGroupField.text.length > 0
                            ? newGroupField.text : qsTr("New group");
            app.createGroup(groupName);
            newGroupField.text = qsTr("New group");
        }
    }
}
