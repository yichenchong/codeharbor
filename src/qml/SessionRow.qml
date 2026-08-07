import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CodeHarbor

// A Dev Session sidebar row (SPEC 4.2): name + repository subtitle + a status
// dot colored by the aggregate rowState. Right-click opens a context menu of
// workspace mutations wired to app.* invokables, addressed by the row's ch id.
// Left click selects and activates the session; dragging the row hands the
// drop resolution to the sidebar (`host`), which is what talks to app.*.
// Role names consumed: name, subtitle, rowState, pinned, archived, itemId, groupId.
ItemDelegate {
    id: row

    required property int index
    required property string name
    required property string subtitle
    required property int rowState
    required property bool pinned
    required property bool archived
    required property string itemId  // this session's ch id (SessionsModel role)
    required property string groupId // containing group's ch id (for moves)
    // The SessionsSidebar that instantiated this row: registry, drag state and
    // selection cursor all live there. Null-guarded everywhere so the row stays
    // usable standalone.
    property var host: null

    readonly property bool selected: host !== null && !host.currentIsGroup
                                     && host.currentId === row.itemId
    readonly property bool dragging: host !== null && host.dragItem === row
    // The session the host actually has loaded, as opposed to the row the
    // keyboard cursor happens to be sitting on.
    readonly property bool active: host !== null && host.hostActiveSessionId === row.itemId
                                   && row.itemId !== ""
    // The link is down, so every status on screen predates the outage.
    readonly property bool stale: host !== null && host.stale === true

    objectName: "sessionRow:" + itemId

    Component.onCompleted: if (host) host.registerRow(row)
    Component.onDestruction: if (host) host.unregisterRow(row)

    // Width is assigned by the instantiating parent (SessionsSidebar sets it to
    // the sessions column width); fall back to the parent's width otherwise.
    // This is a Repeater/Column delegate, never a ListView delegate, so there
    // is no ListView.view to size against.
    //
    // `implicitWidth` is a CONSTANT rather than the control's usual
    // content-derived one, because the labels below are sized FROM this row's
    // width: a content-derived implicit width would close that loop into a
    // binding cycle the moment a row was built without a parent.
    implicitWidth: 240
    width: parent ? parent.width : implicitWidth
    height: 44

    // SPEC 4.2 precedence: Error red > WaitingForInput amber > Running green >
    // FinishedUnseen blue > Idle gray > Disconnected dark. rowState is the int
    // value of ch::SessionRowState.
    //
    // Colour is never the only carrier: the badge also has a glyph and a
    // silhouette, and the row's tooltip spells the state out. Six shades of
    // 10px dot is exactly the encoding a red-green-blind user cannot read.
    function stateColor(state) {
        switch (state) {
        case 0: return Theme.danger;   // Error
        case 1: return Theme.warning;  // WaitingForInput
        case 2: return Theme.success;  // Running
        case 3: return Theme.accent;   // FinishedUnseen
        case 4: return Theme.textDim;  // Idle
        case 5: return Theme.textFaint; // Disconnected
        default: return Theme.textDim;
        }
    }

    function stateGlyph(state) {
        switch (state) {
        case 0: return "!";        // Error
        case 1: return "?";        // WaitingForInput - the agent wants you
        case 2: return "\u25b8";   // Running
        case 3: return "\u2713";   // FinishedUnseen
        case 4: return "\u2219";   // Idle
        case 5: return "\u2013";   // Disconnected
        default: return "\u2219";
        }
    }

    // Squared off for the two states that are waiting on a human.
    function stateRadius(state) {
        return (state === 0 || state === 1) ? 3 : 7;
    }

    function stateWords(state) {
        switch (state) {
        case 0: return qsTr("Error");
        case 1: return qsTr("Waiting for your input");
        case 2: return qsTr("Agent running");
        case 3: return qsTr("Finished \u2014 output unseen");
        case 4: return qsTr("Idle");
        case 5: return qsTr("Disconnected");
        default: return qsTr("Idle");
        }
    }

    // The module's one tooltip (AppToolTip.qml). The attached `ToolTip.text`
    // form is drawn by the Basic style in that style's own light palette, so a
    // hint about this dark sidebar arrived as a white box.
    AppToolTip {
        // Suffixed with the row id, like every other per-row tooltip in this
        // file: several rows are realised at once, so a bare name would let a
        // findChild() lookup answer with an arbitrary one of them.
        objectName: "sessionRowTip:" + row.itemId
        // Long enough that crossing the row on the way somewhere else does not
        // flash it.
        delay: 600
        visible: row.hovered
        text: row.stale
              ? qsTr("%1 (last known \u2014 the link is down)").arg(row.stateWords(row.rowState))
              : row.stateWords(row.rowState)
    }

    // A screen reader gets nothing from this row otherwise: the delegate's own
    // `text` property is never used (the name arrives in `name`, a SessionsModel
    // role), so the accessible name would be empty and the status dot — colour,
    // glyph and silhouette — carries no text at all.
    Accessible.role: Accessible.ListItem
    Accessible.name: row.name
    Accessible.description: (row.archived ? qsTr("Archived") + qsTr(" \u2014 ") : "")
                            + (row.subtitle.length > 0
                               ? qsTr("%1 \u2014 %2").arg(row.stateWords(row.rowState))
                                                     .arg(row.subtitle)
                               : row.stateWords(row.rowState))

    // Selection wins over hover; the source row of a live drag dims so the
    // floating proxy reads as the thing being moved.
    background: Rectangle {
        color: row.selected ? Theme.border : (row.hovered ? Theme.surfaceHover : "transparent")
        opacity: row.dragging ? 0.4 : 1.0

        // Loaded session: a solid rail. The keyboard cursor is a separate
        // thing, and conflating the two is why "which session am I actually
        // looking at?" used to be unanswerable.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            // The inactive rail is a presentation shade; Theme keeps it
            // visible but restrained in either light or dark mode.
            color: row.active ? Theme.accent : Theme.inactiveRail()
            visible: row.active || row.selected
        }

        // Keyboard focus ring, drawn only while the sidebar really owns the
        // keyboard — otherwise it claims a focus the user does not have.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 3
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            visible: row.selected && row.host !== null && row.host.activeFocus
        }
    }

    contentItem: Row {
        spacing: 8
        leftPadding: 24
        // Stale rows are dimmed as a block: the status they show is the last
        // one the server managed to report, not the current one.
        opacity: row.stale ? 0.55 : 1.0

        Rectangle {
            id: dot
            // Named per row so a test can assert that the SPEC 4.2 states are
            // told apart by colour AND by glyph, not by colour alone.
            objectName: "statusDot:" + row.itemId
            width: 14
            height: 14
            radius: row.stateRadius(row.rowState)
            color: row.stateColor(row.rowState)
            anchors.verticalCenter: parent.verticalCenter

            Label {
                objectName: "statusGlyph:" + row.itemId
                anchors.centerIn: parent
                text: row.stateGlyph(row.rowState)
                color: Theme.textOnAccent
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
            }
        }
        // Archived is a separate marker from the status dot and pin star: the
        // box glyph remains visible when archived rows are shown and uses the
        // muted text role rather than any status colour.
        Label {
            id: archivedMarker
            objectName: "archivedMarker:" + row.itemId
            width: 14
            text: row.archived ? "\u25a3" : ""
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeLabel
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: row.archived
            anchors.verticalCenter: parent.verticalCenter
        }

        Column {
            spacing: 2
            anchors.verticalCenter: parent.verticalCenter
            // Text elides only against a WIDTH, and a Label inside a Column
            // inside a Row has none of its own: without this the two labels
            // grew to their full natural width, so a long session name or
            // repository path ran straight off the edge of the sidebar,
            // `elide` notwithstanding.
            //
            // Measured from the Row's OWN width, which the control sets to its
            // available width (the delegate's width less its padding) — not
            // from the Row's implicit width, which is derived from these very
            // children and would be a cycle.
            // The Row's width less everything else in it: its left padding,
            // every sibling, and one gap per gap BETWEEN visible children —
            // six children when the archived marker is drawn and five when it
            // is not, hence five gaps or four. (The delegate's own rightPadding
            // is what keeps the last button off the row's edge; the Row itself
            // has no trailing padding to subtract.)
            width: Math.max(0, parent.width - parent.leftPadding - dot.width
                            - (archivedMarker.visible ? archivedMarker.width : 0)
                            - pinButton.width - archiveButton.width - deleteButton.width
                            - parent.spacing * (archivedMarker.visible ? 5 : 4))

            Label {
                width: parent.width
                text: row.name
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontSizeLabel
                elide: Text.ElideRight
            }
            Label {
                width: parent.width
                text: row.subtitle
                textFormat: Text.PlainText
                color: Theme.textDim
                font.pixelSize: 11
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }
        // The pin is an action on the row, not part of its selection gesture.
        // Keeping it as a child control gives it its own pointer and keyboard
        // target while the rest of the row remains draggable/selectable.
        Button {
            id: pinButton
            objectName: "pinButton:" + row.itemId
            width: 24
            height: 24
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            // Reachable and visibly focusable without a pointer, like the
            // delete button below and both buttons on a group header. Without
            // this a CLICK on the star left the keyboard cursor behind on
            // whatever had focus before, so Space then hit the wrong control.
            focusPolicy: Qt.StrongFocus
            anchors.verticalCenter: parent.verticalCenter
            text: row.pinned ? "\u2605" : "\u2606" // filled / outline star

            readonly property string actionText:
                row.pinned ? qsTr("Unpin %1").arg(row.name)
                           : qsTr("Pin %1").arg(row.name)
            Accessible.role: Accessible.Button
            Accessible.name: pinButton.actionText

            AppToolTip {
                objectName: "pinButtonTip:" + row.itemId
                text: pinButton.actionText
                visible: pinButton.hovered
                delay: 600
            }

            contentItem: Label {
                text: pinButton.text
                color: pinButton.down ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: pinButton.down ? Theme.surfaceRaised
                     : pinButton.hovered ? Theme.surfaceHover : "transparent"
                border.width: pinButton.visualFocus ? 2 : 1
                border.color: pinButton.visualFocus ? Theme.accent : Theme.borderSubtle
            }
            onClicked: if (row.host) row.host.togglePinned(row.itemId, !row.pinned)
        }
        // Archive is deliberately a box/return glyph rather than a star, so
        // this action cannot be confused with the pin marker beside it.
        Button {
            id: archiveButton
            objectName: "archiveButton:" + row.itemId
            width: 24
            height: 24
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            focusPolicy: Qt.StrongFocus
            anchors.verticalCenter: parent.verticalCenter
            text: row.archived ? "\u21a9" : "\u25a3"

            readonly property string actionText:
                row.archived ? qsTr("Unarchive %1").arg(row.name)
                             : qsTr("Archive %1").arg(row.name)
            Accessible.role: Accessible.Button
            Accessible.name: archiveButton.actionText

            AppToolTip {
                objectName: "archiveButtonTip:" + row.itemId
                text: archiveButton.actionText
                visible: archiveButton.hovered
                delay: 600
            }

            contentItem: Label {
                text: archiveButton.text
                color: archiveButton.down ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: archiveButton.down ? Theme.surfaceRaised
                     : archiveButton.hovered ? Theme.surfaceHover : "transparent"
                border.width: archiveButton.visualFocus ? 2 : 1
                border.color: archiveButton.visualFocus ? Theme.accent : Theme.borderSubtle
            }
            onClicked: if (row.host) row.host.toggleArchived(row.itemId, !row.archived)
        }
        // Delete is the destructive sibling of pin and archive: it gets its
        // own pointer/keyboard target, red treatment and named hint, while the
        // row itself remains the selection and drag surface.
        Button {
            id: deleteButton
            objectName: "deleteButton:" + row.itemId
            width: 24
            height: 24
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            focusPolicy: Qt.StrongFocus
            anchors.verticalCenter: parent.verticalCenter
            text: "\u2715"

            readonly property string actionText:
                qsTr("Delete %1").arg(row.name)
            Accessible.role: Accessible.Button
            Accessible.name: deleteButton.actionText

            AppToolTip {
                objectName: "deleteButtonTip:" + row.itemId
                text: deleteButton.actionText
                visible: deleteButton.hovered
                delay: 600
            }

            contentItem: Label {
                text: deleteButton.text
                color: deleteButton.down ? Theme.textOnAccent : Theme.danger
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: deleteButton.down ? Theme.danger
                     : deleteButton.hovered ? Theme.surfaceHover : "transparent"
                border.width: deleteButton.visualFocus ? 2 : 1
                border.color: deleteButton.visualFocus ? Theme.accent : Theme.danger
            }
            onClicked: deleteDialog.open()
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    // Left click selects AND activates: the sidebar emits sessionActivated for
    // the host to load the session's layout.
    onClicked: if (host) host.selectSession(row)

    // Vertical drag = reorder. `target: null` keeps the row in place (the
    // sidebar draws a proxy and an insertion line instead), and the restricted
    // grab permissions stop the enclosing ListView's Flickable from stealing
    // the gesture and turning a reorder into a scroll.
    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromItems
                         | PointerHandler.CanTakeOverFromHandlersOfDifferentType

        onActiveChanged: {
            if (!row.host)
                return;
            if (active) {
                row.host.beginDrag("session", row);
                row.host.updateDrag(centroid.scenePosition);
            } else {
                row.host.endDrag();
            }
        }
        onCentroidChanged: if (active && row.host) row.host.updateDrag(centroid.scenePosition)
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.open()
        }
        MenuItem {
            text: qsTr("Duplicate")
            onTriggered: if (row.host) row.host.duplicateSession(row.itemId)
        }
        MenuItem {
            text: qsTr("Move to top")
            // Move within the current group to position 0.
            onTriggered: if (row.host) row.host.moveSessionToTop(row.itemId, row.groupId)
        }
        MenuItem {
            text: row.archived ? qsTr("Unarchive") : qsTr("Archive")
            onTriggered: if (row.host) row.host.toggleArchived(row.itemId, !row.archived)
        }
        MenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.open()
        }
    }

    AppDialog {
        id: renameDialog
        objectName: "renameDialog:" + row.itemId
        title: qsTr("Rename session")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        // Account for the dialog's horizontal padding around the fixed-width
        // field; the Basic-style default is four pixels too narrow.
        width: renameField.Layout.preferredWidth + leftPadding + rightPadding

        // Reset to the current name each open: imperative edits break the
        // `text: row.name` binding, so without this a cancelled edit would
        // resurface as stale text on the next open. Focused and selected, so
        // "rename" starts by replacing the old name rather than appending to it
        // (assigning `text` leaves the cursor at the end).
        onOpened: {
            renameField.text = row.name;
            renameField.forceActiveFocus();
            renameField.selectAll();
        }

        // Same reason as the two dialogs in SessionsSidebar.qml: a Dialog sizes
        // itself from its content item's IMPLICIT width, which a bare TextField
        // under-reports even when given an explicit `width`, so the field spills
        // out past the dialog's edge. A ColumnLayout reports its children's
        // preferred widths, so the dialog grows to hold the field.
        ColumnLayout {
            // Dialog measures this layout's implicit width, not a child's
            // Layout.preferredWidth; include the field in that measurement.
            implicitWidth: 300
            spacing: 8

            TextField {
                id: renameField
                objectName: "renameField:" + row.itemId
                Layout.preferredWidth: 300
                text: row.name
                placeholderText: qsTr("Session name")
            }
        }

        onAccepted: {
            if (row.host && renameField.text.length > 0)
                row.host.renameSession(row.itemId, renameField.text);
        }
    }
    AppDialog {
        id: deleteDialog
        objectName: "deleteSessionDialog:" + row.itemId
        title: qsTr("Delete Dev Session")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: 400

        ColumnLayout {
            implicitWidth: 360

            Label {
                objectName: "deleteSessionMessage:" + row.itemId
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("Delete the Dev Session \"%1\"? This permanently destroys it and cannot be undone.")
                      .arg(row.name)
            }
        }

        onAccepted: if (row.host) row.host.deleteSession(row.itemId)
    }
}
