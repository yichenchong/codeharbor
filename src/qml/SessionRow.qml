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

    // The size of every trailing action (pin, archive, delete), as one value
    // rather than three: they are read back as a group when the name Label's
    // width is worked out below, and a row whose actions had drifted to
    // different sizes would look accidental. 22 is a step down from the 24 the
    // group header's buttons used to share with them — still an easy pointer
    // target, and the six pixels it returns go to the session name, which is
    // the one thing in this row anybody reads.
    readonly property int actionButtonSize: 22

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

    // The status as a sentence, including the fact that it may be out of date.
    // Written once and read by both the tooltip and the accessible description
    // below: a hovering user was told the link was down while a screen-reader
    // user was told the stale status as if it were current.
    readonly property string statusText:
        row.stale
        ? qsTr("%1 (last known \u2014 the link is down)").arg(row.stateWords(row.rowState))
        : row.stateWords(row.rowState)

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
        text: row.statusText
    }

    // A screen reader gets nothing from this row otherwise: the delegate's own
    // `text` property is never used (the name arrives in `name`, a SessionsModel
    // role), so the accessible name would be empty and the status dot — colour,
    // glyph and silhouette — carries no text at all.
    Accessible.role: Accessible.ListItem
    Accessible.name: row.name
    Accessible.description: (row.archived ? qsTr("Archived") + qsTr(" \u2014 ") : "")
                            + (row.subtitle.length > 0
                               ? qsTr("%1 \u2014 %2").arg(row.statusText)
                                                     .arg(row.subtitle)
                               : row.statusText)
    // The selection is drawn as a wash and a rail, and neither of those is
    // anything a screen reader can see: without this the whole list announces
    // as rows with no current one among them.
    Accessible.selected: row.selected

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

    // The delegate's inset is the genuine nesting offset. Keeping it on the
    // control (rather than inventing a padding property on Row) also means
    // QtQuick can lay out the content item with the correct available width.
    leftPadding: 12
    rightPadding: 12
    contentItem: Row {
        // The same gap the group header uses between its own chevron and name,
        // so the two rows read as one panel rather than two.
        spacing: 6
        // The delegate above owns the 12-pixel nesting inset. A Row is a plain
        // QtQuick positioner and has no padding of its own.
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
        // Archived is a separate marker from the status dot and pin star. It
        // draws the same "filed below the line" arrow the archive action uses,
        // so the marker and the verb that produced it are the same shape, and
        // it uses the muted text role rather than any status colour.
        Label {
            id: archivedMarker
            objectName: "archivedMarker:" + row.itemId
            width: 14
            text: row.archived ? "\u21a7" : ""
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeLabel
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: row.archived
            anchors.verticalCenter: parent.verticalCenter
        }

        Column {
            // Named so a test can check the hand-computed width below against
            // the children it is derived from.
            objectName: "sessionNames:" + row.itemId
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
            // Every term is read back off the actual children and the Row's
            // own spacing, so resizing an action cannot leave a stale constant
            // behind here; only the number of GAPS is counted by hand.
            width: Math.max(0, parent.width - dot.width
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
            width: row.actionButtonSize
            height: row.actionButtonSize
            implicitWidth: row.actionButtonSize
            implicitHeight: row.actionButtonSize
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
                // One step down with the box, so the glyph keeps the same
                // proportion of it that it had at 24 pixels.
                font.pixelSize: Theme.fontSizeLabel
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
        // Archive is an arrow leaving a shelf rather than a star, so this
        // action cannot be confused with the pin marker beside it. There is no
        // literal box or tray character that every platform's default fonts
        // carry (the tray-shaped ones live in Supplemental Arrows-B or the
        // emoji planes and fall back to a missing-glyph box), so the metaphor
        // is the motion instead of the container: down off the list to file it
        // away, back up off the shelf to bring it out again.
        Button {
            id: archiveButton
            objectName: "archiveButton:" + row.itemId
            width: row.actionButtonSize
            height: row.actionButtonSize
            implicitWidth: row.actionButtonSize
            implicitHeight: row.actionButtonSize
            padding: 0
            focusPolicy: Qt.StrongFocus
            anchors.verticalCenter: parent.verticalCenter
            text: row.archived ? "\u21a5" : "\u21a7" // up off / down onto the shelf

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
                font.pixelSize: Theme.fontSizeLabel
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
            width: row.actionButtonSize
            height: row.actionButtonSize
            implicitWidth: row.actionButtonSize
            implicitHeight: row.actionButtonSize
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
                font.pixelSize: Theme.fontSizeLabel
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

    // Keyboard entry points for the two things that used to need a right-click.
    // The sidebar's key handler calls these on whichever row its cursor is on:
    // Rename lived ONLY in the menu below, so without them a user who never
    // touches a pointer could not rename a session at all. Escape closes the
    // menu and the dialog, and either way the keyboard lands back on the
    // sidebar, so neither one can swallow the cursor.
    //
    // Popped at the row's own bottom-left corner rather than at the pointer:
    // there is no pointer in this path, and popup() with no arguments would
    // open the menu wherever the mouse happens to be resting.
    function openContextMenu() {
        contextMenu.popup(row, 12, row.height);
    }

    function openRenameDialog() {
        renameDialog.open();
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
        // Several rows are realised at once, so the menu carries its session's
        // id like every other named part of this delegate; a bare name would
        // let a findChild() lookup answer with an arbitrary row's menu.
        objectName: "sessionMenu:" + row.itemId

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

            // The field arrives PREFILLED with the current name, so its
            // placeholder never shows and the box was announced — and read —
            // as an unlabelled edit box holding a word. Same visible-label
            // convention as SettingsWindow's Field component.
            Label {
                id: renameFieldLabel
                objectName: "renameFieldLabel:" + row.itemId
                Layout.preferredWidth: 300
                text: qsTr("Session name")
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeSmall
            }

            TextField {
                id: renameField
                objectName: "renameField:" + row.itemId
                Layout.preferredWidth: 300
                text: row.name
                placeholderText: qsTr("Session name")
                // The visible Label above is a separate item, so the field has
                // no name of its own; bound rather than repeated so the two
                // cannot drift apart.
                Accessible.name: renameFieldLabel.text
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
        // Enter cancels here rather than deleting: this dialog's affirmative
        // answer destroys a Dev Session, and a stray keypress must not be able
        // to do that. The delete button is still one Tab and a Space away.
        defaultButton: Dialog.Cancel
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
