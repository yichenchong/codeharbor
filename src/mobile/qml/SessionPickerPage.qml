import QtQuick
import QtQuick.Controls
import QtQml.Models
import CodeHarbor.Mobile

// Step one of the two-step selection: which Dev Session.
//
// The model is ch::SessionsModel, shared verbatim with the desktop sidebar, and
// it is a TWO-LEVEL QAbstractItemModel — groups at the root, Dev Sessions as
// their children. A plain ListView can only ever show the root level of such a
// model, which is why this uses a DelegateModel with a nested DelegateModel
// rooted at each group's index, exactly as src/qml/SessionsSidebar.qml does. The
// alternative — a flattening proxy — would be a second, mobile-only view of the
// workspace tree that could disagree with the desktop about ordering, collapse
// state and which rows the pin/archive filters hide.
Page {
    id: page

    readonly property var ctl: (typeof mobile !== "undefined") ? mobile : null
    readonly property var host: (typeof app !== "undefined") ? app : null

    background: Rectangle { color: MobileTheme.surface }

    header: Rectangle {
        implicitHeight: MobileTheme.touchTarget
        color: MobileTheme.surfaceRaised

        Text {
            anchors.fill: parent
            anchors.leftMargin: MobileTheme.spacingLarge
            anchors.rightMargin: MobileTheme.spacingLarge
            text: qsTr("Dev Sessions")
            textFormat: Text.PlainText
            color: MobileTheme.text
            font.pixelSize: MobileTheme.fontSizeTitle
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: MobileTheme.border
        }
    }

    // Aggregate row state, ch::SessionRowState by ordinal. The order is pinned by
    // that enum (SPEC 4.2, lower ordinal = higher display priority) and the words
    // are the same ones the desktop shows, so the two shells describe a session
    // identically.
    function stateWord(rowState) {
        switch (rowState) {
        case 0: return qsTr("Error");
        case 1: return qsTr("Waiting for input");
        case 2: return qsTr("Running");
        case 3: return qsTr("Finished");
        case 4: return qsTr("Idle");
        default: return qsTr("Disconnected");
        }
    }

    function stateColor(rowState) {
        switch (rowState) {
        case 0: return MobileTheme.danger;
        case 1: return MobileTheme.warning;
        case 2: return MobileTheme.busy;
        case 3: return MobileTheme.success;
        case 4: return MobileTheme.textDim;
        default: return MobileTheme.textFaint;
        }
    }

    DelegateModel {
        id: groupsModel
        // Guarded like every host lookup in this module: a bare `app` is a
        // ReferenceError, not `undefined`, so the `typeof` test above is what
        // keeps a bare-loaded page inert rather than broken.
        model: page.host ? page.host.sessionsModel : null

        delegate: Column {
            id: groupBlock
            required property int index
            required property string name
            required property bool collapsed
            required property string itemId

            width: sessionsList.width

            // Group header. Tapping it collapses the group, which is a WORKSPACE
            // mutation (the collapse state is server-side, SPEC 4.2), so it goes
            // through the same AppController entry point the desktop uses.
            AbstractButton {
                id: groupHeader
                width: parent.width
                implicitHeight: MobileTheme.touchTarget
                enabled: page.host !== null
                onClicked: page.host.setGroupCollapsed(groupBlock.itemId,
                                                       !groupBlock.collapsed)

                background: Rectangle {
                    color: groupHeader.pressed ? MobileTheme.surfaceHover
                                               : MobileTheme.surfaceDeep
                }

                // A plain child Row rather than `contentItem`, and margins
                // rather than padding: Row is a positioner, so it has no
                // padding properties at all, and assigning one is a QML error
                // rather than a layout that merely looks wrong.
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: MobileTheme.spacing
                    anchors.rightMargin: MobileTheme.spacing
                    spacing: MobileTheme.spacing

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        // A rotated triangle rather than an icon font: no asset,
                        // and it reads correctly at any density.
                        text: groupBlock.collapsed ? "\u25b6" : "\u25bc"
                        textFormat: Text.PlainText
                        color: MobileTheme.textDim
                        font.pixelSize: MobileTheme.fontSizeSmall
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: groupHeader.width - 3 * MobileTheme.spacing - 16
                        text: groupBlock.name
                        // Server-controlled: a group name is whatever a user
                        // typed on any client. SPEC 7.5.
                        textFormat: Text.PlainText
                        color: MobileTheme.text
                        font.pixelSize: MobileTheme.fontSizeLabel
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
            }

            // Collapsed groups hide their sessions (SPEC 4.2).
            Column {
                id: sessionsColumn
                width: parent.width
                visible: !groupBlock.collapsed
                height: visible ? implicitHeight : 0

                // A group with nothing under it says so. It is a state the user
                // reaches routinely — a brand new group, or every session in it
                // hidden by the pinned filter — and without this line an
                // expanded group is indistinguishable from a collapsed one, so
                // tapping the header appears to do nothing at all.
                Text {
                    width: parent.width
                    height: MobileTheme.touchTarget
                    visible: sessionsRepeater.count === 0
                    leftPadding: MobileTheme.spacingLarge
                    rightPadding: MobileTheme.spacing
                    text: qsTr("No Dev Sessions in this group.")
                    textFormat: Text.PlainText
                    color: MobileTheme.textDim
                    font.pixelSize: MobileTheme.fontSizeSmall
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Repeater {
                    id: sessionsRepeater
                    model: DelegateModel {
                        // Guarded for the same reason the outer model is.
                        model: page.host ? page.host.sessionsModel : null
                        rootIndex: groupsModel.modelIndex(groupBlock.index)

                        delegate: AbstractButton {
                            id: sessionRow
                            required property string name
                            required property string subtitle
                            required property int rowState
                            required property string itemId
                            required property bool pinned
                            required property bool archived

                            width: sessionsColumn.width
                            implicitHeight: MobileTheme.touchTarget
                            enabled: page.ctl !== null
                            onClicked: page.ctl.selectSession(sessionRow.itemId)

                            background: Rectangle {
                                color: sessionRow.pressed
                                       ? MobileTheme.surfaceHover
                                       : (page.host
                                          && page.host.activeSessionId === sessionRow.itemId
                                          ? MobileTheme.surfaceSelected
                                          : "transparent")

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    height: 1
                                    color: MobileTheme.borderSubtle
                                }
                            }

                            // Margins, not padding: see the group header above.
                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: MobileTheme.spacingLarge
                                anchors.rightMargin: MobileTheme.spacing
                                spacing: MobileTheme.spacing

                                // The aggregate state, as a dot. The word is in
                                // the subtitle line beside the repository name,
                                // so the colour is never the only carrier of the
                                // information.
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: page.stateColor(sessionRow.rowState)
                                }

                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: sessionRow.width
                                           - 2 * MobileTheme.spacing
                                           - MobileTheme.spacingLarge - 8

                                    Text {
                                        width: parent.width
                                        text: sessionRow.archived
                                              ? qsTr("%1 (archived)").arg(sessionRow.name)
                                              : sessionRow.name
                                        // Server-controlled. SPEC 7.5.
                                        textFormat: Text.PlainText
                                        color: MobileTheme.text
                                        font.pixelSize: MobileTheme.fontSizeBody
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        width: parent.width
                                        text: sessionRow.subtitle.length > 0
                                              ? sessionRow.subtitle + " \u00b7 "
                                                + page.stateWord(sessionRow.rowState)
                                              : page.stateWord(sessionRow.rowState)
                                        // The subtitle is the basename of the
                                        // session's repositoryRoot: a remote path,
                                        // therefore server-controlled. SPEC 7.5.
                                        textFormat: Text.PlainText
                                        color: MobileTheme.textDim
                                        font.pixelSize: MobileTheme.fontSizeSmall
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ListView {
        id: sessionsList
        anchors.fill: parent
        model: groupsModel
        clip: true
        // Momentum scrolling with a bounce is what both platforms do; the
        // desktop-hosted build gets the same behaviour so a layout problem shows
        // up in either.
        boundsBehavior: Flickable.DragAndOvershootBounds
        ScrollBar.vertical: ScrollBar {}
    }

    // Nothing to choose. Either no server is wired (the workspace read fails with
    // a synthetic transport error and the list stays empty) or this server really
    // has no Dev Sessions yet — and neither is an error worth an error strip.
    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        visible: sessionsList.count === 0
        text: qsTr("No Dev Sessions on this server yet.")
        textFormat: Text.PlainText
        color: MobileTheme.textDim
        font.pixelSize: MobileTheme.fontSizeBody
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
