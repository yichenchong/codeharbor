import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import QtQuick.Layouts

// Sessions sidebar region (SPEC 4.2): collapsible groups, Dev Session rows with
// aggregate status, drag-and-drop reordering and keyboard navigation. Bound to
// app.sessionsModel (a two-level QAbstractItemModel) via a nested DelegateModel:
// the top level renders GroupHeader rows, each of which nests its group's
// SessionRow children (hidden while the group is collapsed). Role names
// consumed: name, subtitle, rowState, isGroup, collapsed, itemId, groupId.
//
// Drag-and-drop contract (SPEC 4.2): the sidebar NEVER reorders the model
// itself. A drop calls the matching app.* invokable with the INDEX/ordering the
// user asked for and nothing else; the server re-packs positions and the
// refreshed model is what moves the rows. A rejected RPC therefore leaves the
// visible order exactly as the server last reported it.
Rectangle {
    id: sidebar
    color: Theme.surface

    // Emitted when the user picks a Dev Session (click, or Enter on the
    // keyboard cursor). The host wires this to layout loading; the sidebar
    // itself owns no navigation.
    signal sessionActivated(string devSessionId)
    // Emitted by the always-visible server control. The window owns the
    // connection sheet, keeping server-profile editing outside the sidebar.
    signal serverSettingsRequested()


    // Keyboard/selection cursor. Groups and sessions have independent id
    // spaces, so the kind flag is part of the cursor.
    property string currentId: ""
    property bool currentIsGroup: false
    // Last selected Dev Session; drives the row highlight.
    property string selectedSessionId: ""

    // Live drag state, read by the delegates for their drag affordances.
    property string dragKind: "" // "", "session" or "group"
    property var dragItem: null  // the SessionRow / GroupHeader being dragged
    property var dropTarget: null // {groupId,index[,append]} | {index} | null

    activeFocusOnTab: true

    // ---------------------------------------------------------------------
    // Link state
    //
    // The sidebar is where a user notices the server is gone: the rows keep
    // showing whatever the last refresh reported, and nothing else on screen
    // says otherwise. It therefore reads the host's connection surface
    // directly. Guarded, because a host that publishes none (a bare sidebar
    // harness) must keep working — it then simply has nothing to report and
    // the footer takes no space.
    // ---------------------------------------------------------------------
    readonly property string linkState: (app && app.connectionState !== undefined)
                                        ? String(app.connectionState) : ""
    // Rows on screen are known to predate the current reality.
    readonly property bool stale: linkState === "reconnecting" || linkState === "failed"
                                  || linkState === "disconnected"
    // The session the host considers loaded, which is not the same thing as the
    // keyboard cursor or the last row clicked.
    readonly property string hostActiveSessionId: (app && app.activeSessionId !== undefined)
                                                  ? String(app.activeSessionId) : ""

    // Same three-way encoding as ConnectSheet's chip: colour, glyph and word.
    // Every word ch::AppController::setConnectionState() publishes needs a case
    // here — including "credential", which it enters while the connect attempt
    // is parked on a password or key passphrase. Falling through to the default
    // painted that as a grey "Not connected", i.e. the footer denied there was
    // anything to answer while the sheet was asking for it.
    function linkColor(state) {
        switch (state) {
        case "connected": return Theme.success;
        case "connecting":
        case "credential":
        // Installing the remote service on first connect. Same "in progress,
        // nothing is wrong" reading as connecting.
        case "provisioning":
        case "hostkey": return Theme.warning;
        // "Reconnecting" needs to read as worse than "connecting" but not as
        // final as "failed". The theme has no role for that middle step yet, so
        // this one stays a literal.
        case "reconnecting": return "#fab387";
        case "failed": return Theme.danger;
        default: return Theme.textDim;
        }
    }
    function linkGlyph(state) {
        switch (state) {
        case "connected": return "\u2713";
        case "connecting": return "\u2219";
        case "provisioning": return "\u2193"; // downloading onto the server
        case "hostkey": return "?";
        case "credential": return "*"; // the password mask
        case "reconnecting": return "\u21bb";
        case "failed": return "\u2715";
        default: return "\u2013";
        }
    }
    function linkWords(state) {
        switch (state) {
        case "connected": return qsTr("Connected");
        case "connecting": return qsTr("Connecting\u2026");
        case "provisioning": return qsTr("Installing the remote service\u2026");
        case "hostkey": return qsTr("Host key needs approval");
        case "credential": return qsTr("Password or passphrase needed");
        case "reconnecting": return qsTr("Reconnecting\u2026 rows may be stale");
        case "failed": return qsTr("Connection failed");
        default: return qsTr("Not connected");
        }
    }

    Component.onCompleted: app.refresh()

    // ---------------------------------------------------------------------
    // Delegate registry
    //
    // Hit-testing and ordering need the live delegates, and a nested
    // DelegateModel gives no flat index to work from. Delegates register
    // themselves on creation and drop out on destruction; every consumer reads
    // itemId/groupId/index/collapsed off the live item, so a model refresh
    // needs no invalidation step.
    // ---------------------------------------------------------------------
    property var rowItems: []
    property var headerItems: []

    function registerRow(item) {
        if (item && rowItems.indexOf(item) < 0)
            rowItems.push(item);
    }
    function unregisterRow(item) {
        var i = rowItems.indexOf(item);
        if (i >= 0)
            rowItems.splice(i, 1);
        if (dragItem === item)
            cancelDrag();
    }
    function registerHeader(item) {
        if (item && headerItems.indexOf(item) < 0)
            headerItems.push(item);
    }
    function unregisterHeader(item) {
        var i = headerItems.indexOf(item);
        if (i >= 0)
            headerItems.splice(i, 1);
        if (dragItem === item)
            cancelDrag();
    }

    // Groups in model order, each with its sessions in model order. Sessions of
    // a collapsed group are included: they still exist as (invisible) items and
    // their ids are needed to build a complete ordering for reorderSessions.
    function orderedGroups() {
        var headers = [];
        for (var h = 0; h < headerItems.length; ++h) {
            if (headerItems[h])
                headers.push(headerItems[h]);
        }
        headers.sort(function (a, b) { return a.index - b.index; });

        var out = [];
        for (var g = 0; g < headers.length; ++g) {
            var rows = [];
            for (var r = 0; r < rowItems.length; ++r) {
                if (rowItems[r] && rowItems[r].groupId === headers[g].itemId)
                    rows.push(rowItems[r]);
            }
            rows.sort(function (a, b) { return a.index - b.index; });
            out.push({ header: headers[g], rows: rows });
        }
        return out;
    }

    function groupEntry(groupId) {
        var gs = orderedGroups();
        for (var i = 0; i < gs.length; ++i) {
            if (gs[i].header.itemId === groupId)
                return gs[i];
        }
        return null;
    }

    function visibleRows(entry) {
        var out = [];
        for (var i = 0; i < entry.rows.length; ++i) {
            if (entry.rows[i].visible)
                out.push(entry.rows[i]);
        }
        return out;
    }

    // ---------------------------------------------------------------------
    // Geometry, all in sidebar coordinates.
    // ---------------------------------------------------------------------
    function itemTop(item) {
        return item.mapToItem(sidebar, 0, 0).y;
    }

    // Vertical extent of a whole group block: the header alone when collapsed,
    // header plus its visible rows otherwise.
    function groupSpan(entry) {
        var top = itemTop(entry.header);
        var bottom = top + entry.header.height;
        if (!entry.header.collapsed) {
            var rows = visibleRows(entry);
            for (var i = 0; i < rows.length; ++i) {
                var b = itemTop(rows[i]) + rows[i].height;
                if (b > bottom)
                    bottom = b;
            }
        }
        return { top: top, bottom: bottom };
    }

    // ---------------------------------------------------------------------
    // Drop resolution
    // ---------------------------------------------------------------------

    // Insertion point for a dragged session: the group under the cursor plus
    // the index within it. Dropping onto a collapsed group appends to it,
    // because its rows offer no insertion points to aim at.
    function resolveSessionDrop(y) {
        var gs = orderedGroups();
        if (gs.length === 0)
            return null;

        var chosen = gs[gs.length - 1];
        for (var i = 0; i < gs.length; ++i) {
            if (y < groupSpan(gs[i]).bottom) {
                chosen = gs[i];
                break;
            }
        }

        if (chosen.header.collapsed)
            return { groupId: chosen.header.itemId, index: chosen.rows.length, append: true };

        var rows = visibleRows(chosen);
        var index = rows.length;
        for (var j = 0; j < rows.length; ++j) {
            if (y < itemTop(rows[j]) + rows[j].height / 2) {
                index = j;
                break;
            }
        }
        return { groupId: chosen.header.itemId, index: index };
    }

    // Insertion point for a dragged group: index in the top-level ordering.
    function resolveGroupDrop(y) {
        var gs = orderedGroups();
        if (gs.length === 0)
            return null;
        for (var i = 0; i < gs.length; ++i) {
            var span = groupSpan(gs[i]);
            if (y < (span.top + span.bottom) / 2)
                return { index: i };
            if (y < span.bottom)
                return { index: i + 1 };
        }
        return { index: gs.length };
    }

    // ---------------------------------------------------------------------
    // Drag lifecycle, driven by the delegates' DragHandlers.
    // ---------------------------------------------------------------------
    function beginDrag(kind, item) {
        dragKind = kind;
        dragItem = item;
        dropTarget = null;
        dropLineVisible = false;
        dropHighlightVisible = false;
    }

    function updateDrag(scenePosition) {
        if (dragKind === "" || !dragItem)
            return;
        var local = sidebar.mapFromItem(null, scenePosition.x, scenePosition.y);
        dragProxyX = local.x + 12;
        dragProxyY = local.y - dragProxy.height / 2;
        dropTarget = dragKind === "group" ? resolveGroupDrop(local.y)
                                          : resolveSessionDrop(local.y);
        applyIndicator();
    }

    function cancelDrag() {
        dragKind = "";
        dragItem = null;
        dropTarget = null;
        dropLineVisible = false;
        dropHighlightVisible = false;
    }

    // Commit the pending drop, then clear the drag regardless of outcome. Only
    // the app.* invokable is called: the model is refreshed by the server round
    // trip, never by us.
    function endDrag() {
        var kind = dragKind;
        var item = dragItem;
        var target = dropTarget;
        cancelDrag();
        if (kind === "" || !item || !target)
            return;
        if (kind === "session")
            commitSessionDrop(item, target);
        else if (kind === "group")
            commitGroupDrop(item, target);
    }

    function commitSessionDrop(row, target) {
        if (target.groupId !== row.groupId) {
            // Cross-group: the server re-packs the target group's positions, so
            // the insertion index is the whole request.
            app.moveSession(row.itemId, target.groupId, target.index);
            return;
        }

        var entry = groupEntry(target.groupId);
        if (!entry)
            return;
        var ids = [];
        for (var i = 0; i < entry.rows.length; ++i)
            ids.push(entry.rows[i].itemId);

        var from = ids.indexOf(row.itemId);
        if (from < 0)
            return;
        // The index the user aimed at counts the dragged row; removing it first
        // shifts every later slot up by one.
        var to = target.index > from ? target.index - 1 : target.index;
        if (to === from)
            return;
        ids.splice(from, 1);
        ids.splice(to, 0, row.itemId);
        app.reorderSessions(target.groupId, ids);
    }

    function commitGroupDrop(header, target) {
        var gs = orderedGroups();
        var ids = [];
        for (var i = 0; i < gs.length; ++i)
            ids.push(gs[i].header.itemId);

        var from = ids.indexOf(header.itemId);
        if (from < 0)
            return;
        var to = target.index > from ? target.index - 1 : target.index;
        if (to === from)
            return;
        ids.splice(from, 1);
        ids.splice(to, 0, header.itemId);
        app.reorderGroups(ids);
    }

    // ---------------------------------------------------------------------
    // Drop indicator
    // ---------------------------------------------------------------------
    property bool dropLineVisible: false
    property real dropLineY: 0
    property bool dropHighlightVisible: false
    property real dropHighlightY: 0
    property real dropHighlightHeight: 0
    property real dragProxyX: 0
    property real dragProxyY: 0

    function applyIndicator() {
        var target = dropTarget;
        if (!target) {
            dropLineVisible = false;
            dropHighlightVisible = false;
            return;
        }

        if (dragKind === "group") {
            var gs = orderedGroups();
            if (gs.length === 0) {
                dropLineVisible = false;
                dropHighlightVisible = false;
                return;
            }
            dropLineY = target.index < gs.length ? groupSpan(gs[target.index]).top
                                                 : groupSpan(gs[gs.length - 1]).bottom;
            dropLineVisible = true;
            dropHighlightVisible = false;
            return;
        }

        var entry = groupEntry(target.groupId);
        if (!entry) {
            dropLineVisible = false;
            dropHighlightVisible = false;
            return;
        }

        // Appending into a collapsed group has no insertion line to draw, so
        // the whole header lights up instead.
        if (entry.header.collapsed) {
            dropHighlightY = itemTop(entry.header);
            dropHighlightHeight = entry.header.height;
            dropHighlightVisible = true;
            dropLineVisible = false;
            return;
        }

        var rows = visibleRows(entry);
        if (target.index < rows.length)
            dropLineY = itemTop(rows[target.index]);
        else if (rows.length > 0)
            dropLineY = itemTop(rows[rows.length - 1]) + rows[rows.length - 1].height;
        else
            dropLineY = itemTop(entry.header) + entry.header.height;
        dropLineVisible = true;
        dropHighlightVisible = false;
    }

    // ---------------------------------------------------------------------
    // Selection and keyboard navigation
    // ---------------------------------------------------------------------

    // Flat, visually ordered cursor list: every group header, plus the sessions
    // of expanded groups.
    function navItems() {
        var gs = orderedGroups();
        var out = [];
        for (var i = 0; i < gs.length; ++i) {
            out.push({ id: gs[i].header.itemId, isGroup: true, item: gs[i].header });
            if (gs[i].header.collapsed)
                continue;
            for (var j = 0; j < gs[i].rows.length; ++j)
                out.push({ id: gs[i].rows[j].itemId, isGroup: false, item: gs[i].rows[j] });
        }
        return out;
    }

    function setCurrent(id, isGroup) {
        currentId = id;
        currentIsGroup = isGroup;
        if (!isGroup)
            selectedSessionId = id;
    }

    function selectSession(row) {
        setCurrent(row.itemId, false);
        sidebar.forceActiveFocus();
        sidebar.sessionActivated(row.itemId);
    }

    function selectGroup(header) {
        setCurrent(header.itemId, true);
        sidebar.forceActiveFocus();
    }

    function moveSelection(delta) {
        var items = navItems();
        if (items.length === 0)
            return;
        var current = -1;
        for (var i = 0; i < items.length; ++i) {
            if (items[i].id === currentId && items[i].isGroup === currentIsGroup) {
                current = i;
                break;
            }
        }
        var next = current < 0 ? (delta > 0 ? 0 : items.length - 1)
                               : Math.max(0, Math.min(items.length - 1, current + delta));
        setCurrent(items[next].id, items[next].isGroup);
        ensureVisible(items[next].item);
    }

    function ensureVisible(item) {
        if (!item)
            return;
        var top = item.mapToItem(sessionsList.contentItem, 0, 0).y;
        var bottom = top + item.height;
        if (top < sessionsList.contentY)
            sessionsList.contentY = top;
        else if (bottom > sessionsList.contentY + sessionsList.height)
            sessionsList.contentY = bottom - sessionsList.height;
    }

    // Enter on a session is the activation the host listens for; on a group it
    // falls back to the same toggle Space performs, so the cursor is never a
    // dead end.
    function activateCurrent() {
        if (currentId === "")
            return;
        if (currentIsGroup)
            toggleCurrentGroup();
        else
            sidebar.sessionActivated(currentId);
    }

    function toggleCurrentGroup() {
        if (!currentIsGroup || currentId === "")
            return;
        var entry = groupEntry(currentId);
        if (entry)
            app.setGroupCollapsed(currentId, !entry.header.collapsed);
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Down) {
            moveSelection(1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Up) {
            moveSelection(-1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            activateCurrent();
            event.accepted = true;
        } else if (event.key === Qt.Key_Space) {
            toggleCurrentGroup();
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape && dragKind !== "") {
            cancelDrag();
            event.accepted = true;
        }
    }

    // Header bar with the compact '+' add-group action.
    Rectangle {
        id: headerBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: Theme.surfaceDeep

        Label {
            text: qsTr("Sessions")
            color: Theme.text
            font.bold: true
            font.pixelSize: Theme.fontSizeTitle
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
        }
        Button {
            id: newGroupButton
            objectName: "newGroupButton"
            text: "+"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            // A compact square rather than the old "+ New group" words: this bar
            // is 320 pixels wide at most and the words took a fifth of it. 24
            // logical pixels is the floor for a reliable pointer target on a
            // high-density display, so the box stays 24 even though the glyph in
            // it is smaller.
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            // Reachable without a pointer: Tab lands here, and the accessible
            // name below is what a screen reader announces.
            focusPolicy: Qt.StrongFocus

            // A bare "+" says nothing about what it adds, so this sentence is
            // the button's real name. The tooltip is a pointer-only hint, so the
            // SAME sentence is also the accessible name — a tooltip is never the
            // only label a control has.
            readonly property string actionText: qsTr("Add a group")

            Accessible.role: Accessible.Button
            Accessible.name: newGroupButton.actionText
            ToolTip.text: newGroupButton.actionText
            ToolTip.visible: newGroupButton.hovered
            // Long enough that crossing the bar on the way somewhere else does
            // not flash it, matching the row tooltip in SessionRow.qml.
            ToolTip.delay: 600

            contentItem: Label {
                text: newGroupButton.text
                color: newGroupButton.down ? Theme.textOnAccent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: newGroupButton.down ? Theme.accent
                     : newGroupButton.hovered ? Theme.border : Theme.surfaceRaised
                border.width: newGroupButton.visualFocus ? 2 : 1
                border.color: newGroupButton.visualFocus ? Theme.accent : Theme.border
            }
            onClicked: newGroupDialog.open()
        }
    }

    ListView {
        id: sessionsList
        objectName: "sessionsList"
        anchors.top: headerBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: statusFooter.top
        clip: true
        model: groupsDelegateModel

        ScrollBar.vertical: AppScrollBar {}
    }

    // Nothing to show. WHICH nothing matters: a fresh install has no server to
    // ask, while a connected one may simply have no groups yet, and the two
    // need different next steps.
    Column {
        id: sidebarEmptyState
        objectName: "sidebarEmptyState"
        anchors.centerIn: sessionsList
        width: sidebar.width - 40
        spacing: 8
        visible: groupsDelegateModel.count === 0

        readonly property bool serverReachable: sidebar.linkState === ""
                                                || sidebar.linkState === "connected"

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: sidebarEmptyState.serverReachable ? "\u2637" : "\u26a0"
            color: Theme.textFaint
            font.pixelSize: 26
        }
        Label {
            objectName: "sidebarEmptyTitle"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: sidebarEmptyState.serverReachable ? qsTr("No sessions yet")
                                                    : qsTr("No server")
            color: Theme.text
            font.pixelSize: Theme.fontSizeLabel
        }
        Label {
            objectName: "sidebarEmptyHint"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: sidebarEmptyState.serverReachable
                  ? qsTr("Press the \u201c+\u201d at the top of this panel to add a group, then add a Dev Session to it.")
                  : qsTr("Connect to the machine that holds your checkout, and its groups and Dev Sessions appear here.")
            color: Theme.textDim
            font.pixelSize: 11
        }
    }

    // Link status. Zero-height (and absent) when the host publishes no
    // connection surface, so a sidebar driven by a bare model is laid out
    // exactly as before.
    Rectangle {
        id: statusFooter
        objectName: "statusFooter"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: sidebar.linkState !== ""
        height: visible ? 26 : 0
        color: Theme.surfaceDeep

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.borderSubtle
        }

        Button {
            id: serverSettingsButton
            objectName: "serverSettingsButton"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Server…")
            font.pixelSize: 11
            leftPadding: 6
            rightPadding: 6

            contentItem: Label {
                text: serverSettingsButton.text
                color: Theme.text
                font: serverSettingsButton.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: serverSettingsButton.down ? Theme.border
                                                  : (serverSettingsButton.hovered ? Theme.surfaceRaised
                                                                                  : "transparent")
                radius: Theme.radiusSmall
            }
            onClicked: sidebar.serverSettingsRequested()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: serverSettingsButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            clip: true
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                // Squared off for every state that is waiting on the user, the
                // same silhouette rule ConnectSheet's chip uses.
                radius: sidebar.linkState === "hostkey" || sidebar.linkState === "credential"
                        || sidebar.linkState === "failed" ? 2 : 6
                color: sidebar.linkColor(sidebar.linkState)

                Label {
                    anchors.centerIn: parent
                    text: sidebar.linkGlyph(sidebar.linkState)
                    color: Theme.textOnAccent
                    font.pixelSize: 9
                    font.bold: true
                }
            }
            Label {
                objectName: "linkStatusLabel"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, parent.width - 18)
                text: sidebar.linkWords(sidebar.linkState)
                // The resting colour of this line is a step brighter than
                // Theme.textDim and a step below Theme.text; the theme has no
                // role for it yet, so it stays a literal.
                color: sidebar.stale ? Theme.warning : "#a6adc8"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
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
                host: sidebar
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
                        delegate: SessionRow {
                            width: sessionsColumn.width
                            host: sidebar
                        }
                    }
                }
            }
        }
    }

    // Insertion indicator: a line between rows, or a filled header when the
    // drop appends into a collapsed group. Clipped to the list viewport so a
    // target scrolled out of view cannot paint over the header bar.
    Item {
        id: dropOverlay
        anchors.fill: sessionsList
        clip: true
        z: 5

        Rectangle {
            id: dropHighlight
            objectName: "dropHighlight"
            visible: sidebar.dropHighlightVisible
            x: 0
            y: sidebar.dropHighlightY - dropOverlay.y
            width: dropOverlay.width
            height: sidebar.dropHighlightHeight
            // A violet-tinted wash used nowhere else; the theme has no role for
            // it yet, so it stays a literal.
            color: "#302a4a"
            border.color: Theme.accent
            border.width: 1
        }

        Rectangle {
            id: dropLine
            objectName: "dropIndicator"
            visible: sidebar.dropLineVisible
            x: 0
            y: sidebar.dropLineY - dropOverlay.y - height / 2
            width: dropOverlay.width
            height: 2
            color: Theme.accent
        }
    }

    // Small label following the cursor so the drag reads as a drag even though
    // the source row stays put.
    Rectangle {
        id: dragProxy
        objectName: "dragProxy"
        visible: sidebar.dragKind !== "" && sidebar.dragItem !== null
        x: sidebar.dragProxyX
        y: sidebar.dragProxyY
        width: Math.min(sidebar.width - 24, dragProxyLabel.implicitWidth + 16)
        height: dragProxyLabel.implicitHeight + 8
        radius: Theme.radiusSmall
        color: Theme.surfaceRaised
        border.color: Theme.accent
        border.width: 1
        opacity: 0.9
        z: 7

        Label {
            id: dragProxyLabel
            anchors.centerIn: parent
            width: parent.width - 16
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideRight
            text: sidebar.dragItem ? sidebar.dragItem.name : ""
        }
    }

    AppDialog {
        id: newGroupDialog
        objectName: "newGroupDialog"
        title: qsTr("New group")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        // `Dialog` defaults to a 320-pixel frame, but this 300-pixel field
        // needs the 12-pixel padding on both sides too.
        width: newGroupField.Layout.preferredWidth + leftPadding + rightPadding

        // Reset the field each open so a cancelled edit doesn't resurface as
        // stale text next time (imperative input breaks the initial binding),
        // then SELECT it: assigning `text` leaves the cursor at the end, so a
        // user who opens this and types "api" was getting "New groupapi".
        // Selecting the placeholder default makes the first keystroke replace
        // it, which is what every other pre-filled field here now does too.
        onOpened: {
            newGroupField.text = qsTr("New group");
            newGroupField.forceActiveFocus();
            newGroupField.selectAll();
        }

        // A Dialog sizes itself from its content item's IMPLICIT width, and a
        // bare TextField with an explicit `width` still reports a much smaller
        // implicit width — so the field used to spill out past the dialog's
        // edge. A ColumnLayout does report the preferred widths of its children,
        // which is the convention newSessionDialog below already uses.
        ColumnLayout {
            // Layout.preferredWidth sizes the child only; Dialog measures this
            // layout's implicit width. Keep both values explicit so the frame
            // includes its 12-pixel padding on either side.
            implicitWidth: 300
            spacing: 8

            TextField {
                id: newGroupField
                objectName: "newGroupField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Group name")
                text: qsTr("New group")
            }
        }

        onAccepted: {
            var groupName = newGroupField.text.length > 0
                            ? newGroupField.text : qsTr("New group");
            app.createGroup(groupName);
        }
    }

    // Which group the pending session belongs to. Captured when the action is
    // invoked rather than read back from the selection, so a click elsewhere
    // while the dialog is open cannot retarget the creation.
    property string pendingSessionGroupId: ""

    function requestNewSession(groupId) {
        sidebar.pendingSessionGroupId = groupId;
        newSessionDialog.open();
    }

    AppDialog {
        id: newSessionDialog
        objectName: "newSessionDialog"
        // Size the frame around the fixed-width content rather than letting the
        // Basic-style 320-pixel default clip its horizontal padding.
        width: newSessionField.Layout.preferredWidth + leftPadding + rightPadding
        title: qsTr("New Dev Session")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        // Same rule as the group dialog: pre-filled name, selected so typing
        // replaces it instead of appending to it.
        onOpened: {
            newSessionField.text = qsTr("New session");
            newSessionRepoField.text = "";
            newSessionField.forceActiveFocus();
            newSessionField.selectAll();
        }

        ColumnLayout {
            // See the group dialog above: Dialog uses the content layout's
            // implicit width, not the children's Layout.preferredWidth.
            implicitWidth: 300
            spacing: 8

            TextField {
                id: newSessionField
                objectName: "newSessionField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Session name")
            }

            // The repository root is not decoration: it becomes the working
            // directory of every terminal in the session and the root the
            // viewers browse, so a session without one opens in the remote
            // home directory instead of the project.
            TextField {
                id: newSessionRepoField
                objectName: "newSessionRepoField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Repository path on the server, e.g. /srv/repos/app")
            }

            Label {
                Layout.preferredWidth: 300
                text: qsTr("Terminals in this session start in the repository path.")
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }

        onAccepted: {
            const name = newSessionField.text.length > 0
                       ? newSessionField.text : qsTr("New session");
            app.createSession(sidebar.pendingSessionGroupId, name,
                              newSessionRepoField.text);
            sidebar.pendingSessionGroupId = "";
        }
        onRejected: sidebar.pendingSessionGroupId = ""
    }
}
