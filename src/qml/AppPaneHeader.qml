import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

// The per-pane header bar (SPEC 4.3/4.4/4.5), used by BOTH regions — a terminal
// pane header and a viewer pane header are the SAME component, so the two
// columns line up across the window instead of each inventing its own strip of
// chrome.
//
// Until now a pane had no header at all: which pane was focused, what a pane was
// showing and how to close it were only reachable through the command palette
// (Ctrl+Shift+P), which means a user who has not found the palette cannot see or
// do any of it. This is that missing surface.
//
// It lives INSIDE the pane, so it travels with the pane when a split re-parents
// it (see the PANE IDENTITY comment in ViewerRegion.qml): a header that was part
// of the region would have to be rebuilt to follow the pane, and rebuilding is
// exactly what must never happen to a pane.
//
// Usage:
//
//     AppPaneHeader {
//         title: "note.txt"
//         subtitle: qsTr("modified")
//         active: pane.paneActive
//         busy: false
//         actions: [
//             AppPaneHeader.Action { text: qsTr("Close"); glyph: "\u00d7"
//                                   onClicked: pane.close() }
//         ]
//         onTitleActivated: pane.focusAddress()
//     }
Rectangle {
    id: header

    // What this pane holds. Drawn as PLAIN TEXT: a title is a remote file name
    // or a server-supplied terminal id, i.e. attacker-influenced data, and a
    // Label defaults to Text.AutoText which promotes anything that looks like
    // markup to StyledText (which fetches <img src="http://...">).
    property string title: ""
    // A dimmed qualifier to the right of the title: a view kind, a connection
    // state, "modified".
    property string subtitle: ""
    // This is the pane the user is working in. Drawn as an accent left edge,
    // which is the ONLY thing on screen that answers "where will the next split
    // land"; the regions publish it as `focusedPaneId`.
    property bool active: false
    // Something is in flight for this pane (a listing, a read, an agent).
    property bool busy: false

    // Controls laid out right-aligned, declared by the caller:
    //
    //     actions: [ AppPaneHeader.Action { ... }, ... ]
    //
    // A `list<Item>` gives its entries this header as their QObject parent but
    // NO visual parent, so nothing would be drawn; _adoptActions() below hands
    // each one to the layout row. Assigning the list again re-adopts.
    property list<Item> actions

    // The title area was clicked. Used for address entry (ViewerPane) and would
    // be used for a rename; a caller that leaves it unhandled simply has an
    // inert title, which is why the title is styled as text rather than as a
    // button.
    signal titleActivated()
    // A terminal caller uses this gesture to open its rename editor. Viewer
    // callers leave it unhandled, so their single-click title behavior stays
    // unchanged.
    signal titleRenameRequested()

    implicitHeight: Theme.paneHeaderHeight
    color: Theme.surfaceRaised

    function _adoptActions() {
        // `actions` can be assigned by the caller before this header's own
        // children have been built, so the row is not assumed to exist yet;
        // Component.onCompleted runs this again once it certainly does.
        if (!actionRow)
            return;
        // Anything the row still holds that the new list does not name is no
        // longer one of this header's actions. Re-assigning `actions` is the
        // only way that happens, and without this the dropped controls would
        // stay in the row — still drawn, still clickable, still wired to
        // whatever they were wired to — because a `list<Item>` entry keeps the
        // header as its QObject parent whatever the list says.
        const wanted = [];
        for (let i = 0; i < header.actions.length; ++i) {
            const item = header.actions[i];
            if (!item)
                continue;
            wanted.push(item);
            if (item.parent !== actionRow)
                item.parent = actionRow;
            if (typeof item.setToolbarEnabled === "function")
                item.setToolbarEnabled(true);
        }
        const shown = actionRow.children;
        for (let k = shown.length - 1; k >= 0; --k) {
            if (wanted.indexOf(shown[k]) < 0) {
                if (typeof shown[k].setToolbarEnabled === "function")
                    shown[k].setToolbarEnabled(false);
                shown[k].parent = null;
            }
        }
    }

    onActionsChanged: header._adoptActions()
    Component.onCompleted: header._adoptActions()

    // A compact header control. `text` is the words — the accessible name and
    // the hover tooltip — while `glyph` is what is actually drawn, so a button
    // can be one character wide without becoming anonymous to a screen reader
    // or to a user who does not recognise the symbol.
    //
    // Declared here rather than in each caller because BOTH region headers and
    // BOTH pane headers need the same control; reach it as
    // `AppPaneHeader.Action` from any file in this module.
    component Action: Button {
        id: action

        property string glyph: ""
        // Stable toolbar ids are declared at the affordance, not inferred from
        // its glyph or position. A viewer pane can therefore register the same
        // action even when it is hosted outside this header's action list.
        property string toolbarId: ""
        property bool toolbarEnabled: true
        property string registeredToolbarId: ""

        function syncToolbarRegistration() {
            const desired = action.toolbarEnabled && action.toolbarId.length > 0
                          ? action.toolbarId : "";
            if (desired === action.registeredToolbarId)
                return;
            if (action.registeredToolbarId.length > 0)
                ToolbarRegistry.unregisterButton(action.registeredToolbarId);
            action.registeredToolbarId = desired;
            if (desired.length > 0)
                ToolbarRegistry.registerButton(desired);
        }

        function setToolbarEnabled(enabled) {
            action.toolbarEnabled = enabled;
            action.syncToolbarRegistration();
        }

        onToolbarIdChanged: action.syncToolbarRegistration()
        onToolbarEnabledChanged: action.syncToolbarRegistration()
        Component.onCompleted: action.syncToolbarRegistration()
        Component.onDestruction: {
            if (action.registeredToolbarId.length > 0)
                ToolbarRegistry.unregisterButton(action.registeredToolbarId);
        }

        // Never takes focus: these sit on top of a terminal or an editor, and a
        // header button that stole the keyboard would send the user's next
        // keystroke to their shell.
        focusPolicy: Qt.NoFocus
        topPadding: 0
        bottomPadding: 0
        leftPadding: 6
        rightPadding: 6
        implicitHeight: 20
        background: Rectangle {
            radius: Theme.radiusSmall
            color: action.down ? Theme.surfaceSelected
                 : action.hovered ? Theme.surfaceHover : "transparent"
            border.width: action.visualFocus ? 2 : 0
            border.color: Theme.accent
        }

        contentItem: Label {
            // Titles and states can carry remote text; a glyph is ours. Both go
            // through the plain-text rule, since only one Label draws either.
            textFormat: Text.PlainText
            text: action.glyph.length > 0 ? action.glyph : action.text
            color: !action.enabled ? Theme.textFaint
                 : action.hovered || action.down ? Theme.text : Theme.textDim
            font.pixelSize: Theme.fontSizeBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }


        // The module's one tooltip (AppToolTip.qml): the ATTACHED ToolTip.text
        // form is drawn by the Basic style in that style's own light palette,
        // so a hint about a dark header would arrive as a white box.
        AppToolTip {
            id: actionTip
            x: 0
            y: action.height + 4
            text: action.text
            visible: action.hovered && action.text.length > 0
        }
    }

    // The focused-pane marker. Two pixels on the leading edge: enough to find
    // across a window of panes, too little to read as a border.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 2
        color: Theme.accent
        visible: header.active
    }

    // The rule under the header, so the header reads as chrome ON the pane
    // rather than as the first line of its content.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.borderSubtle
    }

    Row {
        id: actionRow
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
    }

    Row {
        id: titleRow
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: actionRow.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: Theme.busy
            visible: header.busy
        }

        Label {
            id: titleLabel
            anchors.verticalCenter: parent.verticalCenter
            // Remote data: never markup.
            textFormat: Text.PlainText
            text: header.title
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            // Elided at the LEFT: these are paths, and the end of a path (the
            // file you are looking at) identifies it while the start does not.
            elide: Text.ElideLeft
            width: Math.max(0, Math.min(implicitWidth,
                                        titleRow.width - (header.busy ? 12 : 0)
                                        - (subtitleLabel.visible
                                           ? subtitleLabel.width + titleRow.spacing : 0)))
        }

        Label {
            id: subtitleLabel
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: header.subtitle
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeSmall
            visible: header.subtitle.length > 0
            elide: Text.ElideRight
            width: Math.min(implicitWidth, titleRow.width * 0.45)
        }
    }

    // The title area is the caller's entry point (address entry in a viewer
    // pane). Declared last so it sits above the labels; it does not cover the
    // action row, which is anchored outside it.
    MouseArea {
        anchors.left: titleRow.left
        anchors.right: titleRow.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        onClicked: header.titleActivated()
        onDoubleClicked: header.titleRenameRequested()
    }
}
